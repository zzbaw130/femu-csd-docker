#include "graph_state.h"
#include "graph_batch.h"

/* Before its LOAD block can be recycled, replace output-only byte views
 * by immutable file offsets. The planner excludes every comparison/key use. */
static int defer_output_vectors(struct execution *s,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[b->scan];
    if(n->kind!=QP_SCAN || !n->late_text || b->joined || b->sorted || b->group_source) return 0;
    const struct operator_state *scan=&s->nodes[b->scan];
    const unsigned char *raw=s->arena+input_offset(s,n)+scan->scan_raw;
    for(unsigned i=0;i<b->plan.nexprs;++i) {
        const struct qs_expr *e=&b->plan.exprs[i]; struct qs_vector *v=&b->vectors[i];
        if(!b->needed[i] || e->op!=Q_COLUMN || e->type!=Q_STRING || !(n->late_text&(UINT64_C(1)<<e->column)) || v->kind!=QS_TEXT) continue;
        for(unsigned r=0;r<b->count;++r) {
            struct qs_text_view view=v->strings[r]; __int128 value=0;
            if(!v->nulls[r] && !v->errors[r]) {
                if(view.id || (uintptr_t)view.data<(uintptr_t)raw || view.length>scan->scan_raw_bytes ||
                   (uintptr_t)view.data-(uintptr_t)raw>scan->scan_raw_bytes-view.length) return -5;
                uint64_t offset=scan->position-scan->scan_raw_bytes+((uintptr_t)view.data-(uintptr_t)raw);
                value=qs_late_text(b->scan,offset,view.length);
            }
            v->values[r]=value;
        }
        v->kind=QS_WIDE; v->materialized=b->count;
    }
    return 0;
}
int join_condition(struct execution *s,unsigned id,const struct qs_cell *values,struct qs_cell *match)
{
    const struct qp_node *n=&s->plan->nodes[id];
    if(!s->text) {
        struct qs_eval e; init_eval(&e,&n->expr,values,NULL);
        return evaluate(s,&e,n->join_qual,match);
    }
    struct operator_state *o=&s->nodes[id];
    if(!o->join_condition) o->join_condition=cs_work_calloc(s->context,1,sizeof(*o->join_condition));
    struct join_condition_state *q=o->join_condition; if(!q) return -4;
    if(!q->active) {
        memcpy(q->input,values,n->expr.nattrs*sizeof(*values));
        init_eval(&q->evaluation,&n->expr,q->input,NULL); q->active=1;
    }
    int status=evaluate(s,&q->evaluation,n->join_qual,match);
    if(!status) q->active=0;
    return status;
}
static bool projection_dependency(const struct qs_plan *p,int index,int dependency)
{
    if(dependency<0 || (unsigned)dependency>=p->nexprs) return false;
    if(dependency<index) return true;
    unsigned op=p->exprs[dependency].op;
    return op==Q_COLUMN || op==Q_CONST || op==QP_PARAMETER;
}
int projection_import(struct projection_import *in,unsigned node,int index)
{
    const struct qp_node *n=&in->execution->plan->nodes[node];
    if(index<0 || (unsigned)index>=n->expr.nexprs) return -1;
    struct qs_plan *p=&in->batch->plan;
    for(unsigned i=0;i<p->nexprs;++i) if(in->nodes[i]==node && in->expressions[i]==(unsigned)index) return i;
    struct qs_expr e=n->expr.exprs[index];
    bool edge=in->batch->input_edge && node==in->batch->scan;
    if(edge) {
        unsigned column=0;
        while(column<n->expr.nout && n->expr.outputs[column].index!=index) ++column;
        if(column==n->expr.nout) return -1;
        e.op=Q_COLUMN; e.column=column;
    } else if(in->batch->group_source && node==in->batch->scan) {
        /* Source expressions reference finalized group keys and aggregate
         * values, never the original aggregate input below this boundary. */
        if(e.op==Q_AGGREF) {
            if(e.column<0 || (unsigned)e.column>=n->expr.naggs) return -1;
            e.op=Q_COLUMN; e.column+=n->nkeys;
        } else if(e.op==Q_COLUMN) {
            unsigned k=0; while(k<n->nkeys && n->left_keys[k]!=e.column) ++k;
            if(k==n->nkeys) return -1;
            e.column=k;
        }
    }
    if(e.op>Q_CASE && e.op!=Q_YEAR && e.op!=Q_LIKE && e.op!=Q_SUBSTRING && e.op!=QP_PARAMETER && e.op!=QP_COALESCE && e.op!=Q_DIV && e.op!=QP_INSET) return -1;
    if(e.op==Q_COLUMN && (n->kind==QP_PROJECT || n->kind==QP_AGGREGATE) &&
       !edge && !(in->batch->group_source && node==in->batch->scan)) {
        const struct qp_node *child=&in->execution->plan->nodes[n->left];
        if(e.column<0 || (unsigned)e.column>=child->expr.nout) return -1;
        return projection_import(in,n->left,child->expr.outputs[e.column].index);
    }
    if(e.op==QP_PARAMETER) {
        if(e.column<0 || e.column>=QP_PARAMS) return -1;
        e.scale=in->execution->params[e.column].scale;
        e.dscale=in->execution->params[e.column].dscale;
    }
    if(e.op!=Q_COLUMN && e.op!=Q_CONST && e.op!=QP_PARAMETER) {
        if(!projection_dependency(&n->expr,index,e.left)) return -1;
        e.left=projection_import(in,node,e.left); if(e.left<0) return -1;
        if(e.op!=Q_NOT && e.op!=Q_IS_NULL && e.op!=Q_IS_NOT_NULL && e.op!=Q_YEAR && e.op!=QP_INSET) {
            if(!projection_dependency(&n->expr,index,e.right)) return -1;
            e.right=projection_import(in,node,e.right); if(e.right<0) return -1;
        }
        if(e.op==Q_CASE || e.op==Q_SUBSTRING) {
            if(!projection_dependency(&n->expr,index,e.otherwise)) return -1;
            e.otherwise=projection_import(in,node,e.otherwise); if(e.otherwise<0) return -1;
        }
    }
    /* PG numeric expressions carry a generic declared scale. The imported
     * vector DAG needs the actual scale of its rebound children, just as the
     * scalar evaluator derives it from runtime values. */
    if(e.op==Q_ADD || e.op==Q_SUB || e.op==Q_MUL || e.op==Q_CASE) {
        unsigned left=p->exprs[e.left].scale,right=p->exprs[e.right].scale;
        unsigned scale=e.op==Q_MUL?left+right:left>right?left:right;
        if(e.op==Q_CASE) scale=right>p->exprs[e.otherwise].scale?right:p->exprs[e.otherwise].scale;
        if(scale>UINT8_MAX) return -1;
        e.scale=scale;
    }
    if(e.op==Q_CONST && e.type==Q_STRING && !e.isnull) {
        if(e.constant<0 || e.constant>=n->expr.nstrings) return -1;
        const struct qs_string *v=&n->expr.strings[(unsigned)e.constant];
        if(in->execution->paged) {
            unsigned id=in->execution->literal_ids?in->execution->literal_ids[node*QS_STRINGS+(unsigned)e.constant]:0;
            if(!id) return -1;
            e.constant=id;
        } else {
            int id=qs_dict_insert(in->execution->dictionary,v->bytes,v->length); if(id<0) return -1;
            e.constant=id;
        }
    }
    if(p->nexprs==QS_EXPRS) return -1;
    unsigned at=p->nexprs++; p->exprs[at]=e; in->nodes[at]=node; in->expressions[at]=index;
    return at;
}
/* Keep condition dependencies and, for Index consumption, the columns needed
 * by the output DAG. Both consumers use the same query-budgeted vector state. */
int join_qual_init(struct execution *s,unsigned id,struct projection_batch *b,
    const struct projection_batch *output)
{
    const struct qp_node *n=&s->plan->nodes[id];
    b->plan=n->expr; b->plan.nexprs=b->plan.nout=b->plan.ncols=0;
    struct projection_import import={.execution=s,.batch=b};
    int predicate=projection_import(&import,id,n->join_qual);
    if(predicate<0 || qs_vector_require(&b->plan,predicate,b->needed)) return -1;
    b->plan.predicate=predicate;
    if(output) b->plan.ncols=output->plan.ncols;
    for(unsigned col=0;col<b->plan.nattrs;++col)
        b->plan.cols[col].slot=output?output->plan.cols[col].slot:-1;
    for(unsigned i=0;i<b->plan.nexprs;++i) if(b->plan.exprs[i].op==Q_COLUMN) {
        int col=b->plan.exprs[i].column;
        if(col<0 || (unsigned)col>=b->plan.nattrs) return -1;
        if(b->plan.cols[col].slot<0) b->plan.cols[col].slot=b->plan.ncols++;
    }
    if(b->plan.ncols) b->cells=cs_work_alloc(s->context,(size_t)b->plan.ncols*QS_VECTOR_ROWS*sizeof(*b->cells));
    b->vectors=cs_work_alloc(s->context,b->plan.nexprs*sizeof(*b->vectors));
    if((b->plan.ncols && !b->cells) || !b->vectors) {
        cs_work_free(s->context,b->cells); cs_work_free(s->context,b->vectors);
        cs_work_free(s->context,b->group_input); return -1;
    }
    return 0;
}

const struct graph_projection_ops graph_project_ops={
    .prepare=projection_init,.retry=retry_project
};

static int project_filtered_impl(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row,int filtered,struct qs_eval *e)
{
    struct qs_cell keep;
    if(!filtered && n->expr.predicate>=0) {
        if(!e->plan) init_eval(e,&n->expr,input,aggregates);
        if(evaluate(s,e,n->expr.predicate,&keep)) return -5;
        if(keep.isnull || !keep.value) return 0;
    }
    for(unsigned i=0;i<n->expr.nout;++i) {
        int index=n->expr.outputs[i].index; const struct qs_expr *x=&n->expr.exprs[index];
        if(input && x->op==Q_COLUMN) { row[i]=input[x->column]; continue; }
        if(!e->plan) init_eval(e,&n->expr,input,aggregates);
        if(evaluate(s,e,index,&row[i])) return -5;
    }
    return 1;
}

int retry_project(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id];
    int count=project_filtered(s,n,NULL,NULL,row,0);
    if(count<0) return count;
    if(!count) return 3;
    if(n->kind==QP_SCAN) ++s->metrics.matched;
    else if(n->kind==QP_SORT || n->kind==QP_LIMIT || n->kind==QP_CTE_SCAN || (n->kind==QP_PROJECT && n->left>=0))
        ++s->nodes[id].emitted;
    return 2;
}

int project_filtered(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row,int filtered)
{
    struct operator_state *o=&s->nodes[n-s->plan->nodes];
    struct pending_projection *p=o->project_retry;
    if(p) { input=p->has_input?p->input:NULL; aggregates=p->has_aggregates?p->aggregates:NULL; filtered=p->filtered; }
    struct qs_eval e; if(p) e=p->evaluation; else e.plan=NULL;
    int count=project_filtered_impl(s,n,input,aggregates,row,filtered,&e);
    if(count<0 && s->expression_wait) {
        if(!p) {
            p=&s->projection_retry; memset(p,0,sizeof(*p));
            p->has_input=input!=NULL; p->has_aggregates=aggregates!=NULL; p->filtered=filtered;
            for(unsigned i=0;i<s->plan->count;++i) if(s->nodes[i].index_active &&
                (&s->plan->nodes[i]==n || s->plan->nodes[i].right==n-s->plan->nodes)) p->inline_only=1;
            if(input) memcpy(p->input,input,n->expr.nattrs*sizeof(*input));
            if(aggregates) memcpy(p->aggregates,aggregates,n->expr.naggs*sizeof(*aggregates));
            o->project_retry=p;
        }
        p->evaluation=e; p->evaluation.tuple=p->has_input?p->input:NULL;
        p->evaluation.aggregates=p->has_aggregates?p->aggregates:NULL;
    } else if(p) o->project_retry=NULL;
    return count;
}

int project(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row)
{ return project_filtered(s,n,input,aggregates,row,0); }

static int64_t projection_edge_fill(struct execution *s,struct projection_batch *b)
{
    unsigned count=0;
    if(s->stream_edge) {
        struct projection_batch *edge=s->stream_edge;
        count=edge->count-edge->position;
        if(!count) { s->scan_drained=true; return QP_WAIT; }
        for(unsigned col=0;col<b->plan.nattrs;++col) {
            int slot=b->plan.cols[col].slot;
            if(slot<0) continue;
            unsigned expr=edge->plan.outputs[col].index;
            for(unsigned r=0;r<count;++r) {
                if(edge->vectors[expr].errors[edge->position+r]) return -5;
                b->cells[slot*QS_VECTOR_ROWS+r]=qs_vector_cell(&edge->plan,edge->vectors,expr,edge->position+r);
            }
        }
        edge->position+=count;
    } else if(s->block_input) {
        count=b->input_count;
        for(;count<QS_VECTOR_ROWS;++count) {
            struct qs_cell row[QS_OUT];
            int64_t status=block_edge_row(s,b->scan,row);
            if(status==QP_WAIT) break;
            if(status!=2) { b->input_count=count; return status; }
            for(unsigned col=0;col<b->plan.nattrs;++col) {
                int slot=b->plan.cols[col].slot;
                if(slot>=0) b->cells[slot*QS_VECTOR_ROWS+count]=row[col];
            }
        }
        b->input_count=0;
        if(!count) return QP_WAIT;
    } else {
        const struct qs_result *in=s->hash_input;
        count=in->rows-s->hash_input_position;
        if(!count) return in->finished?0:QP_WAIT;
        if(count>QS_VECTOR_ROWS) count=QS_VECTOR_ROWS;
        unsigned cols=b->plan.nattrs;
        const struct qs_cell *rows=(const struct qs_cell *)in->data+
            (size_t)(in->begin+s->hash_input_position)*cols;
        for(unsigned col=0;col<cols;++col) {
            int slot=b->plan.cols[col].slot;
            if(slot<0) continue;
            for(unsigned r=0;r<count;++r) b->cells[slot*QS_VECTOR_ROWS+r]=rows[(size_t)r*cols+col];
        }
        s->hash_input_position+=count;
    }
    b->count=count; b->position=0;
    struct qs_vector_input input={.cells=b->cells,.dictionary=s->dictionary,
        .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    return projection_execute(b,NULL,NULL,0,input)?-5:2;
}

static int64_t projection_heap_fill(struct execution *s,struct projection_batch *b)
{
#ifdef QP_SPLIT_RUNTIME
    struct operator_state *state=&s->nodes[b->scan];
    int64_t status=state->read_chunk?state->read_chunk(s,b->scan):-1;
#else
    int64_t status=scan_chunk_next(s,b->scan);
#endif
    if(status!=2 || (b->filtered_scan && !s->nodes[b->scan].scan_filter)) return status;
    struct operator_state *scan=&s->nodes[b->scan];
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,&s->plan->nodes[b->scan]));
    b->count=chunk->selected-scan->scan_position; if(b->count>QS_VECTOR_ROWS) b->count=QS_VECTOR_ROWS;
    const struct qp_node *source=&s->plan->nodes[b->scan];
    if(s->paged || (scan->defer_strings && source->expr.predicate<0))
        for(unsigned col=0;col<b->plan.nattrs;++col) if(b->plan.cols[col].type==Q_STRING)
            b->plan.cols[col].deferred=scan->scan_filter?scan->scan_filter->cols[col].deferred:QS_EAGER;
    const void *raw=scan->scan_raw_bytes?s->arena+input_offset(s,source)+scan->scan_raw:NULL;
    struct qs_vector_input input={.raw=raw,.raw_bytes=scan->scan_raw_bytes,.dictionary=s->dictionary,
        .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    const uint32_t *selection=chunk->selected==chunk->rows?NULL:qs_selection(chunk);
    b->input_cursor=&scan->scan_position;
    if(projection_execute(b,chunk,selection,scan->scan_position,input)) return -5;
    b->position=0;
    return 2;
}

#ifndef QP_SPLIT_RUNTIME
static int64_t projection_index_fill(struct execution *s,struct projection_batch *b)
{ return index_next(s,b->scan,NULL,b); }
#endif

int projection_init(struct execution *s,unsigned id,int source)
{
    struct operator_state *o=&s->nodes[id];
    struct projection_batch **slot=source?&o->source_projection:&o->projection;
    int *checked=source?&o->source_checked:&o->projection_checked;
    unsigned *deferred=source?&o->source_deferred:&o->projection_deferred;
    if(*slot || (*checked && (!*deferred || s->plan->nodes[*deferred-1].parallel ||
        projection_pending(s->nodes[*deferred-1].source_projection)))) return 0;
    *checked=1; *deferred=0;
    const struct qp_node *root=&s->plan->nodes[id];
    int aggregate=root->kind==QP_AGGREGATE && !source;
    if(aggregate && (root->distinct || root->distinct_aggs || root->left<0 ||
        (root->nkeys && ((!root->hash && !root->sorted_input) || !qp_hash_aggregate_supported(root))))) return 0;
    int grouped=aggregate && root->nkeys;
    int scan=aggregate?root->left:(int)id;
    unsigned projections[QP_NODES],nprojections=0;
    while(!graph_input_edge(s,scan) && s->plan->nodes[scan].kind==QP_PROJECT) {
        const struct qp_node *n=&s->plan->nodes[scan];
        if(n->left<0) return 0;
        if(!s->hash_input && !s->worker && s->plan->workers>1 && n->parallel) { *deferred=scan+1; return 0; }
        projections[nprojections++]=scan;
        scan=n->left;
    }
    const struct qp_node *leaf=&s->plan->nodes[scan];
    bool edge=graph_input_edge(s,scan);
    int group_source=!edge && leaf->kind==QP_AGGREGATE && (!s->worker || (s->source_node==scan &&
        s->owner->partition_join<0 && (s->owner->nodes[scan].group_phase>=2 || s->owner->nodes[scan].group_radix)));
    bool cte=!edge && leaf->kind==QP_CTE_SCAN && s->worker && s->stream_block;
    int joined=!edge && (leaf->kind==QP_INDEX_JOIN || leaf->kind==QP_JOIN);
    int sorted=!edge && leaf->kind==QP_SORT && leaf->left>=0 &&
        leaf->expr.nattrs==s->plan->nodes[leaf->left].expr.nout;
    if(leaf->kind!=QP_SCAN && !edge && !joined && !sorted && !group_source && !cte) return 0;
    if(!s->hash_input && !s->worker && s->plan->workers>1 && leaf->parallel) { *deferred=scan+1; return 0; }
    if(group_source && !s->worker && projection_pending(s->nodes[scan].source_projection)) {
        *deferred=scan+1; return 0;
    }
#ifdef QP_SPLIT_RUNTIME
    graph_projection_reader reader=edge?projection_edge_fill:leaf->kind==QP_SCAN?projection_heap_fill:
        group_source?graph_state_owner(s,scan)->read_projection:s->nodes[scan].read_projection;
    if(!reader) { *checked=0; return 0; }
#else
    graph_projection_reader reader=edge?projection_edge_fill:group_source?projection_group_fill:sorted?projection_sort_fill:
        cte?projection_cte_fill:leaf->kind==QP_INDEX_JOIN?projection_index_fill:
        joined?projection_join_fill:projection_heap_fill;
#endif
    if(!edge && !group_source && !joined && !sorted && !leaf->ordered_index && !cte && leaf->expr.predicate>=0) {
        int status=scan_filter_init(s,&leaf->expr,&s->nodes[scan]); if(status) return status;
    }
    struct projection_batch *b=cs_work_calloc(s->context,1,sizeof(*b)); if(!b) return 0;
    b->plan=leaf->expr; b->plan.nexprs=0; b->plan.nout=grouped?s->plan->nodes[root->left].expr.nout:
        aggregate?root->expr.naggs:root->expr.nout; b->scan=scan; b->joined=joined; b->sorted=sorted;
    if(leaf->kind==QP_SCAN && leaf->late_text && !joined && !sorted)
        b->finish_vectors=defer_output_vectors;
    b->group_source=group_source; b->input_edge=edge;
    b->filtered_scan=!edge && !joined && !sorted && !group_source && s->nodes[scan].scan_filter &&
        s->nodes[scan].scan_filter->predicate>=0;
    if(edge) b->plan.nattrs=leaf->expr.nout;
    else if(group_source) b->plan.nattrs=leaf->nkeys+leaf->expr.naggs;
    if(sorted) {
        b->plan.ncols=b->plan.nattrs;
        for(unsigned col=0;col<b->plan.nattrs;++col) b->plan.cols[col].slot=col;
    }
    struct projection_import import={.execution=s,.batch=b};
    /* Apply each node's WHERE/HAVING in input-to-output order. A NULL result
     * removes the row before any higher filter or projection is evaluated. */
    for(unsigned i=0;i<=nprojections;++i) {
        unsigned node=i?projections[nprojections-i]:(unsigned)scan;
        if(!i && (edge || b->filtered_scan)) continue; /* Producer already filters. */
        int predicate=s->plan->nodes[node].expr.predicate;
        if(predicate<0) continue;
        int index=projection_import(&import,node,predicate);
        if(index<0 || qs_vector_dependency(&b->plan,index,b->needed)) { cs_work_free(s->context,b); return 0; }
        b->filters[b->nfilters].node=node; b->filters[b->nfilters++].expression=index;
    }
    if(grouped) {
        for(unsigned k=0;k<root->nkeys;++k) {
            int index=projection_import(&import,id,root->expr.keys[k]);
            if(index<0 || qs_vector_require(&b->plan,index,b->needed)) { cs_work_free(s->context,b); return 0; }
            b->group_keys[k]=index;
        }
        for(unsigned a=0;a<root->expr.naggs;++a) {
            b->group_values[a]=-1;
            if(root->expr.aggs[a].kind==Q_COUNT_STAR) continue;
            int index=projection_import(&import,id,root->expr.aggs[a].expr);
            if(index<0 || qs_vector_require(&b->plan,index,b->needed)) { cs_work_free(s->context,b); return 0; }
            b->group_values[a]=index;
        }
    }
    for(unsigned i=0;i<b->plan.nout;++i) {
        if(aggregate && !grouped && root->expr.aggs[i].kind==Q_COUNT_STAR) { b->plan.outputs[i].index=-1; continue; }
        const struct qp_node *source=grouped?&s->plan->nodes[root->left]:root;
        int index=projection_import(&import,grouped?(unsigned)root->left:id,
            aggregate && !grouped?root->expr.aggs[i].expr:source->expr.outputs[i].index);
        if(index<0 || qs_vector_require(&b->plan,index,b->needed)) { cs_work_free(s->context,b); return 0; }
        b->plan.outputs[i]=source->expr.outputs[i]; b->plan.outputs[i].index=index;
    }
    if(joined || group_source || edge || cte) {
        for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_INDEX_JOIN)
            for(int at=s->plan->nodes[i].left;at>=0;at=s->plan->nodes[at].left)
                if(at==scan) { b->keep_window=1; break; }
        b->plan.ncols=0;
        for(unsigned col=0;col<b->plan.nattrs;++col) b->plan.cols[col].slot=-1;
        for(unsigned i=0;i<b->plan.nexprs;++i) if(b->plan.exprs[i].op==Q_COLUMN) {
            int column=b->plan.exprs[i].column;
            if(column<0 || (unsigned)column>=b->plan.nattrs) { cs_work_free(s->context,b); return 0; }
            struct qs_column *col=&b->plan.cols[column];
            if(col->slot<0) col->slot=b->plan.ncols++;
        }
        if(b->plan.ncols) b->cells=cs_work_alloc(s->context,(size_t)b->plan.ncols*QS_VECTOR_ROWS*sizeof(*b->cells));
        if(b->plan.ncols && !b->cells) { cs_work_free(s->context,b); return 0; }
    }
    if(b->plan.nexprs) b->vectors=cs_work_alloc(s->context,b->plan.nexprs*sizeof(*b->vectors));
    if(b->plan.nexprs && !b->vectors) { cs_work_free(s->context,b->cells); cs_work_free(s->context,b); return 0; }
    /* Once this vector DAG is known to fit, let it filter raw Heap text.
     * Rejected values never enter the dictionary. A negative predicate on
     * this decoder copy means the DAG, rather than the flat Filter CSF,
     * owns filtering and the resumable materialization of survivors. */
    if(s->paged && !edge && !joined && !sorted && !group_source && !cte && !leaf->ordered_index &&
       !s->nodes[scan].scan_filter) {
        int text=0;
        for(unsigned col=0;col<leaf->expr.nattrs;++col) text|=leaf->expr.cols[col].type==Q_STRING;
        if(text) {
            struct qs_plan *decoder=cs_work_alloc(s->context,sizeof(*decoder));
            if(decoder) {
                *decoder=leaf->expr; decoder->predicate=-1;
                for(unsigned col=0;col<decoder->nattrs;++col) if(decoder->cols[col].type==Q_STRING && decoder->cols[col].slot>=0)
                    decoder->cols[col].deferred=QS_DEFERRED;
                s->nodes[scan].scan_filter=decoder;
            }
        }
    }
    b->read_input=reader;
    *slot=b; return 0;
}
int64_t graph_stream_project(struct execution *s,unsigned scan,unsigned id)
{
    int64_t status=graph_attach_stream_input(s,scan,s->plan->nodes[id].left);
    if(status) return status;
    status=projection_init(s,id,0);
    if(!status) status=s->nodes[id].projection?projection_fill(s,id,0):-4;
    if(status==2) s->nodes[id].published_batch=&s->nodes[id].projection;
    if(status==QP_WAIT && s->control->index_nreads && !s->control->ready) {
        int64_t request=namespace_wait(s);
        status=request?request:-1;
    } else if((status==QP_WAIT || !status) && s->scan_drained) {
        s->nodes[id].stream_inlet=false;
        status=graph_consume_input();
    } else if(status==3 || !status) status=-4;
    s->stream_edge=NULL;
    return status;
}


static void defer_probe_numbers(struct execution *s,const struct qp_node *join,struct projection_batch *b)
{
    const struct qp_node *scan=&s->plan->nodes[join->left];
    if(scan->kind!=QP_SCAN || scan->ordered_index || b->nfilters ||
       (scan->expr.predicate>=0 && !b->filtered_scan)) return;
    /* The raw input can only be borrowed for direct Heap column outputs.
     * Keep a number eager if any output of that column is a join key. */
    uint64_t keys=0;
    for(unsigned i=0;i<b->plan.nout;++i)
        if(b->plan.exprs[b->plan.outputs[i].index].op!=Q_COLUMN) return;
    for(unsigned k=0;k<join->nkeys;++k) {
        int index=b->plan.outputs[join->left_keys[k]].index;
        keys|=UINT64_C(1)<<b->plan.exprs[index].column;
    }
    unsigned numbers=0;
    for(unsigned i=0;i<b->plan.nout;++i) {
        const struct qs_expr *e=&b->plan.exprs[b->plan.outputs[i].index];
        if(e->type==Q_DECIMAL && b->plan.cols[e->column].type==Q_DECIMAL &&
           !(keys&(UINT64_C(1)<<e->column))) numbers|=1U<<i;
    }
    if(!numbers) return;
    struct operator_state *o=&s->nodes[join->left];
    if(!o->scan_filter) {
        if(scan_filter_init(s,&scan->expr,o)) return;
        if(!o->scan_filter) {
            o->scan_filter=cs_work_alloc(s->context,sizeof(*o->scan_filter));
            if(!o->scan_filter) return;
            *o->scan_filter=scan->expr;
        }
    }
    o->scan_filter_checked=1;
    b->deferred_numbers=numbers;
    for(;numbers;numbers&=numbers-1) {
        unsigned i=__builtin_ctz(numbers);
        int index=b->plan.outputs[i].index,column=b->plan.exprs[index].column;
        /* Filter can evaluate a raw value for its predicate without replacing
         * the reference in the chunk. The join still owns this LOAD when a
         * matching row requests the projected value. */
        o->scan_filter->cols[column].deferred=QS_FILTER_ONLY;
        b->plan.cols[column].deferred=QS_DEFERRED;
        b->needed[index]&=~2; /* Initialize a raw view, evaluate it on demand. */
    }
}

int hash_probe_projection(struct execution *s,unsigned id,int source)
{
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->nodes[id];
    int status=projection_init(s,n->left,source); if(status) return status;
    struct projection_batch *b=source?s->nodes[n->left].source_projection:s->nodes[n->left].projection;
    if(b && !o->hash_probe) {
        o->hash_probe=cs_work_calloc(s->context,1,sizeof(*o->hash_probe));
        if(o->hash_probe) {
            b->lazy_text=1;
            const struct qp_node *source=&s->plan->nodes[b->scan];
            if(source->kind==QP_SCAN && !source->ordered_index && source->expr.predicate<0)
                for(unsigned col=0;col<source->expr.nattrs;++col)
                    if(source->expr.cols[col].slot>=0 && source->expr.cols[col].type==Q_STRING)
                        s->nodes[b->scan].defer_strings=1;
            defer_probe_numbers(s,n,b);
        }
    }
    return 0;
}

