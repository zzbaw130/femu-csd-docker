#include "graph_state.h"
#include "graph_batch.h"

int64_t pg_sina_filter_impl(struct femu_csd_args *);
int64_t qs_filter_selected(struct femu_csd_args *);

int scan_filter_init(struct execution *s,const struct qs_plan *p,struct operator_state *o)
{
    int raw=s->raw_projection && p==&s->plan->nodes[s->plan->root].expr;
    if(!o->scan_filter_checked && raw && p->predicate<0) {
        o->scan_filter=cs_work_alloc(s->context,sizeof(*p)); if(!o->scan_filter) return -4;
        *o->scan_filter=*p;
        for(unsigned col=0;col<p->nattrs;++col)
            if(p->cols[col].slot>=0 && (p->cols[col].type==Q_DECIMAL || p->cols[col].type==Q_STRING))
                o->scan_filter->cols[col].deferred=QS_FILTER_ONLY;
        o->scan_filter_checked=1;
    }
    if(!o->scan_filter_checked) {
        o->scan_filter_checked=1; uint8_t needed[QS_EXPRS]={0};
        if(p->predicate<0 && !o->defer_strings) return 0;
        struct qs_plan *filter=cs_work_alloc(s->context,sizeof(*p));
        if(!filter) { if(raw) s->raw_projection=0; return 0; }
        *filter=*p; filter->work_mem_parent=(uintptr_t)s->context;
        if(p->predicate<0) {
            for(unsigned col=0;col<p->nattrs;++col)
                if(filter->cols[col].slot>=0 && filter->cols[col].type==Q_STRING) filter->cols[col].deferred=QS_DEFERRED;
            o->scan_filter=filter; return 0;
        }
        /* Parameter values keep int128 payloads and their actual numeric scale.
         * The original PG plan remains unchanged; this copy belongs to a CU. */
        for(unsigned i=0;i<filter->nexprs;++i) {
            struct qs_expr *e=&filter->exprs[i];
            if(e->op==QP_INSET) goto scalar; /* Query-owned state is filtered by projection batches. */
            if(e->op==QP_PARAMETER) {
                if(e->column<0 || e->column>=QP_PARAMS) goto scalar;
                e->scale=s->params[e->column].scale; e->dscale=s->params[e->column].dscale;
            } else if(e->op==Q_ADD || e->op==Q_SUB || e->op==Q_MUL || e->op==Q_CASE) {
                if(e->left<0 || e->right<0 || e->left>=(int)i || e->right>=(int)i) goto scalar;
                unsigned left=filter->exprs[e->left].scale,right=filter->exprs[e->right].scale;
                unsigned scale=e->op==Q_MUL?left+right:left>right?left:right;
                if(e->op==Q_CASE) {
                    if(e->otherwise<0 || e->otherwise>=(int)i) goto scalar;
                    scale=right>filter->exprs[e->otherwise].scale?right:filter->exprs[e->otherwise].scale;
                }
                if(scale>UINT8_MAX) goto scalar;
                e->scale=scale;
            }
        }
        if(qs_vector_require(filter,filter->predicate,needed)) goto scalar;
        uint8_t projected[QS_EXPRS]={0}; int known=1;
        for(unsigned i=0;i<filter->nout;++i)
            if(qs_vector_require(filter,filter->outputs[i].index,projected)) { known=0; break; }
        for(unsigned col=0;col<filter->nattrs;++col) {
            struct qs_column *c=&filter->cols[col];
            if((c->type!=Q_DECIMAL && c->type!=Q_STRING) || c->slot<0) continue;
            c->deferred=raw || known?QS_FILTER_ONLY:QS_DEFERRED;
            for(unsigned i=0;!raw && i<filter->nexprs;++i) if(projected[i] && filter->exprs[i].op==Q_COLUMN && filter->exprs[i].column==(int)col)
                c->deferred=QS_DEFERRED;
        }
        /* Reuse the baseline Filter path on borrowed Heap strings. Only its
         * immutable literals need a resident dictionary; survivors are still
         * interned by the resumable projection, not by the flat Filter CSF. */
        if(s->paged) for(unsigned col=0;col<filter->nattrs;++col)
            if(filter->cols[col].type==Q_STRING && filter->cols[col].slot>=0)
                filter->cols[col].deferred=QS_FILTER_ONLY;
        for(unsigned i=0;i<filter->nexprs;++i) if(needed[i] && filter->exprs[i].op==Q_CONST && filter->exprs[i].type==Q_STRING && !filter->exprs[i].isnull) {
            const struct qs_string *literal=&filter->strings[filter->exprs[i].constant];
            struct qs_dict *dictionary=s->dictionary;
            if(s->paged) {
                if(!o->filter_dictionary) {
                    unsigned bytes=0;
                    for(unsigned j=0;j<filter->nstrings;++j) bytes+=filter->strings[j].length;
                    o->filter_dictionary=cs_work_calloc(s->context,1,qs_dict_bytes(512,bytes));
                    if(!o->filter_dictionary) goto scalar;
                    *o->filter_dictionary=(struct qs_dict){.entries=512,.capacity=512,.data=bytes};
                }
                dictionary=o->filter_dictionary;
            }
            int id=qs_dict_insert(dictionary,literal->bytes,literal->length);
            if(id<0) { cs_work_free(s->context,filter); return -3; }
            filter->exprs[i].constant=id;
        }
        o->scan_filter=filter; return 0;
scalar:
        if(raw) s->raw_projection=0;
        cs_work_free(s->context,o->filter_dictionary); o->filter_dictionary=NULL;
        cs_work_free(s->context,filter);
    }
    return 0;
}

static void scan_hash_filter_init(struct execution *s,unsigned id,struct operator_state *o)
{
    o->scan_hash_checked=true;
    const struct qp_plan *plan=s->plan;
    const struct qs_plan *filter=o->scan_filter;
    if(plan->ninit || plan->nparams || (s->owner?s->owner:s)->partition_join>=0) return;
    for(unsigned i=0;i<QP_PARAMS;++i) if(plan->cte_nodes[i]==(int)id) return;
    uint8_t needed[QS_EXPRS]={0};
    if(qs_vector_require(filter,filter->predicate,needed)) return;
    bool text=false;
    for(unsigned i=0;i<filter->nexprs;++i) if(needed[i]) {
        const struct qs_expr *e=&filter->exprs[i];
        if(e->op==Q_COLUMN) text|=e->type==Q_STRING;
        else if(e->op!=Q_CONST && e->op!=Q_AND && e->op!=Q_OR && (e->op<Q_EQ || e->op>Q_GE))
            return;
    }
    if(!text) return;
    int parent=-1;
    for(unsigned i=0;i<plan->count;++i) {
        const struct qp_node *n=&plan->nodes[i];
        if(n->right==(int)id) return;
        if(n->left==(int)id) {
            if(parent>=0) return;
            parent=i;
        }
    }
    if(parent<0) return;
    const struct qp_node *join=&plan->nodes[parent];
    const struct operator_state *build=graph_state_owner(s,parent);
    if(join->kind!=QP_JOIN || !join->hash || join->join_type!=QP_INNER || join->nkeys!=1 ||
        !build->hash_phase || !build->hash_data || build->external || build->hash_fallback ||
        !build->hash_buckets || build->hash_buckets>32768 ||
        8.0*build->hash_rows>=plan->nodes[id].estimated_rows)
        return;
    const struct qp_node *right=&plan->nodes[join->right];
    if(right->kind!=QP_SCAN || right->expr.predicate<0) return;
    const struct qs_expr *key=&filter->exprs[filter->outputs[join->left_keys[0]].index];
    const struct qs_expr *build_key=&right->expr.exprs[right->expr.outputs[join->right_keys[0]].index];
    if(key->op!=Q_COLUMN || key->type!=Q_INTEGER || build_key->type!=Q_INTEGER ||
        key->column<0 || (unsigned)key->column>=filter->nattrs) return;
    const struct qs_column *column=&filter->cols[key->column];
    if(column->type!=Q_INTEGER || column->slot<0 || column->deferred) return;
    o->scan_hash_slot=column->slot;
    o->scan_hash_mask=build->hash_buckets-1;
    o->scan_hash_buckets=hash_chain(build,hash_row_bytes(&right->expr))+build->hash_capacity;
}

int64_t scan_filter_chunk(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->nodes[id];
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n));
    if(!o->scan_filter || o->scan_filter->predicate<0) return 0;
    void *fm[]={chunk,o->scan_filter,s->arena+input_offset(s,n)+o->scan_raw,o->filter_dictionary?o->filter_dictionary:s->dictionary,s->params};
    long long fl[]={qs_chunk_bytes(chunk->capacity,chunk->ncols),sizeof(*o->scan_filter),o->scan_raw_bytes,0,sizeof(s->params)};
    struct femu_csd_args filter={.numr=5,.mr_addr=fm,.mr_len=fl};
    int64_t status;
    if(!o->scan_hash_checked) scan_hash_filter_init(s,id,o);
    if(o->scan_hash_buckets) {
        struct blocking_timer timer={0}; if(s->profiling) timer=blocking_begin(s);
        const int64_t *keys=qs_values(chunk)+(size_t)o->scan_hash_slot*chunk->capacity;
        const uint8_t *nulls=qs_nulls(chunk)+(size_t)o->scan_hash_slot*((chunk->capacity+7)/8);
        chunk->selected=0;
        for(unsigned row=0;row<chunk->rows;++row) {
            if(nulls[row/8]&(1U<<(row%8))) continue;
            int64_t key=keys[row];
            uint64_t hash=mix_hash(UINT64_C(0x9e3779b97f4a7c15)^(uint64_t)key^-(uint64_t)(key<0));
            if(o->scan_hash_buckets[hash&o->scan_hash_mask]!=UINT_MAX)
                qs_selection(chunk)[chunk->selected++]=row;
        }
        status=qs_filter_selected(&filter);
        if(s->profiling) blocking_end(s,QS_FILTER,timer);
    } else status=timed_call(s,pg_sina_filter_impl,&filter,QS_FILTER);
    return status;
}

