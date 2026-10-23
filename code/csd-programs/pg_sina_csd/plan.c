/* Execute a PG physical tree. PG submits page LOADs and SNIA handles spill;
 * tuple bytes and intermediate rows never cross back to the PG executor. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "plan.h"
#include "query_vector.h"
#include "index_scan.h"
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>

extern int64_t pg_sina_scan_impl(struct femu_csd_args *);
extern int64_t pg_sina_filter_impl(struct femu_csd_args *);
extern int64_t pg_sina_sort_sink_impl(struct femu_csd_args *);
extern int64_t pg_sina_sort_source_impl(struct femu_csd_args *);
extern void qs_sort_dictionary(uint64_t,struct qs_dict *);
#define QP_WAIT 1
/* Store PG integer/date widths directly; numeric and other values retain all
 * 128 bits. Keep scale/dscale/NULL, and avoid unaligned integer accesses. */
static unsigned hash_value_bytes(const struct qs_plan *p,unsigned column)
{
    switch(p->outputs[column].type_oid) {
    case 16:case 21:case 23:case 1082:return 4;
    case 20:return 8;
    default:return 16;
    }
}
static unsigned hash_row_bytes(const struct qs_plan *p)
{
    unsigned bytes=0; for(unsigned i=0;i<p->nout;++i) {
        unsigned value=hash_value_bytes(p,i); bytes+=value+(value<16?1:3);
    }
    return bytes;
}
static void hash_store(unsigned char *to,const struct qs_cell *from,const struct qs_plan *p)
{
    for(unsigned i=0;i<p->nout;++i) {
        unsigned bytes=hash_value_bytes(p,i);
        memcpy(to,&from[i].value,bytes);
        if(bytes<16) { to[bytes]=from[i].isnull; to+=bytes+1; }
        else { to[bytes]=from[i].scale; to[bytes+1]=from[i].dscale; to[bytes+2]=from[i].isnull; to+=bytes+3; }
    }
}
static void hash_load(struct qs_cell *to,const unsigned char *from,const struct qs_plan *p)
{
    for(unsigned i=0;i<p->nout;++i) {
        unsigned bytes=hash_value_bytes(p,i);
        if(bytes==4) { int32_t v; memcpy(&v,from,4); to[i].value=v; }
        else if(bytes==8) { int64_t v; memcpy(&v,from,8); to[i].value=v; }
        else memcpy(&to[i].value,from,16);
        if(bytes<16) { to[i].scale=to[i].dscale=0; to[i].isnull=from[bytes]; from+=bytes+1; }
        else { to[i].scale=from[bytes]; to[i].dscale=from[bytes+1]; to[i].isnull=from[bytes+2]; from+=bytes+3; }
    }
}
struct sorted_rows {
    struct qs_plan plan;
    struct qs_task task;
    struct qs_group group;
    struct qs_result *input,*output;
    unsigned position;
    int initialized, complete, drained, input_pending;
    unsigned char *prefix;
    unsigned prefix_rows,prefix_position;
    struct qs_metrics metrics;
};
struct spool {
    unsigned char *tail,*cache;
    uint64_t *tokens;
    unsigned blocks, capacity, used, loaded;
    uint64_t rows;
};
struct accumulator { struct qs_cell sum,extreme,last; uint64_t count; int has_last; };
struct compact_group { struct qs_cell sum; int64_t key; uint64_t count; unsigned occupied; };
struct set_key { int64_t value; unsigned occupied; };
struct operator_state {
    struct sorted_rows left_sort,right_sort;
    struct qs_cell left[QS_OUT],right[QS_OUT],first[QS_OUT],keys[QS_KEYS];
    struct accumulator accum[QS_AGGS];
    struct spool group;
    uint64_t position, skipped, emitted;
    unsigned scan_position;
    struct qs_plan *scan_filter;
    int scan_filter_checked;
    int phase, have_left, have_right, input_done, group_ready, matched, finished;
    struct qs_plan *index_plan;
    struct qs_join_control index_control;
    struct qs_task index_task;
    struct qs_result *index_output;
    struct qs_cell *index_left;
    uint64_t index_matches;
    unsigned index_count, index_unmatched;
    unsigned index_position;
    int index_resume, index_flush;
    unsigned char *hash_data;
    unsigned hash_rows,hash_capacity,hash_match,hash_phase;
    int hash_matched,hash_fallback,hash_shared;
    struct compact_group *groups;
    unsigned group_capacity,group_count,group_flush,group_phase;
    struct set_key *set;
    unsigned set_capacity,set_count;
    int set_null,set_ready;
};
struct execution {
    cs_work_context *context;
    struct qp_plan *plan;
    struct qp_control *control;
    struct qs_dict *dictionary;
    struct operator_state *nodes;
    struct qs_cell params[QP_PARAMS];
    unsigned char *arena;
    void *io;
    size_t io_bytes, sort_limit;
    struct sorted_rows *pending_sort;
    struct spool *pending_spool;
    unsigned pending_block;
    int pending_store;
    struct qs_metrics metrics;
    struct qs_profile profile;
    bool profiling;
    struct femu_csd_args *args;
    unsigned initializer,initializer_phase;
    unsigned worker, parallel_done, parallel_position[QS_WORKERS];
    int parallel_started;
    unsigned parallel_node, root, used_workers;
    atomic_uint_fast64_t next_offset;
    atomic_uint active_cus, peak_cus;
    struct execution *owner, *workers[QS_WORKERS];
    struct qs_result *outputs;
};
static uint64_t now_ns(clockid_t clock)
{ struct timespec t; clock_gettime(clock,&t); return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }
static size_t input_offset(const struct execution *s,const struct qp_node *n)
{ return s->worker>1?n->parallel_input+(s->worker-2)*s->plan->parallel_stride:n->input_offset; }
static size_t vector_offset(const struct execution *s,const struct qp_node *n)
{ return s->worker>1?n->parallel_vector+(s->worker-2)*s->plan->parallel_stride:n->vector_offset; }
static int64_t parallel_next(struct execution *,unsigned,struct qs_cell *);
static int64_t timed_call(struct execution *s,int64_t (*call)(struct femu_csd_args *),struct femu_csd_args *a,unsigned op)
{
    if(!s->profiling) return call(a);
    uint64_t wall=now_ns(CLOCK_MONOTONIC),cpu=now_ns(CLOCK_THREAD_CPUTIME_ID);
    int64_t status=call(a); struct qs_timing *t=&s->profile.operators[op];
    ++t->calls; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-wall; t->cpu_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
    return status;
}
static int64_t next_row_impl(struct execution *,unsigned,struct qs_cell *);
static inline int64_t next_row(struct execution *s,unsigned id,struct qs_cell *row)
{
    int64_t status=next_row_impl(s,id,row);
    if(status<0 && !s->control->error) { s->control->error=status; s->control->error_node=id; }
    return status;
}
static int64_t join_next(struct execution *,unsigned,struct qs_cell *);
static int key_compare(struct execution *,const struct qp_node *,const struct qs_cell *,const struct qs_cell *);
static int keys_null(const struct qp_node *,const struct qs_cell *);
static uint64_t mix_hash(uint64_t);
static int64_t build_set(struct execution *,unsigned);
static _Thread_local struct execution *index_execution;
static _Thread_local unsigned index_node;
#define QP_INDEX_YIELD 4
static void init_eval(struct qs_eval *e,const struct qs_plan *plan,const struct qs_cell *row,const struct qs_cell *aggregates)
{
    e->plan=plan; e->tuple=row; e->aggregates=aggregates;
    memset(e->valid,0,plan->nexprs);
}
static int compare_cell(struct execution *s,struct qs_cell a,struct qs_cell b,unsigned type)
{
    if(a.isnull || b.isnull) return (a.isnull!=0)-(b.isnull!=0);
    if(type==Q_STRING) {
        const char *x,*y; unsigned xn,yn;
        if(qs_dict_get(s->dictionary,(unsigned)a.value,&x,&xn) || qs_dict_get(s->dictionary,(unsigned)b.value,&y,&yn)) return INT_MIN;
        int cmp=memcmp(x,y,xn<yn?xn:yn); return cmp?cmp:(xn>yn)-(xn<yn);
    }
    int cmp=0; return qs_compare(a,b,&cmp)?INT_MIN:cmp;
}
static unsigned character_bytes(const char *text,unsigned available)
{
    if(!available) return 0;
    unsigned char c=*text; unsigned n=c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:4;
    return n<=available?n:available;
}
static int glob(const char *p,unsigned pn,const char *v,unsigned vn)
{
    if(!pn) return !vn;
    if(*p=='%') {
        while(pn && *p=='%') { ++p; --pn; }
        if(!pn) return 1;
        for(unsigned i=0;;) {
            if(glob(p,pn,v+i,vn-i)) return 1;
            if(i==vn) break;
            i+=character_bytes(v+i,vn-i);
        }
        return 0;
    }
    if(*p=='\\' && pn>1) { ++p; --pn; return vn && *p==*v && glob(p+1,pn-1,v+1,vn-1); }
    if(*p=='_') { unsigned n=character_bytes(v,vn); return n && glob(p+1,pn-1,v+n,vn-n); }
    return vn && *p==*v && glob(p+1,pn-1,v+1,vn-1);
}
static int evaluate(struct execution *s,struct qs_eval *e,int index,struct qs_cell *out)
{
    if(index<0 || (unsigned)index>=e->plan->nexprs) return -1;
    if(e->valid[index]) { *out=e->cells[index]; return 0; }
    const struct qs_expr *x=&e->plan->exprs[index]; struct qs_cell a={0},b={0},v={0};
    if(x->op==QP_PARAMETER) v=s->params[x->column];
    else if(x->op==QP_INSET) {
        if(evaluate(s,e,x->left,&a)) return -5;
        struct operator_state *set=&s->nodes[x->column];
        if(!set->set_ready) return -1;
        if(a.isnull) v.isnull=set->set_count || set->set_null;
        else {
            if(a.value<=INT64_MAX && a.value>=INT64_MIN && set->set_capacity) {
                unsigned slot=mix_hash((uint64_t)a.value)&(set->set_capacity-1);
                while(set->set[slot].occupied) {
                    if(set->set[slot].value==a.value) { v.value=1; break; }
                    slot=(slot+1)&(set->set_capacity-1);
                }
            }
            if(!v.value) v.isnull=set->set_null;
        }
    }
    else if(x->op==Q_CONST && x->type==Q_STRING && !x->isnull) {
        const struct qs_string *literal=&e->plan->strings[x->constant];
        int id=qs_dict_insert(s->dictionary,literal->bytes,literal->length); if(id<0) return -3;
        v.value=id;
    } else if(x->op==Q_COLUMN || x->op==Q_CONST || x->op==Q_AGGREF) return qs_evaluate(e,index,out);
    else {
        if(evaluate(s,e,x->left,&a)) return -5;
        if(x->op==Q_CASE) {
            if(evaluate(s,e,!a.isnull && a.value?x->right:x->otherwise,&v)) return -5;
            /* Preserve the actual numeric scale of an intermediate value. */
        } else if(x->op==QP_COALESCE) {
            v=a; if(a.isnull && evaluate(s,e,x->right,&v)) return -5;
        } else if(x->op==Q_AND && !a.isnull && !a.value) v=a;
        else if(x->op==Q_OR && !a.isnull && a.value) v=a;
        else if(x->op==Q_NOT || x->op==Q_IS_NULL || x->op==Q_IS_NOT_NULL || x->op==Q_YEAR)
            return qs_evaluate(e,index,out);
        else {
            if(evaluate(s,e,x->right,&b)) return -5;
            v.isnull=a.isnull || b.isnull;
            if(x->op==QP_LIKE || x->op==QP_SUBSTRING ||
               (x->op>=Q_EQ && x->op<=Q_GE && e->plan->exprs[x->left].type==Q_STRING)) {
                if(!v.isnull) {
                    const char *left,*right; unsigned ln,rn;
                    if(qs_dict_get(s->dictionary,(unsigned)a.value,&left,&ln)) return -3;
                    if(x->op==QP_SUBSTRING) {
                        struct qs_cell length; if(evaluate(s,e,x->otherwise,&length)) return -5;
                        if(length.isnull) v.isnull=1;
                        else {
                            if(length.value<0 || b.value<INT_MIN || b.value>INT_MAX || length.value>INT_MAX) return -5;
                            int64_t begin=b.value-1,end=begin+length.value;
                            if(begin<0) begin=0;
                            if(end<begin) end=begin;
                            unsigned first=0,last=0; int64_t character=0;
                            while(first<ln && character<begin) { first+=character_bytes(left+first,ln-first); ++character; }
                            last=first;
                            while(last<ln && character<end) { last+=character_bytes(left+last,ln-last); ++character; }
                            int id=qs_dict_insert(s->dictionary,left+first,last-first); if(id<0) return -3;
                            v.value=id;
                        }
                    } else {
                        if(qs_dict_get(s->dictionary,(unsigned)b.value,&right,&rn)) return -3;
                        if(x->op==QP_LIKE) v.value=glob(right,rn,left,ln);
                        else {
                            int cmp=memcmp(left,right,ln<rn?ln:rn); if(!cmp) cmp=(ln>rn)-(ln<rn);
                            v.value=x->op==Q_EQ?cmp==0:x->op==Q_NE?cmp!=0:x->op==Q_LT?cmp<0:x->op==Q_LE?cmp<=0:x->op==Q_GT?cmp>0:cmp>=0;
                        }
                    }
                }
            } else return qs_evaluate(e,index,out);
        }
    }
    e->valid[index]=1; e->cells[index]=v; *out=v; return 0;
}
static int project_filtered(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row,int filtered)
{
    struct qs_eval e; struct qs_cell keep; int initialized=0;
    if(!filtered && n->expr.predicate>=0) {
        init_eval(&e,&n->expr,input,aggregates); initialized=1;
        if(evaluate(s,&e,n->expr.predicate,&keep)) return -5;
        if(keep.isnull || !keep.value) return 0;
    }
    for(unsigned i=0;i<n->expr.nout;++i) {
        int index=n->expr.outputs[i].index; const struct qs_expr *x=&n->expr.exprs[index];
        if(input && x->op==Q_COLUMN) { row[i]=input[x->column]; continue; }
        if(!initialized) { init_eval(&e,&n->expr,input,aggregates); initialized=1; }
        if(evaluate(s,&e,index,&row[i])) return -5;
    }
    return 1;
}
static int project(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row)
{ return project_filtered(s,n,input,aggregates,row,0); }
static int64_t sort_call(struct execution *s,struct sorted_rows *sort,int source)
{
    void *mr[4]; long long len[4];
    struct femu_csd_args a={.numr=source?3:4,.mr_addr=mr,.mr_len=len};
    if(source) {
        mr[0]=&sort->group; len[0]=sizeof(sort->group);
        mr[1]=sort->output; len[1]=sizeof(*sort->output); mr[2]=s->io; len[2]=s->io_bytes;
    } else {
        mr[0]=sort->input; len[0]=sizeof(*sort->input); mr[1]=&sort->plan; len[1]=sizeof(sort->plan);
        mr[2]=&sort->task; len[2]=sizeof(sort->task); mr[3]=s->io; len[3]=s->io_bytes;
    }
    if(s->pending_sort==sort) { a.cparam2=s->args->cparam2; s->pending_sort=NULL; }
    int64_t status=timed_call(s,source?pg_sina_sort_source_impl:pg_sina_sort_sink_impl,&a,source?QS_SORT_SOURCE:QS_SORT_SINK);
    if(status>0) s->pending_sort=sort;
    if(source && !status) {
        const struct qs_metrics *m=&sort->output->metrics,*old=&sort->metrics;
        s->metrics.spill_read_bytes+=m->spill_read_bytes-old->spill_read_bytes;
        s->metrics.spill_write_bytes+=m->spill_write_bytes-old->spill_write_bytes;
        s->metrics.runs+=m->runs-old->runs;
        s->metrics.spilled_runs+=m->spilled_runs-old->spilled_runs;
        sort->metrics=*m;
    }
    return status;
}
static int64_t sort_init(struct execution *s,struct sorted_rows *sort,const struct qs_plan *input,
    unsigned keys,const int *columns,const struct qs_order *order)
{
    if(sort->initialized) return 0;
    sort->input=cs_work_calloc(s->context,1,sizeof(*sort->input));
    sort->output=cs_work_calloc(s->context,1,sizeof(*sort->output));
    if(!sort->input || !sort->output) return -4;
    sort->plan.nout=input->nout; sort->plan.ordered_rows=2; sort->plan.limit=-1;
    sort->plan.work_mem_bytes=s->sort_limit;
    sort->plan.work_mem_parent=(uintptr_t)s->context;
    memcpy(sort->plan.outputs,input->outputs,input->nout*sizeof(input->outputs[0]));
    for(unsigned i=0;i<keys;++i) sort->plan.order[sort->plan.norder++]=order?order[i]:(struct qs_order){.output=columns[i]};
    sort->task.workers=1;
    int64_t status=sort_call(s,sort,0); if(status) return status;
    qs_sort_dictionary(sort->task.state,s->dictionary);
    sort->initialized=1; return 0;
}
static int64_t sorted_input(struct execution *s,struct sorted_rows *sort,unsigned child,
    unsigned keys,const int *columns,const struct qs_order *order,struct qs_cell *row)
{
    const struct qp_node *input=&s->plan->nodes[child];
    int64_t status=sort_init(s,sort,&input->expr,keys,columns,order); if(status) return status;
    if(sort->drained && !sort->output) return 0;
    while(!sort->complete) {
        if(sort->input_pending) {
            status=sort_call(s,sort,0); if(status) return status;
            sort->input_pending=0; sort->input->rows=0;
        }
        if(sort->prefix_position<sort->prefix_rows) {
            hash_load(sort->input->data[sort->input->rows].cells,
                sort->prefix+(size_t)sort->prefix_position++*hash_row_bytes(&input->expr),&input->expr);
            status=2;
        } else {
            if(sort->prefix) { cs_work_free(s->context,sort->prefix); sort->prefix=NULL; }
            status=next_row(s,child,sort->input->data[sort->input->rows].cells);
        }
        if(status<0 || status>2) return status;
        if(status==QP_WAIT) return QP_WAIT;
        /* next_row returns 2 for a row and 0 for EOF. */
        if(!status) {
            if(sort->input->rows) { sort->input_pending=1; continue; }
            sort->complete=1; sort->group.count=1; sort->group.states[0]=sort->task.state;
            cs_work_free(s->context,sort->input); sort->input=NULL; break;
        }
        if(++sort->input->rows==QS_RESULT_ROWS) sort->input_pending=1;
    }
    if((s->pending_sort==sort || sort->position==sort->output->rows) && !sort->drained) {
        status=sort_call(s,sort,1); if(status) return status;
        sort->position=0; sort->drained=sort->output->finished;
    }
    if(sort->position<sort->output->rows) {
        memcpy(row,sort->output->data[sort->position++].cells,input->expr.nout*sizeof(*row)); return 2;
    }
    if(sort->drained) { cs_work_free(s->context,sort->output); sort->output=NULL; }
    return 0;
}
static uint64_t mix_hash(uint64_t x)
{ x^=x>>33; x*=UINT64_C(0xff51afd7ed558ccd); x^=x>>33; x*=UINT64_C(0xc4ceb9fe1a85ec53); return x^(x>>33); }
static uint64_t row_hash(const struct qp_node *n,const struct qs_cell *row,int right)
{
    uint64_t hash=0x9e3779b97f4a7c15ULL;
    for(unsigned i=0;i<n->nkeys;++i) {
        struct qs_cell v=row[right?n->right_keys[i]:n->left_keys[i]];
        while(v.scale && !(v.value%10)) { v.value/=10; --v.scale; }
        hash=mix_hash(hash^(uint64_t)v.value^(uint64_t)((unsigned __int128)v.value>>64)^((uint64_t)v.scale<<56));
    }
    return hash;
}
static int64_t hash_build(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    unsigned row_bytes=hash_row_bytes(right);
    if(o->hash_fallback) return 0;
    size_t stride=row_bytes+3*sizeof(uint32_t);
    /* Hash build is not a sort run: allow it to use the query budget while
     * retaining two sort quotas for a Merge fallback. The parent allocator
     * still enforces actual usage across all active nodes and workers. */
    struct cs_work_context_stats memory;
    if(cs_work_get_context_stats(s->context,&memory)) return -4;
    size_t hash_limit=memory.limit_bytes-2*s->sort_limit;
    if(!o->hash_capacity) {
        uint64_t estimate=s->plan->nodes[n->right].estimated_rows;
        /* Blocking children need their working space before producing rows. */
        if(s->plan->nodes[n->right].kind!=QP_SCAN) estimate=128;
        unsigned max=1; while((size_t)max*2*stride<=hash_limit && max<UINT_MAX/4) max*=2;
        unsigned capacity=128; while(capacity<estimate && capacity<max) capacity*=2;
        if(capacity>max) capacity=max;
        o->hash_data=cs_work_alloc(s->context,capacity*stride);
        if(!o->hash_data) { o->hash_fallback=1; return 0; }
        o->hash_capacity=capacity;
    }
    while(!o->hash_phase) {
        if(o->hash_rows==o->hash_capacity) {
            size_t bytes=(size_t)o->hash_capacity*2*stride;
            void *data=bytes<=hash_limit?cs_work_realloc(s->context,o->hash_data,bytes):NULL;
            if(!data) {
                /* Preserve already-built projected rows. The fallback sorts
                 * this prefix plus the remaining child stream, never rereads
                 * or decodes the original heap for another build batch. */
                size_t used=(size_t)o->hash_rows*row_bytes;
                void *prefix=cs_work_realloc(s->context,o->hash_data,used);
                o->right_sort.prefix=prefix?prefix:o->hash_data; o->right_sort.prefix_rows=o->hash_rows;
                o->hash_data=NULL; o->hash_fallback=1; return 0;
            }
            o->hash_data=data; o->hash_capacity*=2;
        }
        int64_t status=next_row(s,n->right,o->right);
        if(status==QP_WAIT || status<0 || status>2) return status;
        if(status==2) { hash_store(o->hash_data+(size_t)o->hash_rows*row_bytes,o->right,right); ++o->hash_rows; ++s->metrics.build_rows; continue; }
        uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*row_bytes),*buckets=next+o->hash_capacity;
        memset(buckets,0xff,(size_t)o->hash_capacity*2*sizeof(*buckets));
        for(unsigned i=0;i<o->hash_rows;++i) {
            hash_load(o->right,o->hash_data+(size_t)i*row_bytes,right);
            unsigned bucket=row_hash(n,o->right,1)&(2*o->hash_capacity-1);
            next[i]=buckets[bucket]; buckets[bucket]=i;
        }
        o->hash_phase=1;
    }
    return 0;
}
static int64_t hash_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned rn=s->plan->nodes[n->right].expr.nout,ln=s->plan->nodes[n->left].expr.nout;
    const struct qs_plan *right_plan=&s->plan->nodes[n->right].expr;
    unsigned row_bytes=hash_row_bytes(right_plan);
    if(!o->hash_phase && !o->hash_fallback) { int64_t status=hash_build(s,id); if(status) return status; }
    if(o->hash_fallback) return join_next(s,id,row);
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*row_bytes),*buckets=next+o->hash_capacity;
    for(;;) {
        if(!o->have_left) {
            int64_t status=next_row(s,n->left,o->left);
            if(status!=2) {
                if(!status) { if(!o->hash_shared) cs_work_free(s->context,o->hash_data); o->hash_data=NULL; o->finished=1; }
                return status;
            }
            o->have_left=1; o->hash_matched=0;
            unsigned at=keys_null(n,o->left)?UINT_MAX:buckets[row_hash(n,o->left,0)&(2*o->hash_capacity-1)];
            o->hash_match=at==UINT_MAX?0:at+1;
        }
        while(o->hash_match) {
            unsigned at=o->hash_match-1; o->hash_match=next[at]==UINT_MAX?0:next[at]+1;
            hash_load(o->right,o->hash_data+(size_t)at*row_bytes,right_plan);
            const struct qs_cell *right=o->right;
            if(key_compare(s,n,o->left,right)) continue;
            int null=0; for(unsigned i=0;i<n->nkeys;++i) null|=right[n->right_keys[i]].isnull;
            if(null) continue;
            struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values)); memcpy(values+ln,right,rn*sizeof(*values));
            struct qs_eval e; init_eval(&e,&n->expr,values,NULL); struct qs_cell match={.value=1};
            if(n->join_qual>=0 && evaluate(s,&e,n->join_qual,&match)) return -5;
            if(match.isnull || !match.value) continue;
            o->hash_matched=1; ++s->metrics.join_rows;
            if(n->join_type==QP_ANTI) { o->hash_match=0; break; }
            if(n->join_type==QP_SEMI) { o->hash_match=0; o->have_left=0; }
            int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
        }
        int unmatched=!o->hash_matched && (n->join_type==QP_LEFT || n->join_type==QP_ANTI);
        o->have_left=0;
        if(unmatched) {
            struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
            for(unsigned i=0;i<rn;++i) values[ln+i]=(struct qs_cell){.isnull=1};
            int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
        }
    }
}
static int64_t spool_append(struct execution *s,struct spool *p,const struct qs_cell *row,unsigned cols)
{
    unsigned row_bytes=cols*sizeof(*row),block_rows=QS_SORT_IO_BYTES/row_bytes;
    if(!p->tail) {
        p->tail=cs_work_alloc(s->context,QS_SORT_IO_BYTES); p->loaded=UINT_MAX;
        if(!p->tail) return -4;
    }
    if(p->used==block_rows) {
        if(p->blocks==p->capacity) {
            unsigned capacity=p->capacity?p->capacity*2:8;
            void *tokens=cs_work_realloc(s->context,p->tokens,capacity*sizeof(*p->tokens)); if(!tokens) return -4;
            p->tokens=tokens; p->capacity=capacity;
        }
        memcpy(s->io,p->tail,QS_SORT_IO_BYTES); s->pending_spool=p; s->pending_store=1;
        return csNeedStore(QS_SORT_IO_BYTES);
    }
    memcpy(p->tail+(size_t)p->used*row_bytes,row,row_bytes); ++p->used; ++p->rows; return 0;
}
static int64_t spool_row(struct execution *s,struct spool *p,uint64_t index,unsigned cols,struct qs_cell *row)
{
    unsigned row_bytes=cols*sizeof(*row),block_rows=QS_SORT_IO_BYTES/row_bytes,block=index/block_rows;
    if(index>=p->rows) return -1;
    unsigned char *data=p->tail;
    if(block<p->blocks) {
        if(!p->cache) { p->cache=cs_work_alloc(s->context,QS_SORT_IO_BYTES); if(!p->cache) return -4; }
        if(p->loaded!=block) {
            s->pending_spool=p; s->pending_store=0; s->pending_block=block;
            return csNeedLoad(p->tokens[block]);
        }
        data=p->cache;
    }
    memcpy(row,data+(index%block_rows)*row_bytes,row_bytes); return 0;
}
static int64_t materialized_row(struct execution *s,unsigned id,uint64_t position,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned cols=s->plan->nodes[n->left].expr.nout;
    while(!o->group_ready) {
        if(!o->have_left) {
            int64_t status=next_row(s,n->left,o->left);
            if(status==QP_WAIT || status<0 || status>2) return status;
            if(!status) { o->group_ready=1; break; }
            o->have_left=1;
        }
        int64_t status=spool_append(s,&o->group,o->left,cols); if(status) return status;
        o->have_left=0;
    }
    if(position==o->group.rows) return 0;
    int64_t status=spool_row(s,&o->group,position,cols,row); return status?status:2;
}
static int64_t build_set(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id]; const struct qp_node *n=&s->plan->nodes[id];
    while(!o->set_ready) {
        int64_t status=next_row(s,n->left,o->left);
        if(status==QP_WAIT || status<0 || status>2) return status;
        if(!status) { o->set_ready=1; return 0; }
        struct qs_cell v=o->left[0];
        if(v.isnull) { o->set_null=1; continue; }
        if(v.value>INT64_MAX || v.value<INT64_MIN) return -5;
        if(!o->set_capacity || o->set_count*2>=o->set_capacity) {
            unsigned capacity=o->set_capacity?o->set_capacity*2:64;
            struct set_key *next=cs_work_calloc(s->context,capacity,sizeof(*next)); if(!next) return -4;
            for(unsigned i=0;i<o->set_capacity;++i) if(o->set[i].occupied) {
                unsigned slot=mix_hash((uint64_t)o->set[i].value)&(capacity-1);
                while(next[slot].occupied) slot=(slot+1)&(capacity-1);
                next[slot]=o->set[i];
            }
            if(o->set) cs_work_free(s->context,o->set);
            o->set=next; o->set_capacity=capacity;
        }
        unsigned slot=mix_hash((uint64_t)v.value)&(o->set_capacity-1);
        while(o->set[slot].occupied && o->set[slot].value!=v.value) slot=(slot+1)&(o->set_capacity-1);
        if(!o->set[slot].occupied) { o->set[slot]=(struct set_key){.value=v.value,.occupied=1}; ++o->set_count; }
    }
    return 0;
}
static int key_compare(struct execution *s,const struct qp_node *n,const struct qs_cell *left,const struct qs_cell *right)
{
    const struct qs_plan *types=&s->plan->nodes[n->left].expr;
    for(unsigned i=0;i<n->nkeys;++i) {
        unsigned col=n->left_keys[i];
        unsigned type=types->exprs[types->outputs[col].index].type;
        struct qs_cell a=left[col],b=right[n->right_keys[i]];
        int cmp=compare_cell(s,a,b,type);
        if(cmp==INT_MIN) return cmp;
        if(n->expr.norder) {
            if(a.isnull || b.isnull) { if(n->expr.order[i].nulls_first) cmp=-cmp; }
            else if(n->expr.order[i].descending) cmp=-cmp;
        }
        if(cmp) return cmp;
    }
    return 0;
}
static int keys_null(const struct qp_node *n,const struct qs_cell *left)
{ for(unsigned i=0;i<n->nkeys;++i) if(left[n->left_keys[i]].isnull) return 1; return 0; }
static int64_t join_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned ln=s->plan->nodes[n->left].expr.nout,rn=s->plan->nodes[n->right].expr.nout;
    struct qs_order right_order[QS_KEYS];
    for(unsigned i=0;i<n->expr.norder;++i) { right_order[i]=n->expr.order[i]; right_order[i].output=n->right_keys[i]; }
    const struct qs_order *left_order=n->expr.norder?n->expr.order:NULL;
    for(;;) {
        int64_t status;
        if(o->phase==2) {
            status=sorted_input(s,&o->left_sort,n->left,n->nkeys,n->left_keys,left_order,o->left);
            if(status!=2) return status;
            o->have_left=1; o->position=0; o->matched=0; o->phase=0;
            const struct qs_plan *types=&s->plan->nodes[n->left].expr;
            int same=1;
            for(unsigned i=0;i<n->nkeys;++i) {
                unsigned col=n->left_keys[i];
                int cmp=compare_cell(s,o->left[col],o->first[col],types->exprs[types->outputs[col].index].type);
                if(cmp==INT_MIN) return -5;
                if(cmp) same=0;
            }
            if(!same) { o->group_ready=0; o->group.rows=o->group.blocks=o->group.used=0; o->group.loaded=UINT_MAX; }
        }
        if(!o->have_left) {
            status=sorted_input(s,&o->left_sort,n->left,n->nkeys,n->left_keys,left_order,o->left);
            if(status!=2) return status;
            o->have_left=1; o->position=0; o->matched=0;
        }
        if(!o->group_ready) {
            if(!o->have_right && !o->input_done) {
                status=sorted_input(s,&o->right_sort,n->right,n->nkeys,n->right_keys,n->expr.norder?right_order:NULL,o->right);
                if(status==QP_WAIT || status<0 || status>2) return status;
                o->have_right=status==2; o->input_done=!status;
            }
            if(o->have_right && !keys_null(n,o->left)) {
                int cmp=key_compare(s,n,o->left,o->right); if(cmp==INT_MIN) return -5;
                if(cmp>0) { o->have_right=0; continue; }
                if(!cmp) {
                    status=spool_append(s,&o->group,o->right,rn); if(status) return status;
                    o->have_right=0; o->phase=1; continue;
                }
            }
            if(o->phase==1) { o->group_ready=1; o->phase=0; }
            else {
                o->have_left=0;
                if(n->join_type==QP_LEFT || n->join_type==QP_ANTI) {
                    struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
                    for(unsigned i=0;i<rn;++i) values[ln+i]=(struct qs_cell){.isnull=1};
                    int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
                }
                continue;
            }
        }
        if(o->position<o->group.rows) {
            struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
            status=spool_row(s,&o->group,o->position,rn,values+ln); if(status) return status;
            ++o->position;
            struct qs_eval e; init_eval(&e,&n->expr,values,NULL); struct qs_cell match={.value=1};
            if(n->join_qual>=0 && evaluate(s,&e,n->join_qual,&match)) return -5;
            if(match.isnull || !match.value) continue;
            o->matched=1;
            if(n->join_type==QP_ANTI) { o->position=o->group.rows; continue; }
            if(n->join_type==QP_SEMI) o->position=o->group.rows;
            int count=project(s,n,values,NULL,row); if(count<0) return count;
            if(count) { ++s->metrics.join_rows; return 2; }
            continue;
        }
        if(!o->matched && (n->join_type==QP_LEFT || n->join_type==QP_ANTI)) {
            struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
            for(unsigned i=0;i<rn;++i) values[ln+i]=(struct qs_cell){.isnull=1};
            o->matched=1;
            int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
        }
        /* Keep the right duplicate group for another left row with this key. */
        memcpy(o->first,o->left,ln*sizeof(*o->first)); o->have_left=0; o->phase=2;
    }
}
/* Reuse the existing selection-vector evaluator. String equality can compare
 * dictionary IDs; lexical ordering/LIKE and parameters retain scalar semantics. */
static int scan_filter_init(struct execution *s,const struct qs_plan *p,struct operator_state *o)
{
    if(!o->scan_filter_checked) {
        o->scan_filter_checked=1; uint8_t needed[QS_EXPRS]={0};
        if(p->predicate<0 || qs_vector_require(p,p->predicate,needed)) return 0;
        for(unsigned i=0;i<p->nexprs;++i) if(needed[i]) {
            const struct qs_expr *x=&p->exprs[i];
            if(x->op>=Q_LT && x->op<=Q_GE && p->exprs[x->left].type==Q_STRING) return 0;
        }
        o->scan_filter=cs_work_alloc(s->context,sizeof(*p));
        if(!o->scan_filter) return 0;
        *o->scan_filter=*p; o->scan_filter->work_mem_parent=(uintptr_t)s->context;
        for(unsigned i=0;i<p->nattrs;++i) if(p->cols[i].type==Q_DECIMAL && p->cols[i].slot>=0)
            o->scan_filter->cols[i].deferred=1;
        for(unsigned i=0;i<p->nexprs;++i) if(needed[i] && p->exprs[i].op==Q_CONST && p->exprs[i].type==Q_STRING && !p->exprs[i].isnull) {
            const struct qs_string *literal=&p->strings[p->exprs[i].constant];
            int id=qs_dict_insert(s->dictionary,literal->bytes,literal->length); if(id<0) return -3;
            o->scan_filter->exprs[i].constant=id;
        }
    }
    return 0;
}
static int64_t scan_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; const struct qs_plan *p=&n->expr; struct operator_state *o=&s->nodes[id];
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n));
    if(n->ordered_index && !o->index_control.state) {
        o->index_plan=cs_work_alloc(s->context,sizeof(*p)); if(!o->index_plan) return -4;
        *o->index_plan=*p; o->index_plan->work_mem_parent=(uintptr_t)s->context;
        o->index_plan->join.nattrs=p->nattrs; o->index_plan->join.work_mem_bytes=QS_INDEX_WORK_BYTES;
        int status=qs_index_init(o->index_plan,&o->index_control); if(status) return status;
        qs_index_dictionary(&o->index_control,s->dictionary);
    }
    for(;;) {
        if(!o->index_task.index_pending && o->scan_position<chunk->selected) {
            unsigned at=qs_selection(chunk)[o->scan_position++]; struct qs_cell values[QS_COLS];
            for(unsigned i=0;i<p->nattrs;++i) if(p->cols[i].slot>=0) {
                unsigned slot=p->cols[i].slot;
                values[i]=(struct qs_cell){.value=qs_values(chunk)[(size_t)slot*chunk->capacity+at],
                    .scale=p->cols[i].scale,.dscale=qs_dscales(chunk)[(size_t)slot*chunk->capacity+at],
                    .isnull=(qs_nulls(chunk)[(size_t)slot*((chunk->capacity+7)/8)+at/8]>>(at%8))&1};
            }
            int count=project_filtered(s,n,values,NULL,row,o->scan_filter!=NULL); if(count<0) return count;
            if(count) { ++s->metrics.matched; return 2; }
            continue;
        }
        if(n->ordered_index) {
            if(o->index_task.index_pending) {
                if(!s->control->ready) return QP_WAIT;
                s->control->ready=s->control->index_nreads=0;
            } else chunk->rows=chunk->selected=0;
            int64_t status=qs_index_scan(chunk,&o->index_task,&o->index_control,s->arena+n->input_offset,
                n->bytes/QS_PAGE,n->index_bytes/QS_PAGE,n->ordered_index);
            if(status==QS_INDEX_WAIT) {
                s->control->node=id; s->control->index_nreads=o->index_task.index_nreads;
                memcpy(s->control->index_reads,o->index_task.index_reads,sizeof(s->control->index_reads));
                return QP_WAIT;
            }
            if(status) return status;
            o->scan_position=0; s->metrics.scanned+=chunk->rows;
            if(!chunk->rows) return 0;
            continue;
        }
        if(s->control->ready && s->control->node==id && !s->control->index_nreads) {
            struct qs_task task={.input_bytes=s->control->bytes};
            int64_t status=scan_filter_init(s,p,o); if(status) return status;
            void *mr[]={s->arena+input_offset(s,n),o->scan_filter?o->scan_filter:(void *)p,chunk,&task,s->dictionary};
            long long len[]={qp_scan_buffer_bytes(s->plan,n),sizeof(*p),qs_chunk_bytes(chunk->capacity,chunk->ncols),sizeof(task),0};
            struct femu_csd_args a={.numr=5,.mr_addr=mr,.mr_len=len};
            status=timed_call(s,pg_sina_scan_impl,&a,QS_SCAN); if(status) return status;
            s->metrics.scanned+=chunk->rows; s->metrics.input_bytes+=task.input_bytes; ++s->metrics.chunks;
            if(o->scan_filter) {
                void *fm[]={chunk,o->scan_filter,mr[0]};
                long long fl[]={len[2],sizeof(*p),task.input_bytes};
                struct femu_csd_args filter={.numr=3,.mr_addr=fm,.mr_len=fl};
                status=timed_call(s,pg_sina_filter_impl,&filter,QS_FILTER);
                if(status==-4) {
                    /* Keep the query budget: if vector scratch does not fit,
                     * decode this retained input eagerly and use scalar SQL. */
                    cs_work_free(s->context,o->scan_filter); o->scan_filter=NULL; mr[1]=(void *)p;
                    status=timed_call(s,pg_sina_scan_impl,&a,QS_SCAN);
                }
                if(status) return status;
            }
            o->scan_position=0; o->position+=task.input_bytes; s->control->ready=0; s->control->bytes=0; continue;
        }
        if(s->worker && id==(unsigned)qp_probe_scan(s->plan,s->root))
            o->position=atomic_fetch_add_explicit(&s->owner->next_offset,s->plan->scan_bytes,memory_order_relaxed);
        if(o->position>=n->bytes) return 0;
        s->control->node=id; s->control->offset=o->position;
        s->control->bytes=n->bytes-o->position<s->plan->scan_bytes?n->bytes-o->position:s->plan->scan_bytes;
        return QP_WAIT;
    }
}
static int group_same(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    struct qs_eval e; init_eval(&e,&n->expr,o->left,NULL);
    for(unsigned k=0;k<n->expr.nkeys;++k) {
        struct qs_cell key; if(evaluate(s,&e,n->expr.keys[k],&key)) return -1;
        int cmp=compare_cell(s,key,o->keys[k],n->expr.exprs[n->expr.keys[k]].type);
        if(cmp==INT_MIN) return -1;
        if(cmp) return 0;
    }
    return 1;
}
static int accumulate(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    struct qs_eval e; init_eval(&e,&n->expr,o->left,NULL);
    for(unsigned i=0;i<n->expr.naggs;++i) {
        const struct qs_agg *agg=&n->expr.aggs[i]; struct accumulator *a=&o->accum[i]; struct qs_cell v={.value=1};
        if(agg->kind!=Q_COUNT_STAR && evaluate(s,&e,agg->expr,&v)) return -5;
        if(v.isnull) continue;
        if(n->distinct_aggs&(1U<<i)) {
            int cmp=1;
            if(a->has_last) cmp=compare_cell(s,a->last,v,n->expr.exprs[agg->expr].type);
            if(cmp==INT_MIN) return -5;
            if(!cmp) continue;
            a->last=v; a->has_last=1;
        }
        if(agg->kind==Q_MIN || agg->kind==Q_MAX) {
            int cmp=a->count?compare_cell(s,v,a->extreme,n->expr.exprs[agg->expr].type):0;
            if(cmp==INT_MIN) return -5;
            if(!a->count || (agg->kind==Q_MIN?cmp<0:cmp>0)) a->extreme=v;
        } else if(agg->kind==Q_SUM || agg->kind==Q_AVG) {
            if(!a->count) a->sum=v;
            else {
                unsigned scale=v.scale>a->sum.scale?v.scale:a->sum.scale; __int128 left,right;
                if(qs_scale(a->sum.value,scale-a->sum.scale,&left) || qs_scale(v.value,scale-v.scale,&right) ||
                    __builtin_add_overflow(left,right,&a->sum.value)) return -5;
                a->sum.scale=scale; if(v.dscale>a->sum.dscale) a->sum.dscale=v.dscale;
            }
        }
        ++a->count;
    }
    return 0;
}
static int64_t aggregate_next(struct execution *,unsigned,struct qs_cell *);
static int group_add(struct compact_group *g,struct qs_cell v,uint64_t count,unsigned kind)
{
    if(count && (kind==Q_SUM || kind==Q_AVG)) {
        if(!g->count) g->sum=v;
        else {
            unsigned scale=v.scale>g->sum.scale?v.scale:g->sum.scale; __int128 left,right;
            if(qs_scale(g->sum.value,scale-g->sum.scale,&left) || qs_scale(v.value,scale-v.scale,&right) ||
               __builtin_add_overflow(left,right,&g->sum.value)) return -5;
            g->sum.scale=scale; if(v.dscale>g->sum.dscale) g->sum.dscale=v.dscale;
        }
    }
    return __builtin_add_overflow(g->count,count,&g->count)?-5:0;
}
static int group_result(struct execution *s,const struct qp_node *n,struct compact_group *g,struct qs_cell *row)
{
    struct qs_cell input[QS_COLS],value={.isnull=!g->count};
    for(unsigned i=0;i<QS_COLS;++i) input[i]=(struct qs_cell){.isnull=1};
    input[n->left_keys[0]]=(struct qs_cell){.value=g->key,.isnull=g->occupied==2};
    unsigned kind=n->expr.aggs[0].kind;
    if(kind==Q_COUNT || kind==Q_COUNT_STAR) value=(struct qs_cell){.value=g->count};
    else if(g->count) {
        value=g->sum;
        if(kind==Q_AVG && qs_average(value,g->count,&value)) return -5;
    }
    int status=project(s,n,input,&value,row);
    return status<0?status:status?2:0;
}
/* A single integer grouping key and SUM/COUNT/AVG need only a key, exact sum
 * and count per group. Spill partial states, not the original heap rows. */
static int64_t hash_aggregate_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    struct sorted_rows *sort=&o->left_sort; unsigned kind=n->expr.aggs[0].kind;
    if(!o->group_capacity) {
        struct cs_work_context_stats memory;
        if(cs_work_get_context_stats(s->context,&memory)) return -4;
        size_t available=memory.limit_bytes-memory.used_bytes,reserve=2*s->sort_limit;
        available=available>reserve?available-reserve:available/2;
        unsigned capacity=128;
        while(capacity< n->estimated_rows*8/7 && (size_t)capacity*2*sizeof(*o->groups)<=available && capacity<UINT_MAX/2)
            capacity*=2;
        for(;;) {
            o->groups=cs_work_calloc(s->context,capacity,sizeof(*o->groups));
            if(o->groups || capacity==128) break;
            capacity/=2;
        }
        if(!o->groups) { n->hash=0; return aggregate_next(s,id,row); }
        o->group_capacity=capacity;
    }
    for(;;) {
        if(o->group_phase==0) {
            if(!o->have_left) {
                int64_t status=next_row(s,n->left,o->left);
                if(status==QP_WAIT || status<0 || status>2) return status;
                o->have_left=status==2;
                if(!status) { o->input_done=1; o->group_phase=1; continue; }
            }
            struct qs_cell key=o->left[n->left_keys[0]];
            if(!key.isnull && (key.scale || key.value<INT64_MIN || key.value>INT64_MAX)) return -5;
            unsigned occupied=key.isnull?2:1,mask=o->group_capacity-1;
            unsigned at=mix_hash(key.isnull?0:(uint64_t)key.value)&mask;
            while(o->groups[at].occupied && (o->groups[at].occupied!=occupied || (!key.isnull && o->groups[at].key!=(int64_t)key.value))) at=(at+1)&mask;
            struct compact_group *g=&o->groups[at];
            if(!g->occupied) {
                if(o->group_count>=o->group_capacity*7/8) { o->group_phase=1; continue; }
                g->occupied=occupied; g->key=key.isnull?0:key.value; g->sum.isnull=1; ++o->group_count;
            }
            struct qs_cell value={.value=1};
            if(kind!=Q_COUNT_STAR) value=o->left[n->expr.exprs[n->expr.aggs[0].expr].column];
            int status=group_add(g,value,!value.isnull,kind); if(status) return status;
            o->have_left=0; continue;
        }
        if(o->group_phase==1) {
            if(o->input_done && !o->hash_fallback) { o->group_phase=3; continue; }
            o->hash_fallback=1;
            struct qs_plan partial={.nout=3};
            partial.outputs[0].type_oid=20; partial.outputs[1].type_oid=1700; partial.outputs[2].type_oid=20;
            int key=0; int64_t status=sort_init(s,sort,&partial,1,&key,NULL); if(status) return status;
            if(sort->input_pending) {
                status=sort_call(s,sort,0); if(status) return status;
                sort->input_pending=0; sort->input->rows=0;
            }
            while(o->group_flush<o->group_capacity && sort->input->rows<QS_RESULT_ROWS) {
                struct compact_group *g=&o->groups[o->group_flush++]; if(!g->occupied) continue;
                struct qs_cell *out=sort->input->data[sort->input->rows++].cells;
                out[0]=(struct qs_cell){.value=g->key,.isnull=g->occupied==2};
                out[1]=g->sum; out[2]=(struct qs_cell){.value=g->count};
            }
            if(sort->input->rows) { sort->input_pending=1; continue; }
            if(o->input_done) {
                cs_work_free(s->context,o->groups); o->groups=NULL;
                sort->complete=1; sort->group.count=1; sort->group.states[0]=sort->task.state;
                cs_work_free(s->context,sort->input); sort->input=NULL;
                o->group_phase=2;
            } else {
                memset(o->groups,0,(size_t)o->group_capacity*sizeof(*o->groups));
                o->group_count=o->group_flush=0; o->group_phase=0;
            }
            continue;
        }
        if(o->group_phase==3) {
            while(o->group_flush<o->group_capacity) {
                struct compact_group *g=&o->groups[o->group_flush++]; if(!g->occupied) continue;
                return group_result(s,n,g,row);
            }
            cs_work_free(s->context,o->groups); o->groups=NULL; o->finished=1; return 0;
        }
        /* Merge partial SUM/count pairs; AVG is divided only for final output. */
        if(!o->have_right && !o->group_ready) {
            if((s->pending_sort==sort || sort->position==sort->output->rows) && !sort->drained) {
                int64_t status=sort_call(s,sort,1); if(status) return status;
                sort->position=0; sort->drained=sort->output->finished;
            }
            if(sort->position<sort->output->rows) {
                memcpy(o->right,sort->output->data[sort->position++].cells,3*sizeof(*o->right)); o->have_right=1;
            } else o->group_ready=1;
        }
        if(!o->phase) {
            if(!o->have_right) { cs_work_free(s->context,sort->output); sort->output=NULL; o->finished=1; return 0; }
            o->keys[0]=o->right[0]; memset(&o->accum[0],0,sizeof(o->accum[0])); o->accum[0].sum.isnull=1; o->phase=1;
        }
        struct compact_group g={.key=o->keys[0].value,.occupied=o->keys[0].isnull?2:1,.sum=o->accum[0].sum,.count=o->accum[0].count};
        if(o->have_right && o->right[0].isnull==o->keys[0].isnull && (o->keys[0].isnull || o->right[0].value==o->keys[0].value)) {
            int status=group_add(&g,o->right[1],o->right[2].value,kind); if(status) return status;
            o->accum[0].sum=g.sum; o->accum[0].count=g.count; o->have_right=0; continue;
        }
        o->phase=0; return group_result(s,n,&g,row);
    }
}
static int64_t aggregate_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    if(n->hash) return hash_aggregate_next(s,id,row);
    for(;;) {
        if(!o->have_left && !o->input_done) {
            int64_t status;
            if(n->nkeys && (!n->sorted_input || n->distinct)) {
                int keys[QS_KEYS+1]; memcpy(keys,n->left_keys,n->nkeys*sizeof(*keys)); unsigned count=n->nkeys;
                if(n->distinct) keys[count++]=n->distinct-1;
                status=sorted_input(s,&o->left_sort,n->left,count,keys,NULL,o->left);
            } else status=next_row(s,n->left,o->left);
            if(status==QP_WAIT || status<0 || status>2) return status;
            o->have_left=status==2; o->input_done=!status;
        }
        if(!o->phase) {
            if(!o->have_left && (n->nkeys || o->emitted)) return 0;
            memset(o->accum,0,sizeof(o->accum));
            if(o->have_left) {
                memcpy(o->first,o->left,sizeof(o->first));
                struct qs_eval e; init_eval(&e,&n->expr,o->left,NULL);
                for(unsigned k=0;k<n->expr.nkeys;++k) if(evaluate(s,&e,n->expr.keys[k],&o->keys[k])) return -5;
            }
            o->phase=1;
        }
        if(o->have_left) {
            int same=group_same(s,n,o); if(same<0) return -5;
            if(same) {
                int status=accumulate(s,n,o); if(status) return status;
                o->have_left=0; continue;
            }
        }
        struct qs_cell values[QS_AGGS];
        for(unsigned i=0;i<n->expr.naggs;++i) {
            const struct qs_agg *agg=&n->expr.aggs[i]; const struct accumulator *a=&o->accum[i];
            struct qs_cell v={.isnull=!a->count};
            if(agg->kind==Q_COUNT || agg->kind==Q_COUNT_STAR) v=(struct qs_cell){.value=a->count};
            else if(a->count) {
                v=agg->kind==Q_MIN || agg->kind==Q_MAX?a->extreme:a->sum;
                if(agg->kind==Q_AVG && qs_average(v,a->count,&v)) return -5;
            }
            values[i]=v;
        }
        o->phase=0; ++o->emitted;
        int count=project(s,n,o->first,values,row); if(count<0) return count;
        if(count) return 2;
    }
}

/* index_scan.c calls this consumer with a batch of decoded heap matches.
 * Only the lookup key is narrowed to int64; left payloads keep 128-bit cells. */
int64_t qs_join_consume(unsigned stage,struct qs_chunk *chunk,const struct qs_plan *unused,
    struct qs_task *task,struct qs_join_control *control,struct femu_csd_args *args,int resume)
{
    (void)stage; (void)unused; (void)task; (void)control; (void)args; (void)resume;
    struct execution *s=index_execution; unsigned id=index_node;
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    struct operator_state *o=&s->nodes[id]; unsigned ln=s->plan->nodes[n->left].expr.nout;
    if(o->index_flush) { o->index_flush=0; return 0; }
    o->index_output->rows=0; o->index_position=0;
    for(unsigned r=0;r<chunk->selected;++r) {
        unsigned at=qs_selection(chunk)[r]; struct qs_cell raw[QS_COLS],values[QS_COLS];
        unsigned source=qs_values(chunk)[chunk->capacity+at];
        if(source>=o->index_count) return -1;
        memcpy(values,o->index_left+(size_t)source*ln,ln*sizeof(*values));
        for(unsigned col=0;col<right->expr.nattrs;++col) if(right->expr.cols[col].slot>=0) {
            unsigned slot=2+right->expr.cols[col].slot;
            raw[col]=(struct qs_cell){.value=qs_values(chunk)[(size_t)slot*chunk->capacity+at],
                .scale=right->expr.cols[col].scale,.dscale=qs_dscales(chunk)[(size_t)slot*chunk->capacity+at],
                .isnull=(qs_nulls(chunk)[(size_t)slot*((chunk->capacity+7)/8)+at/8]>>(at%8))&1};
        }
        int status=project(s,right,raw,NULL,values+ln); if(status<0) return status; if(!status) continue;
        struct qs_eval e; init_eval(&e,&n->expr,values,NULL); struct qs_cell match={.value=1};
        if(n->join_qual>=0 && evaluate(s,&e,n->join_qual,&match)) return -5;
        if(match.isnull || !match.value) continue;
        uint64_t bit=UINT64_C(1)<<source;
        if(n->join_type==QP_SEMI && (o->index_matches&bit)) continue;
        o->index_matches|=bit; ++s->metrics.join_rows;
        if(n->join_type==QP_ANTI) continue;
        status=project(s,n,values,NULL,o->index_output->data[o->index_output->rows].cells);
        if(status<0) return status;
        if(status) ++o->index_output->rows;
    }
    if(n->join_type==QP_SEMI || n->join_type==QP_ANTI) qs_index_skip(&o->index_control,0,o->index_matches);
    if(o->index_output->rows) { o->index_flush=1; return QP_INDEX_YIELD; }
    return 0;
}
static int64_t index_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    struct operator_state *o=&s->nodes[id]; unsigned ln=s->plan->nodes[n->left].expr.nout;
    if(!o->index_plan) {
        struct qs_plan *p=cs_work_calloc(s->context,1,sizeof(*p));
        o->index_output=cs_work_calloc(s->context,1,sizeof(*o->index_output));
        o->index_left=cs_work_alloc(s->context,64*ln*sizeof(*o->index_left));
        if(!p || !o->index_output || !o->index_left) return -4;
        o->index_plan=p; p->nattrs=right->expr.nattrs+2; p->ncols=right->expr.ncols+2; p->njoins=1;
        p->work_mem_parent=(uintptr_t)s->context;
        p->join=right->expr.join; p->join.relid=right->expr.relid; p->join.relfilenode=right->expr.relfilenode;
        p->join.nattrs=right->expr.nattrs;
        p->join.semi=n->join_type==QP_SEMI || n->join_type==QP_ANTI?-1:0;
        p->join.probe_key=0; p->join.predicate=-1; p->join.work_mem_bytes=QS_INDEX_WORK_BYTES;
        ++p->join.build_key;
        p->cols[0]=(struct qs_column){.slot=0,.type=Q_INTEGER,.length=8,.align=8};
        p->cols[1]=p->cols[0]; p->cols[1].slot=1;
        for(unsigned i=0;i<right->expr.nattrs;++i) {
            p->cols[i+2]=right->expr.cols[i];
            if(p->cols[i+2].slot>=0) p->cols[i+2].slot+=2;
        }
        int status=qs_index_init(p,&o->index_control); if(status) return status;
        qs_index_dictionary(&o->index_control,s->dictionary);
        struct qs_chunk *input=(void *)(s->arena+vector_offset(s,n)),*output=(void *)(s->arena+vector_offset(s,n)+4096);
        *input=(struct qs_chunk){.capacity=64,.ncols=2};
        *output=(struct qs_chunk){.capacity=256,.ncols=p->ncols};
    }
    for(;;) {
        if(o->index_position<o->index_output->rows) {
            memcpy(row,o->index_output->data[o->index_position++].cells,n->expr.nout*sizeof(*row)); return 2;
        }
        o->index_output->rows=0; o->index_position=0;
        if(o->phase==2) {
            while(o->index_unmatched<o->index_count) {
                unsigned at=o->index_unmatched++;
                if((n->join_type!=QP_LEFT && n->join_type!=QP_ANTI) || (o->index_matches&(UINT64_C(1)<<at))) continue;
                struct qs_cell values[QS_COLS]; memcpy(values,o->index_left+(size_t)at*ln,ln*sizeof(*values));
                for(unsigned i=0;i<right->expr.nout;++i) values[ln+i]=(struct qs_cell){.isnull=1};
                int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
            }
            o->index_count=0; o->phase=0;
        }
        if(!o->phase) {
            unsigned batch=n->sorted_input?1:64;
            while(o->index_count<batch && !o->input_done) {
                int64_t status=next_row(s,n->left,o->index_left+(size_t)o->index_count*ln);
                if(status==QP_WAIT || status<0 || status>2) return status;
                if(!status) o->input_done=1; else ++o->index_count;
            }
            if(!o->index_count) return 0;
            o->index_matches=0; o->index_resume=0; o->index_flush=0; o->index_unmatched=0; o->phase=1;
            struct qs_chunk *input=(void *)(s->arena+vector_offset(s,n));
            input->rows=input->selected=o->index_count; memset(qs_nulls(input),0,16);
            for(unsigned i=0;i<o->index_count;++i) {
                struct qs_cell key=o->index_left[(size_t)i*ln+n->left_keys[0]];
                if(key.value>INT64_MAX || key.value<INT64_MIN) return -5;
                qs_values(input)[i]=key.value; qs_values(input)[64+i]=i;
                if(key.isnull) qs_nulls(input)[i/8]|=1U<<(i%8);
                qs_selection(input)[i]=i;
            }
        }
        if(s->control->ready && s->control->node==id && s->control->index_nreads) {
            s->control->ready=s->control->index_nreads=0;
        }
        struct qs_plan *p=o->index_plan;
        void *mr[]={s->arena+vector_offset(s,n),p,&o->index_task,&o->index_control,
            s->arena+vector_offset(s,n)+4096,s->arena+input_offset(s,n)};
        long long len[]={qs_chunk_bytes(64,2),sizeof(*p),sizeof(o->index_task),sizeof(o->index_control),
            qs_chunk_bytes(256,p->ncols),QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE};
        struct femu_csd_args a={.numr=6,.mr_addr=mr,.mr_len=len};
        /* A self-join can reuse the input pages already retained by its scan.
         * This is the current morsel only; advancing the scan updates the window. */
        int scan=qp_probe_scan(s->plan,n->left);
        if(scan>=0 && right->expr.relid && s->plan->nodes[scan].expr.relid==right->expr.relid &&
           s->plan->nodes[scan].expr.relfilenode==right->expr.relfilenode && s->nodes[scan].position) {
            uint64_t end=s->nodes[scan].position,begin=(end-1)/s->plan->scan_bytes*s->plan->scan_bytes;
            qs_index_heap_window(&o->index_control,0,s->arena+input_offset(s,&s->plan->nodes[scan]),begin/QS_PAGE,(end-begin)/QS_PAGE);
        }
        index_execution=s; index_node=id;
        int64_t status=qs_index_probe(0,mr[0],p,&o->index_task,&o->index_control,&a,o->index_resume);
        o->index_resume=1;
        if(status==QP_INDEX_YIELD) continue;
        if(status==QS_INDEX_WAIT && o->index_task.index_pending) {
            s->control->node=id; s->control->index_nreads=o->index_task.index_nreads;
            memcpy(s->control->index_reads,o->index_task.index_reads,s->control->index_nreads*sizeof(s->control->index_reads[0]));
            return QP_WAIT;
        }
        if(status) return status;
        o->phase=2;
    }
}
static int64_t next_row_impl(struct execution *s,unsigned id,struct qs_cell *row)
{
    if(id>=s->plan->count) return -1;
    struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    if(o->finished) return 0;
    if(!s->worker && s->plan->workers>1 && n->parallel) return parallel_next(s,id,row);
    if(n->kind==QP_SCAN) return scan_next(s,id,row);
    if(n->kind==QP_JOIN) return n->hash?hash_next(s,id,row):join_next(s,id,row);
    if(n->kind==QP_INDEX_JOIN) return index_next(s,id,row);
    if(n->kind==QP_AGGREGATE) return aggregate_next(s,id,row);
    if(n->kind==QP_PROJECT && n->left<0) {
        if(o->emitted++) return 0;
        int count=project(s,n,NULL,NULL,row);
        return count<0?count:count?2:0;
    }
    if(n->kind==QP_MATERIALIZED) {
        int64_t status=materialized_row(s,id,o->position,row); if(status==2) ++o->position; return status;
    }
    for(;;) {
        int64_t status;
        if(n->kind==QP_SORT) status=sorted_input(s,&o->left_sort,n->left,n->expr.norder,NULL,n->expr.order,o->left);
        else if(n->kind==QP_CTE_SCAN) {
            status=materialized_row(s,n->left,o->position,o->left); if(status==2) ++o->position;
        } else {
            if(n->kind==QP_LIMIT && n->expr.limit>=0 && o->emitted>=(uint64_t)n->expr.limit) return 0;
            status=next_row(s,n->left,o->left);
        }
        if(status!=2) return status;
        if(n->kind==QP_LIMIT && o->skipped<(uint64_t)n->expr.offset) { ++o->skipped; continue; }
        int count=project(s,n,o->left,NULL,row); if(count<0) return count;
        if(count) { ++o->emitted; return 2; }
    }
}
/* Build the read-only side once, then give each CU its own probe cursor,
 * index cache and AFDM output. The host schedules these ordinary CSF calls. */
static void close_workers(struct execution *s)
{
    for(unsigned w=0;w<s->plan->workers;++w) if(s->workers[w]) {
        struct execution *e=s->workers[w];
        for(unsigned i=0;i<s->plan->count;++i) if(e->nodes[i].index_control.state) qs_index_close(&e->nodes[i].index_control);
        e->control->state=0; cs_work_end(e->context); s->workers[w]=NULL;
    }
}
static int64_t parallel_next(struct execution *s,unsigned root,struct qs_cell *row)
{
    struct qp_plan *p=s->plan;
    if(!s->parallel_started || s->parallel_node!=root) {
        int builds[QP_NODES]; unsigned count=0;
        for(int id=root;id>=0 && p->nodes[id].kind!=QP_SCAN;id=p->nodes[id].left)
            if(p->nodes[id].kind==QP_JOIN) builds[count++]=id;
        /* Finish deeper build dependencies first. If a parent falls back to
         * Merge Join, its left input must not start another parallel phase
         * while the parent's unfinished right input still owns the buffers. */
        for(unsigned i=count;i;--i) {
            unsigned id=builds[i-1];
            int64_t status=hash_build(s,id); if(status) return status;
            if(s->nodes[id].hash_fallback) {
                p->nodes[root].parallel=0;
                return next_row(s,root,row);
            }
        }
        for(unsigned w=0;w<p->workers;++w) {
            struct cs_work_context_stats stats;
            if(cs_work_get_context_stats(s->context,&stats)) return -4;
            cs_work_context *context=cs_work_begin_child(s->context,stats.limit_bytes);
            struct execution *e=context?cs_work_calloc(context,1,sizeof(*e)):NULL;
            if(e) e->nodes=cs_work_calloc(context,p->count,sizeof(*e->nodes));
            if(!e || !e->nodes) {
                if(context) cs_work_end(context);
                close_workers(s); p->nodes[root].parallel=0;
                return next_row(s,root,row);
            }
            e->context=context; e->plan=p; e->owner=s; e->worker=w+1; e->root=root;
            e->control=s->control+w+1; e->dictionary=s->dictionary; e->arena=s->arena;
            e->io=(char *)s->io+(w+1)*QS_SORT_IO_BYTES; e->io_bytes=QS_SORT_IO_BYTES;
            e->sort_limit=s->sort_limit; e->profiling=s->profiling; e->initializer=p->ninit;
            memcpy(e->params,s->params,sizeof(e->params));
            for(unsigned i=0;i<p->count;++i) {
                const struct operator_state *built=&s->nodes[i]; struct operator_state *local=&e->nodes[i];
                if(built->hash_phase) {
                    local->hash_data=built->hash_data; local->hash_rows=built->hash_rows;
                    local->hash_capacity=built->hash_capacity; local->hash_phase=1; local->hash_shared=1;
                }
                if(built->set_ready) {
                    local->set=built->set; local->set_capacity=built->set_capacity;
                    local->set_count=built->set_count; local->set_null=built->set_null; local->set_ready=1;
                }
            }
            *e->control=(struct qp_control){.state=(uintptr_t)e}; s->workers[w]=e;
            const struct qp_node *leaf=&p->nodes[qp_probe_scan(p,root)];
            *(struct qs_chunk *)(s->arena+vector_offset(e,leaf))=
                (struct qs_chunk){.capacity=qp_scan_buffer_bytes(p,leaf)/24,.ncols=leaf->expr.ncols};
        }
        s->parallel_node=root; s->parallel_done=0;
        memset(s->parallel_position,0,sizeof(s->parallel_position)); atomic_store(&s->next_offset,0);
        s->control->parallel=1; ++s->control->epoch; s->control->available=s->control->drained=0;
        s->parallel_started=1; s->used_workers=p->workers;
    }
    for(unsigned w=0;w<p->workers;++w) if(s->control->available&(1U<<w)) {
        struct qs_result *out=&s->outputs[w+1];
        if(s->parallel_position[w]<out->rows) {
            unsigned cols=p->nodes[root].expr.nout;
            memcpy(row,(struct qs_cell *)out->data+(size_t)s->parallel_position[w]++*cols,cols*sizeof(*row));
            return 2;
        }
        if(out->finished) s->parallel_done|=1U<<w;
        s->parallel_position[w]=0; s->control->available&=~(1U<<w); s->control->drained|=1U<<w;
    }
    if(s->parallel_done==((1U<<p->workers)-1)) {
        for(unsigned w=0;w<p->workers;++w) {
            const struct qs_result *v=&s->outputs[w+1];
            s->metrics.scanned+=v->metrics.scanned; s->metrics.matched+=v->metrics.matched;
            s->metrics.input_bytes+=v->metrics.input_bytes-v->metrics.index_pages*QS_PAGE;
            s->metrics.index_pages+=v->metrics.index_pages; s->metrics.index_tuples+=v->metrics.index_tuples;
            for(unsigned op=0;op<QS_OPERATORS;++op) {
                struct qs_timing *t=&s->profile.operators[op]; const struct qs_timing *x=&v->profile.operators[op];
                t->calls+=x->calls; t->wall_ns+=x->wall_ns; t->cpu_ns+=x->cpu_ns;
            }
        }
        close_workers(s);
        for(int id=root;id>=0 && p->nodes[id].kind!=QP_SCAN;id=p->nodes[id].left) if(p->nodes[id].kind==QP_JOIN) {
            cs_work_free(s->context,s->nodes[id].hash_data); s->nodes[id].hash_data=NULL;
        }
        s->parallel_started=0; s->control->parallel=0; s->nodes[root].finished=1; return 0;
    }
    s->control->parallel=1; return QP_WAIT;
}
static unsigned sort_depth(const struct qp_plan *plan,unsigned id)
{
    const struct qp_node *n=&plan->nodes[id]; unsigned left=n->left<0?0:sort_depth(plan,n->left),right=n->right<0?0:sort_depth(plan,n->right);
    if(n->kind==QP_JOIN) return (left>right?left:right)+2;
    return left+(n->kind==QP_SORT || (n->kind==QP_AGGREGATE && n->nkeys && (!n->sorted_input || n->distinct)));
}
static void drop_sort(struct sorted_rows *sort)
{
    if(!sort->initialized || !sort->task.state || sort->drained) return;
    struct qs_group group={.count=1,.states={sort->task.state}};
    void *mr[]={&group}; long long len[]={sizeof(group)};
    struct femu_csd_args a={.numr=1,.mr_addr=mr,.mr_len=len};
    pg_sina_sort_source_impl(&a); sort->task.state=0;
}
static void close_execution(struct execution *s)
{
    for(unsigned i=0;i<s->plan->count;++i) { drop_sort(&s->nodes[i].left_sort); drop_sort(&s->nodes[i].right_sort); }
    for(unsigned i=0;i<s->plan->count;++i) if(s->nodes[i].index_control.state) qs_index_close(&s->nodes[i].index_control);
    close_workers(s);
    cs_work_context *context=s->context; s->control->state=0; cs_work_end(context);
}
/* plan, lifecycle/page requests, result, input/dictionary arena, spill I/O.
 * A successful call either returns result rows or asks PG to LOAD pages.
 * NEED_STORE/LOAD is handled entirely by the existing SNIA batch scheduler. */
int64_t pg_sina_plan(struct femu_csd_args *a)
{
    if(!a || a->numr!=5 || a->mr_len[0]<(long long)sizeof(struct qp_plan) ||
        a->mr_len[1]<(long long)sizeof(struct qp_control) || a->mr_len[2]<(long long)sizeof(struct qs_result) ||
        a->mr_len[4]<QS_SORT_IO_BYTES) return -1;
    struct qp_plan *plan=a->mr_addr[0]; unsigned worker=(uint64_t)a->cparam1>>1;
    if(worker>plan->workers || a->mr_len[1]<(long long)((worker+1)*sizeof(struct qp_control)) ||
       a->mr_len[2]<(long long)((worker+1)*sizeof(struct qs_result))) return -1;
    struct qp_control *control=(struct qp_control *)a->mr_addr[1]+worker;
    struct qs_result *out=(struct qs_result *)a->mr_addr[2]+worker;
    struct execution *s=(void *)(uintptr_t)control->state;
    if(control->close) { if(s) close_execution(s); return 0; }
    if(!s) {
        if(worker) return -1;
        if(!plan->count || plan->count>QP_NODES || plan->root>=plan->count) return -1;
        struct cs_work_stats stats; if(cs_work_get_stats(&stats)) return -4;
        size_t budget=plan->nodes[plan->root].expr.work_mem_bytes;
        if(!budget || budget>stats.capacity_bytes) budget=stats.capacity_bytes;
        cs_work_context *context=cs_work_begin_limit(budget); if(!context) return -4;
        s=cs_work_calloc(context,1,sizeof(*s)); if(!s) { cs_work_end(context); return -4; }
        s->context=context; s->plan=plan; s->control=control; s->dictionary=a->mr_addr[3];
        s->arena=a->mr_addr[3]; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
        s->nodes=cs_work_calloc(context,plan->count,sizeof(*s->nodes));
        if(!s->nodes) { cs_work_end(context); return -4; }
        unsigned depth=sort_depth(plan,plan->root);
        s->sort_limit=budget/(depth+2); if(s->sort_limit<512*1024) { cs_work_end(context); return -4; }
        s->profiling=a->cparam1&1; s->outputs=a->mr_addr[2]; control->state=(uintptr_t)s;
    }
    s->args=a;
    /* SNIA may invoke us repeatedly inside one batch submission for spill.
     * PG only observes completion once, so retain all consumed-buffer bits. */
    if(!csIsReentry(a)) control->drained=0;
    if(s->pending_spool) {
        if(!csIsReentry(a)) return -1;
        struct spool *p=s->pending_spool;
        if(s->pending_store) { p->tokens[p->blocks++]=csReentryValue(a); p->used=0; s->metrics.spill_write_bytes+=QS_SORT_IO_BYTES; }
        else { memcpy(p->cache,s->io,QS_SORT_IO_BYTES); p->loaded=s->pending_block; s->metrics.spill_read_bytes+=QS_SORT_IO_BYTES; }
        s->pending_spool=NULL;
    }
    if(!csIsReentry(a)) { out->rows=out->begin=out->finished=out->str_bytes=0; out->status=0; }
    uint64_t start=s->profiling?now_ns(CLOCK_MONOTONIC):0,cpu=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
    struct execution *owner=worker?s->owner:s;
    if(s->profiling) {
        unsigned active=atomic_fetch_add(&owner->active_cus,1)+1,peak=atomic_load(&owner->peak_cus);
        while(active>peak && !atomic_compare_exchange_weak(&owner->peak_cus,&peak,active)) { }
    }
    int64_t status=0;
    while(s->initializer<plan->ninit) {
        unsigned index=s->initializer,node=plan->init_nodes[index]; int param=plan->init_params[index];
        if(param<0) {
            status=build_set(s,node); if(status) break;
            ++s->initializer; continue;
        }
        struct qs_cell value[QS_OUT];
        status=next_row(s,node,value);
        if(status==QP_WAIT || status<0 || status>2) break;
        if(!s->initializer_phase) {
            if(!status) { s->params[param]=(struct qs_cell){.isnull=1}; ++s->initializer; }
            else { s->params[param]=value[0]; s->initializer_phase=1; }
        } else {
            if(status==2) { status=-8; break; }
            s->initializer_phase=0; ++s->initializer;
        }
    }
    unsigned columns=plan->nodes[worker?(int)s->root:(int)plan->root].expr.nout;
    /* Internal buffers pack only the pipeline's projected columns. A two-column
     * pipeline fits 2048 rows in the same AFDM as 256 final sixteen-cell rows. */
    unsigned capacity=worker?QS_RESULT_ROWS*QS_OUT/(columns?columns:1):QS_RESULT_ROWS;
    while(status>=0 && status!=QP_WAIT && status<=2 && out->rows<capacity) {
        struct qs_cell *row=worker?(struct qs_cell *)out->data+(size_t)out->rows*columns:out->data[out->rows].cells;
        status=next_row(s,worker?(int)s->root:(int)plan->root,row);
        if(status==QP_WAIT || status<0 || status>2) break;
        if(!status) { out->finished=control->finished=1; break; }
        const struct qs_plan *root=&plan->nodes[plan->root].expr;
        for(unsigned col=0;!worker && col<root->nout;++col) if(!row[col].isnull && root->exprs[root->outputs[col].index].type==Q_STRING) {
            const char *bytes; unsigned length;
            if(qs_dict_get(s->dictionary,(unsigned)row[col].value,&bytes,&length) || length>255 || out->str_bytes+length>QS_RESULT_STRING_BYTES) return -3;
            if(length<=QS_STRING_MAX) qs_cell_set_string(&row[col],bytes,length);
            else {
                memcpy(out->str_data+out->str_bytes,bytes,length); qs_cell_set_string_ref(&row[col],out->str_bytes,length); out->str_bytes+=length;
            }
        }
        ++out->rows;
    }
    if(s->profiling) {
        struct qs_timing *t=&s->profile.operators[QS_JOIN_PROBE];
        ++t->calls; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-start; t->cpu_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
        atomic_fetch_sub(&owner->active_cus,1); control->peak_cus=atomic_load(&owner->peak_cus);
    }
    control->output_rows=out->rows;
    out->metrics=s->metrics; out->metrics.workers=s->used_workers?s->used_workers:1; out->profile=s->profile;
    for(unsigned i=0;i<plan->count;++i) if(plan->nodes[i].kind==QP_INDEX_JOIN || plan->nodes[i].ordered_index) {
        out->metrics.index_pages+=s->nodes[i].index_task.index_pages;
        out->metrics.index_tuples+=s->nodes[i].index_task.index_tuples;
    }
    out->metrics.input_bytes+=out->metrics.index_pages*QS_PAGE;
    struct cs_work_context_stats memory;
    if(!cs_work_get_context_stats(s->context,&memory)) out->metrics.work_peak=memory.peak_used_bytes;
    if(status<0) return status;
    if(out->finished && !worker) close_execution(s);
    return status==QP_WAIT?0:status>2?status:0;
}
