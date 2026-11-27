#include "graph_state.h"
#include "graph_batch.h"

#define QP_INDEX_YIELD 4

static _Thread_local struct execution *index_execution;
static _Thread_local unsigned index_node;
static _Thread_local struct projection_batch *index_columns;


static void index_projection_init(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    int useful=right->expr.predicate>=0;
    for(unsigned i=0;i<right->expr.nout;++i)
        useful|=right->expr.exprs[right->expr.outputs[i].index].op!=Q_COLUMN;
    if(!useful) return;
    struct projection_batch *b=cs_work_calloc(s->context,1,sizeof(*b)); if(!b) return;
    b->plan=right->expr; b->plan.nexprs=0; b->plan.predicate=-1; b->plan.ncols+=2;
    for(unsigned i=0;i<b->plan.nattrs;++i) if(b->plan.cols[i].slot>=0) b->plan.cols[i].slot+=2;
    struct projection_import import={.execution=s,.batch=b};
    for(unsigned i=0;i<b->plan.nout;++i) {
        int index=projection_import(&import,n->right,right->expr.outputs[i].index);
        if(index<0 || qs_vector_require(&b->plan,index,b->needed)) goto fallback;
        b->plan.outputs[i].index=index;
    }
    /* Initialize output dependencies but evaluate them only for survivors. */
    for(unsigned i=0;i<b->plan.nexprs;++i) b->needed[i]&=1;
    if(right->expr.predicate>=0) {
        int index=projection_import(&import,n->right,right->expr.predicate);
        if(index<0 || qs_vector_require(&b->plan,index,b->needed)) goto fallback;
        b->plan.predicate=index;
    }
    b->vectors=cs_work_alloc(s->context,b->plan.nexprs*sizeof(*b->vectors));
    if(!b->vectors) goto fallback;
    s->nodes[id].index_projection=b; return;
fallback:
    cs_work_free(s->context,b);
}

static int index_project_batch(struct execution *s,struct projection_batch *b,struct qs_chunk *chunk)
{
    if(b->materializing) return materialize_outputs(s,b);
    b->count=chunk->selected;
    struct qs_vector_input input={.dictionary=s->dictionary,.parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    if(s->paged) { input.text=s->text; input.text_io=s->io; input.resume=b->retry; }
    int status=0;
    if(qs_vector_execute_input(&b->plan,chunk,qs_selection(chunk),0,chunk->selected,b->needed,b->vectors,input)) { status=-5; goto finished; }
    uint16_t active[QS_VECTOR_ROWS]; unsigned count=0;
    for(unsigned r=0;r<chunk->selected;++r) {
        int index=b->plan.predicate;
        if(index>=0) {
            if(b->vectors[index].errors[r]) continue;
            struct qs_cell keep=qs_vector_cell(&b->plan,b->vectors,index,r);
            if(keep.isnull || !keep.value) continue;
        }
        active[count++]=r;
    }
    input.plan=&b->plan; input.vectors=b->vectors; input.count=chunk->selected;
    for(unsigned i=0;i<b->plan.nout;++i)
        if(qs_vector_evaluate(&input,b->plan.outputs[i].index,active,count)) { status=-5; break; }
finished:
    if(status && input.text) text_status(s,qs_dictionary_text_request(input.text));
    b->retry=status && s->expression_wait;
    if(status) return status;
    return s->paged?materialize_outputs(s,b):0;
}

static int index_join_values(struct execution *s,unsigned id,struct qs_chunk *chunk,unsigned r,
    const struct projection_batch *projected,struct qs_cell *values)
{
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    struct operator_state *o=&s->nodes[id]; unsigned ln=s->plan->nodes[n->left].expr.nout;
    unsigned at=qs_selection(chunk)[r]; if(at>=chunk->rows) return -1;
    unsigned source=qs_values(chunk)[chunk->capacity+at]; if(source>=o->index_count) return -1;
    memcpy(values,o->index_left+(size_t)source*ln,ln*sizeof(*values));
    if(projected) {
        int predicate=projected->plan.predicate;
        if(predicate>=0) {
            if(projected->vectors[predicate].errors[r]) return -5;
            struct qs_cell keep=qs_vector_cell(&projected->plan,projected->vectors,predicate,r);
            if(keep.isnull || !keep.value) return 0;
        }
        for(unsigned i=0;i<right->expr.nout;++i) {
            unsigned index=projected->plan.outputs[i].index;
            const struct qs_vector *v=&projected->vectors[index];
            if(v->errors[r]) return -5;
            values[ln+i]=qs_vector_cell(&projected->plan,projected->vectors,index,r);
            if(v->kind==QS_TEXT && !values[ln+i].isnull) {
                unsigned text; if(intern_view(s,v->strings[r],&text)) return -3;
                values[ln+i].value=text;
            }
        }
    } else {
        struct qs_cell raw[QS_COLS];
        for(unsigned col=0;col<right->expr.nattrs;++col) if(right->expr.cols[col].slot>=0) {
            unsigned slot=2+right->expr.cols[col].slot;
            raw[col]=(struct qs_cell){.value=qs_values(chunk)[(size_t)slot*chunk->capacity+at],
                .scale=right->expr.cols[col].scale,.dscale=qs_dscales(chunk)[(size_t)slot*chunk->capacity+at],
                .isnull=(qs_nulls(chunk)[(size_t)slot*((chunk->capacity+7)/8)+at/8]>>(at%8))&1};
        }
        return project(s,right,raw,NULL,values+ln);
    }
    return 1;
}

static int index_qual_prepare(struct execution *s,unsigned id,struct qs_chunk *chunk,
    const struct projection_batch *projected,const struct projection_batch *output)
{
    if(chunk->selected>QS_VECTOR_ROWS) return -1;
    struct operator_state *o=&s->nodes[id]; struct index_qual_batch *q=o->index_qual;
    if(!q) {
        if(o->index_qual_checked) return 0;
        o->index_qual_checked=1;
        q=cs_work_calloc(s->context,1,sizeof(*q)); if(!q) return 0;
        if(join_qual_init(s,id,&q->expr,output)) { cs_work_free(s->context,q); return 0; }
        for(unsigned i=0;i<q->expr.plan.nexprs;++i) q->expr.needed[i]&=1;
        o->index_qual=q;
    }
    struct projection_batch *b=&q->expr;
    uint16_t active[QS_VECTOR_ROWS]; unsigned count=0;
    if(!q->preparing) { q->preparing=1; q->prepared=0; memset(q->status,0,chunk->selected*sizeof(*q->status)); }
    for(;q->prepared<chunk->selected;++q->prepared) {
        unsigned r=q->prepared;
        struct qs_cell values[QS_COLS];
        int status=index_join_values(s,id,chunk,r,projected,values);
        if(s->expression_wait) return status;
        q->status[r]=status;
        for(unsigned col=0;col<b->plan.nattrs;++col) {
            int slot=b->plan.cols[col].slot;
            if(slot>=0) b->cells[(size_t)slot*QS_VECTOR_ROWS+r]=status>0?values[col]:(struct qs_cell){.isnull=1};
        }
        if(status<0) {
            for(unsigned col=0;col<b->plan.ncols;++col) for(unsigned rest=r+1;rest<chunk->selected;++rest)
                b->cells[(size_t)col*QS_VECTOR_ROWS+rest]=(struct qs_cell){.isnull=1};
            q->prepared=chunk->selected; break;
        }
    }
    for(unsigned r=0;r<chunk->selected;++r) {
        if(q->status[r]<0) break;
        if(q->status[r]) active[count++]=r;
    }
    struct qs_vector_input input={.cells=b->cells,.dictionary=s->dictionary,
        .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    if(s->paged) { input.text=s->text; input.text_io=s->io; input.resume=b->retry; }
    int status=qs_vector_execute_input(&b->plan,NULL,NULL,0,chunk->selected,b->needed,b->vectors,input);
    input.plan=&b->plan; input.vectors=b->vectors; input.count=chunk->selected;
    if(!status) status=qs_vector_evaluate(&input,b->plan.predicate,active,count);
    if(status && input.text) text_status(s,qs_dictionary_text_request(input.text));
    b->retry=status && s->expression_wait;
    return status?-5:1;
}

int64_t qs_join_consume(unsigned stage,struct qs_chunk *chunk,const struct qs_plan *unused,
    struct qs_task *task,struct qs_join_control *control,struct femu_csd_args *args,int resume)
{
    (void)stage; (void)unused; (void)task; (void)control; (void)args; (void)resume;
    struct execution *s=index_execution; unsigned id=index_node;
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->nodes[id];
    struct projection_batch *b=index_columns;
    if(o->index_flush) { o->index_flush=0; return 0; }
    if(!o->index_active) {
        o->index_active=1; o->index_consume_position=0; o->index_projected=0;
        if(!b) { o->index_output->rows=0; o->index_position=0; }
    }
    struct projection_batch *projected=chunk->selected>1?o->index_projection:NULL;
    if(projected && !o->index_projected) {
        int status=index_project_batch(s,projected,chunk); if(status) return status;
        o->index_projected=1;
    }
    struct index_qual_batch *qualified=NULL;
    if(b && n->join_qual>=0 && chunk->selected>1) {
        int status=index_qual_prepare(s,id,chunk,projected,b); if(status<0) return status;
        if(status) qualified=o->index_qual;
    }
    /* The Index cursor retains this batch until the output is consumed.
     * SEMI gathers across rounds, so it keeps its existing owned buffer. */
    int borrow=qualified && !b->input_count && (n->join_type==QP_INNER || n->join_type==QP_LEFT);
    for(;o->index_consume_position<chunk->selected;++o->index_consume_position) {
        unsigned r=o->index_consume_position;
        unsigned at=qs_selection(chunk)[r]; struct qs_cell values[QS_COLS];
        unsigned source=qs_values(chunk)[chunk->capacity+at];
        if(source>=o->index_count) return -1;
        struct qs_cell match={.value=1};
        if(qualified) {
            int status=qualified->status[r]; if(status<0) return status; if(!status) continue;
            const struct projection_batch *q=&qualified->expr; unsigned index=q->plan.predicate;
            if(q->vectors[index].errors[r]) return -5;
            match=qs_vector_cell(&q->plan,q->vectors,index,r);
            if(match.isnull || !match.value) continue;
            for(unsigned col=0;!borrow && col<q->plan.nattrs;++col) {
                int slot=q->plan.cols[col].slot;
                if(slot>=0) values[col]=q->cells[(size_t)slot*QS_VECTOR_ROWS+r];
            }
        } else {
            if(o->project_retry) memcpy(values,o->project_retry->input,n->expr.nattrs*sizeof(*values));
            else {
                if(o->join_condition && o->join_condition->active)
                    memcpy(values,o->join_condition->input,n->expr.nattrs*sizeof(*values));
                else {
                    int status=index_join_values(s,id,chunk,r,projected,values); if(status<0) return status; if(!status) continue;
                }
                if(n->join_qual>=0 && join_condition(s,id,values,&match)) return -5;
            }
        }
        if(match.isnull || !match.value) continue;
        uint64_t bit=UINT64_C(1)<<(source%64);
        uint64_t *matches=&o->index_matches[source/64];
        if(n->join_type==QP_SEMI && (*matches&bit)) continue;
        if(n->join_type==QP_ANTI) { *matches|=bit; ++s->metrics.join_rows; continue; }
        if(b) {
            /* Keep only columns needed by the fused expression DAG. The
             * index cursor retains the decoded chunk until this batch drains. */
            if(borrow) {
                b->borrowed_cells=qualified->expr.cells;
                b->borrowed_selection=qualified->selection;
                qualified->selection[b->input_count++]=r;
            } else projection_append(b,values);
        } else {
            int status=project(s,n,values,NULL,o->index_output->data[o->index_output->rows].cells);
            if(status<0) return status;
            if(status) ++o->index_output->rows;
        }
        *matches|=bit; ++s->metrics.join_rows;
    }
    o->index_active=0; o->index_consume_position=0;
    if(o->index_qual) o->index_qual->preparing=o->index_qual->prepared=0;
    if(n->join_type==QP_SEMI || n->join_type==QP_ANTI) qs_index_skip(&o->index_control,0,o->index_matches);
    /* SEMI candidates arrive in rounds over unresolved lookup keys. Gather
     * accepted left rows until the lookup batch finishes, preserving its
     * early match skip without running the output DAG per candidate. */
    unsigned rows=b?b->input_count:o->index_output->rows;
    if(rows && (!b || n->join_type!=QP_SEMI)) {
        o->index_flush=1; return QP_INDEX_YIELD;
    }
    return 0;
}

static void index_metrics(struct operator_state *state,struct qs_metrics *metrics,struct qs_profile *profile)
{
    ordered_index_metrics(state,metrics,profile);
    if(metrics) qs_index_cache_stats(&state->index_control,metrics,profile);
}

static int64_t index_next_impl(struct execution *s,unsigned id,struct qs_cell *row,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    struct operator_state *o=&s->nodes[id]; unsigned ln=s->plan->nodes[n->left].expr.nout;
    if(!o->index_plan) {
        struct qs_plan *p=cs_work_calloc(s->context,1,sizeof(*p));
        if(!b) o->index_output=cs_work_calloc(s->context,1,sizeof(*o->index_output));
        o->index_left=cs_work_alloc(s->context,QS_INDEX_KEYS*ln*sizeof(*o->index_left));
        if(!p || (!b && !o->index_output) || !o->index_left) return -4;
        o->index_plan=p; p->nattrs=right->expr.nattrs+2; p->ncols=right->expr.ncols+2; p->njoins=1;
        p->work_mem_parent=(uintptr_t)s->context;
        p->join=right->expr.join; p->join.relid=right->expr.relid; p->join.relfilenode=right->expr.relfilenode;
        p->join.nattrs=right->expr.nattrs;
        p->join.semi=n->join_type==QP_SEMI || n->join_type==QP_ANTI?(b?-2:-1):0;
        p->join.probe_key=0; p->join.predicate=-1; p->join.work_mem_bytes=QS_INDEX_WORK_BYTES;
        ++p->join.build_key;
        p->cols[0]=(struct qs_column){.slot=0,.type=Q_INTEGER,.length=8,.align=8};
        p->cols[1]=p->cols[0]; p->cols[1].slot=1;
        for(unsigned i=0;i<right->expr.nattrs;++i) {
            p->cols[i+2]=right->expr.cols[i];
            if(p->cols[i+2].slot>=0) p->cols[i+2].slot+=2;
        }
        int status=graph_open_index(p,o); if(status) return status;
        o->metrics=index_metrics;
        struct execution *owner=s->worker?s->owner:s;
        if(owner->index_cache && s->plan->files[n->right].count && s->plan->indexes[n->right].count)
            qs_index_cache_bind(&o->index_control,owner->index_cache);
        if(s->paged) { int bound=qs_index_paged_dictionary(&o->index_control,s->paged); if(bound) return bound; }
        else qs_index_dictionary(&o->index_control,s->dictionary);
        index_projection_init(s,id);
        struct qs_chunk *input=(void *)(s->arena+vector_offset(s,n)),*output=(void *)(s->arena+vector_offset(s,n)+qs_index_input_bytes());
        *input=(struct qs_chunk){.capacity=QS_INDEX_KEYS,.ncols=2};
        *output=(struct qs_chunk){.capacity=256,.ncols=p->ncols};
    }
    for(;;) {
        if(!b && !o->index_active) {
            if(o->index_position<o->index_output->rows) {
                memcpy(row,o->index_output->data[o->index_position++].cells,n->expr.nout*sizeof(*row)); return 2;
            }
            o->index_output->rows=0; o->index_position=0;
        }
        if(o->phase==2) {
            while(o->index_unmatched<o->index_count) {
                unsigned at=o->index_unmatched++;
                if((n->join_type!=QP_LEFT && n->join_type!=QP_ANTI) ||
                   (o->index_matches[at/64]&(UINT64_C(1)<<(at%64)))) continue;
                struct qs_cell values[QS_COLS]; memcpy(values,o->index_left+(size_t)at*ln,ln*sizeof(*values));
                for(unsigned i=0;i<right->expr.nout;++i) values[ln+i]=(struct qs_cell){.isnull=1};
                if(b) {
                    projection_append(b,values);
                } else {
                    int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
                }
            }
            o->index_count=0; o->phase=0;
            if(b && b->input_count) return 2;
        }
        if(!o->phase) {
            unsigned batch=n->sorted_input?1:QS_INDEX_KEYS;
            while(o->index_count<batch && !o->input_done) {
                int64_t status=next_row(s,n->left,o->index_left+(size_t)o->index_count*ln);
                if(status==QP_WAIT && s->block_graph && !s->stream_block && s->scan_drained) status=0;
                if(status==QP_WAIT || status<0 || status>2) return status;
                if(!status) o->input_done=1; else ++o->index_count;
            }
            if(!o->index_count) {
                if(o->index_qual) {
                    cs_work_free(s->context,o->index_qual->expr.cells);
                    cs_work_free(s->context,o->index_qual->expr.vectors);
                    cs_work_free(s->context,o->index_qual); o->index_qual=NULL;
                }
                o->finished=1; return 0;
            }
            memset(o->index_matches,0,sizeof(o->index_matches));
            o->index_resume=0; o->index_flush=0; o->index_unmatched=0; o->phase=1;
            struct qs_chunk *input=(void *)(s->arena+vector_offset(s,n));
            input->rows=input->selected=o->index_count; memset(qs_nulls(input),0,2*((QS_INDEX_KEYS+7)/8));
            for(unsigned i=0;i<o->index_count;++i) {
                struct qs_cell key=o->index_left[(size_t)i*ln+n->left_keys[0]];
                if(key.value>INT64_MAX || key.value<INT64_MIN) return -5;
                qs_values(input)[i]=key.value; qs_values(input)[QS_INDEX_KEYS+i]=i;
                if(key.isnull) qs_nulls(input)[i/8]|=1U<<(i%8);
                qs_selection(input)[i]=i;
            }
        }
        if(s->control->ready && s->control->node==id && s->control->index_nreads) {
            s->control->ready=s->control->index_nreads=0;
        }
        struct qs_plan *p=o->index_plan;
        void *mr[]={s->arena+vector_offset(s,n),p,&o->index_task,&o->index_control,
            s->arena+vector_offset(s,n)+qs_index_input_bytes(),s->arena+input_offset(s,n)};
        long long len[]={qs_chunk_bytes(QS_INDEX_KEYS,2),sizeof(*p),sizeof(o->index_task),sizeof(o->index_control),
            qs_chunk_bytes(256,p->ncols),QS_INDEX_IO_BYTES};
        struct femu_csd_args a={.numr=6,.mr_addr=mr,.mr_len=len,.cparam2=s->args->cparam2};
        if(s->paged) qs_index_spill_io(&o->index_control,0,s->io);
        /* A self-join can reuse the input pages already retained by its scan.
         * This is the current morsel only; advancing the scan updates the window. */
        int scan=qp_probe_scan(s->plan,n->left);
        if(scan>=0 && right->expr.relid && s->plan->nodes[scan].expr.relid==right->expr.relid &&
           s->plan->nodes[scan].expr.relfilenode==right->expr.relfilenode && s->nodes[scan].position) {
            uint64_t end=s->nodes[scan].position,begin=(end-1)/s->plan->scan_bytes*s->plan->scan_bytes;
            qs_index_heap_window(&o->index_control,0,s->arena+input_offset(s,&s->plan->nodes[scan]),begin/QS_PAGE,(end-begin)/QS_PAGE);
        }
        index_execution=s; index_node=id; index_columns=b;
        int64_t status=qs_index_probe(0,mr[0],p,&o->index_task,&o->index_control,&a,o->index_resume,qs_join_consume);
        index_columns=NULL; o->index_resume=1;
        s->dictionary_wait=s->paged && qs_index_dictionary_pending(&o->index_control,0)?status:0;
        if(status==QP_INDEX_YIELD) { if(b) return 2; continue; }
        if(status==QS_INDEX_WAIT && o->index_task.index_pending) {
            s->control->node=id; s->control->index_nreads=o->index_task.index_nreads;
            memcpy(s->control->index_reads,o->index_task.index_reads,s->control->index_nreads*sizeof(s->control->index_reads[0]));
            return QP_WAIT;
        }
        if(status) return status;
        o->phase=2;
    }
}

int64_t index_next(struct execution *s,unsigned id,struct qs_cell *row,struct projection_batch *b)
{
    if(!b) return index_next_impl(s,id,row,NULL);
    b->stream_input_drained=false;
    if(b->error) { s->control->error=b->error; s->control->error_node=b->error_node; return b->error; }
    b->borrowed_cells=NULL; b->borrowed_selection=NULL;
    int previous=s->control->error; unsigned previous_node=s->control->error_node;
    int64_t status=index_next_impl(s,id,NULL,b);
    if(s->expression_wait) return status;
    if(status<0 && b->input_count) {
        b->error=status; b->error_node=s->control->error?s->control->error_node:id;
        s->control->error=previous; s->control->error_node=previous_node;
    } else if(status!=2) return status;
    b->count=b->input_count; b->input_count=b->position=0;
    struct qs_vector_input input={.cells=b->borrowed_cells?b->borrowed_cells:b->cells,.dictionary=s->dictionary,
        .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    if(b->plan.nexprs && projection_execute(b,NULL,b->borrowed_cells?b->borrowed_selection:NULL,0,input)) return -5;
    return 2;
}

static int64_t index_batch_read(struct execution *s,struct projection_batch *b)
{ return index_next(s,b->scan,NULL,b); }

static int64_t index_read_row(struct execution *s,unsigned id,struct qs_cell *row)
{ return index_next(s,id,row,NULL); }

static int64_t stream_index_probe(struct execution *s,unsigned id,unsigned scan,bool eof)
{
    int status=projection_init(s,id,0); if(status) return status;
    const struct projection_batch *projection=s->nodes[id].projection;
    if(!projection) return -4;
    if(eof) {
        if(s->stream_scanning) {
            const struct qp_node *leaf=&s->plan->nodes[scan];
            const struct qp_node *index=&s->plan->nodes[projection->scan];
            const struct qp_node *right=&s->plan->nodes[index->right];
            bool shares_heap=right->expr.relid && right->expr.relid==leaf->expr.relid &&
                right->expr.relfilenode==leaf->expr.relfilenode;
            /* A self-join can reuse pages from its current LOAD. Flush
             * before replacing that window, even if more blocks follow. */
            if(!shares_heap && s->nodes[scan].position<leaf->bytes)
                return graph_finish_stream();
        }
        s->nodes[projection->scan].input_done=1;
    }
    int64_t ready=projection_fill(s,id,0);
    if(ready==2) {
        s->nodes[id].published_batch=&s->nodes[id].projection;
        return graph_publish_output();
    }
    if(ready==QP_WAIT && s->control->index_nreads && !s->control->ready) {
        int64_t request=namespace_wait(s); return request?request:-1;
    }
    if(eof && !ready) return graph_finish_stream();
    if(!eof && (ready==QP_WAIT || !ready) && s->scan_drained)
        return graph_consume_input();
    return ready==3 || !ready?-4:ready;
}

static int64_t stream_index_join(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    (void)out;
    bool eof=s->args->graph_input==FEMU_GRAPH_INPUT_EOF;
    if(!s->block_chain && !graph_device_source(&s->plan->nodes[scan])) {
        s->nodes[s->root].read_projection=index_batch_read;
        s->nodes[s->root].read_row=index_read_row;
        return stream_index_probe(s,sink,scan,eof);
    }

    uint64_t encoded=(uint64_t)s->args->cparam1>>QP_GRAPH_BLOCK_NODE_SHIFT;
    if(!encoded || encoded>s->plan->count) return -1;
    unsigned id=encoded-1;
    s->nodes[id].read_projection=index_batch_read;
    s->nodes[id].read_row=index_read_row;
    if(eof) return stream_index_probe(s,id,scan,true);

    struct operator_state *op=&s->nodes[id];
    if(!op->stream_inlet) {
        op->stream_inlet=true;
        op->input_done=op->finished=0;
    }
    int64_t status=graph_attach_stream_input(s,scan,s->plan->nodes[id].left);
    if(status) return status;
    status=stream_index_probe(s,id,scan,false);
    if(status==graph_consume_input()) op->stream_inlet=false;
    s->stream_edge=NULL;
    return status;
}

int64_t pg_sina_graph_block_index(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_INDEX,stream_index_join); }
