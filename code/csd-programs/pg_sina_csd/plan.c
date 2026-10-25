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
#define HASH_PARTITIONS 4
/* Bounded radix partitions use the existing SNIA spill protocol. A skewed
 * partition is probed in build batches, without sorting or rereading Heap. */
struct external_hash {
    struct spool build[HASH_PARTITIONS],probe[HASH_PARTITIONS];
    unsigned char *prefix;
    uint64_t prefix_rows,prefix_at,build_at,probe_at;
    unsigned phase,partition,direct;
};
struct accumulator { struct qs_cell sum,extreme,last; uint64_t count; int has_last; };
/* sum holds the total for SUM/AVG, or the selected value for MIN/MAX. */
struct group_value { struct qs_cell sum; uint64_t count; };
struct compact_group { int64_t key; atomic_uint occupied; struct group_value values[]; };
struct set_key { int64_t value; unsigned occupied; };
/* Private per-CU expression state; referenced input columns stay in AFDM. */
struct projection_batch {
    struct qs_plan plan;
    struct qs_vector *vectors;
    uint8_t needed[QS_EXPRS];
    unsigned scan,count,position,input_count;
    struct qs_cell *cells;
    int64_t error;
    unsigned error_node;
    int joined,keep_window,lazy_text;
    int group_keys[QS_KEYS],group_values[QS_AGGS];
};
struct hash_probe_batch {
    uint32_t matches[QS_VECTOR_ROWS];
    uint8_t errors[QS_VECTOR_ROWS];
    int ready;
};
struct operator_state {
    struct sorted_rows left_sort,right_sort;
    struct qs_cell left[QS_OUT],right[QS_OUT],first[QS_OUT],keys[QS_KEYS];
    struct accumulator accum[QS_AGGS];
    struct spool group;
    uint64_t position, skipped, emitted;
    unsigned scan_position, scan_input, scan_bytes,scan_raw,scan_raw_bytes;
    int defer_strings;
    struct qs_plan *scan_filter;
    int scan_filter_checked,projection_checked;
    struct projection_batch *projection;
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
    atomic_uint hash_rows;
    unsigned hash_capacity,hash_match,hash_phase,hash_buckets;
    int hash_matched,hash_fallback,hash_shared,hash_null_right;
    unsigned hash_payload; /* Columns still borrowed from the probe vector. */
    struct hash_probe_batch *hash_probe;
    struct external_hash *external;
    struct compact_group *groups;
    unsigned group_capacity,group_flush,group_phase,group_stride,group_integer;
    atomic_uint group_count;
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
    uint64_t local_profile_cpu, pipeline_start, pipeline_tick;
    uint64_t sink_ns,sink_rows;
    bool profiling, host_scan, scan_drained;
    struct femu_csd_args *args;
    unsigned initializer,initializer_phase;
    unsigned worker, parallel_position[QS_WORKERS];
    uint64_t parallel_done;
    int parallel_started;
    unsigned parallel_node, root, used_workers;
    /* One-based parent receiving rows directly from parallel workers. */
    unsigned hash_sink,group_sink,aggregate_sink;
    atomic_uint_fast64_t next_offset;
    atomic_uint active_cus, peak_cus;
    struct execution *owner, *workers[QS_WORKERS];
    struct qs_result *outputs;
    int partition_join,source_node;
    struct spool source;
    uint64_t source_at,source_end;
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
    uint64_t elapsed=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
    ++t->calls; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-wall; t->cpu_ns+=elapsed;
    s->local_profile_cpu+=elapsed;
    return status;
}
/* Blocking-call boundaries, never per input tuple. Child Scan/Sort and nested
 * blocking CPU is subtracted; merged worker counters are not thread-local CPU. */
struct blocking_timer { uint64_t wall,cpu,children; };
static struct blocking_timer blocking_begin(struct execution *s)
{
    return (struct blocking_timer){now_ns(CLOCK_MONOTONIC),now_ns(CLOCK_THREAD_CPUTIME_ID),s->local_profile_cpu};
}
static void blocking_end(struct execution *s,unsigned op,struct blocking_timer begin)
{
    uint64_t elapsed=now_ns(CLOCK_THREAD_CPUTIME_ID)-begin.cpu,children=s->local_profile_cpu-begin.children;
    uint64_t own=elapsed>children?elapsed-children:0;
    struct qs_timing *t=&s->profile.operators[op]; ++t->calls;
    t->cpu_ns+=own; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-begin.wall;
    s->local_profile_cpu+=own;
}
static int64_t next_row_impl(struct execution *,unsigned,struct qs_cell *);
static inline int64_t next_row(struct execution *s,unsigned id,struct qs_cell *row)
{
    int64_t status=next_row_impl(s,id,row);
    if(status<0 && !s->control->error) { s->control->error=status; s->control->error_node=id; }
    return status;
}
static int64_t join_next(struct execution *,unsigned,struct qs_cell *);
static int64_t hash_probe_input(struct execution *,unsigned,const uint32_t *);
static int hash_left_payload(struct execution *,unsigned);
static int hash_left_cell(struct execution *,unsigned,unsigned,struct qs_cell *);
static int64_t external_build(struct execution *,unsigned);
static int64_t external_probe(struct execution *,unsigned,struct qs_cell *);
static void external_advance(struct execution *,unsigned);
static int key_compare(struct execution *,const struct qp_node *,const struct qs_cell *,const struct qs_cell *);
static int keys_null(const struct qp_node *,const struct qs_cell *);
static uint64_t mix_hash(uint64_t);
static int64_t build_set(struct execution *,unsigned);
static _Thread_local struct execution *index_execution;
static _Thread_local unsigned index_node;
static _Thread_local struct projection_batch *index_columns;
static int64_t index_next(struct execution *,unsigned,struct qs_cell *,struct projection_batch *);
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
                            while(first<ln && character<begin) { first+=qs_text_character(left+first,ln-first); ++character; }
                            last=first;
                            while(last<ln && character<end) { last+=qs_text_character(left+last,ln-last); ++character; }
                            int id=qs_dict_insert(s->dictionary,left+first,last-first); if(id<0) return -3;
                            v.value=id;
                        }
                    } else {
                        if(qs_dict_get(s->dictionary,(unsigned)b.value,&right,&rn)) return -3;
                        if(x->op==QP_LIKE) { int match=qs_text_like(right,rn,left,ln); if(match<0) return -5; v.value=match; }
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
static uint64_t hash_key(uint64_t hash,struct qs_cell v)
{
    while(v.scale && !(v.value%10)) { v.value/=10; --v.scale; }
    return mix_hash(hash^(uint64_t)v.value^(uint64_t)((unsigned __int128)v.value>>64)^((uint64_t)v.scale<<56));
}
static uint64_t row_hash(const struct qp_node *n,const struct qs_cell *row,int right)
{
    uint64_t hash=0x9e3779b97f4a7c15ULL;
    for(unsigned i=0;i<n->nkeys;++i) hash=hash_key(hash,row[right?n->right_keys[i]:n->left_keys[i]]);
    return hash;
}
/* Row storage and bucket links have one writer per appended row. Bucket heads
 * are published atomically while the build CU tasks run; probing starts only
 * after all build tasks have completed. Reallocation happens at a wave barrier. */
static void hash_link(const struct qp_node *n,struct operator_state *o,
    const struct qs_plan *right,const struct qs_cell *row,unsigned at)
{
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*hash_row_bytes(right));
    uint32_t *buckets=next+o->hash_capacity;
    unsigned bucket=row_hash(n,row,1)&(o->hash_buckets-1);
    next[at]=__atomic_exchange_n(&buckets[bucket],at,__ATOMIC_RELAXED);
}
static void hash_reindex(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    unsigned rb=hash_row_bytes(right);
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*rb);
    o->hash_buckets=2*o->hash_capacity;
    memset(next+o->hash_capacity,0xff,(size_t)o->hash_buckets*sizeof(uint32_t));
    for(unsigned at=0;at<o->hash_rows;++at) {
        hash_load(o->right,o->hash_data+(size_t)at*rb,right);
        hash_link(n,o,right,o->right,at);
    }
}
static void hash_sink_batch(struct execution *s,struct qs_result *out)
{
    unsigned id=s->hash_sink-1;
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->owner->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    if(!o->hash_data || o->hash_fallback) return;
    unsigned at=atomic_load_explicit(&o->hash_rows,memory_order_relaxed),count;
    do {
        /* Leave one slot for the owner's pending next_row(). */
        count=o->hash_capacity-1-at;
        if(count>out->rows) count=out->rows;
        if(!count) return;
    } while(!atomic_compare_exchange_weak_explicit(&o->hash_rows,&at,at+count,
        memory_order_relaxed,memory_order_relaxed));
    unsigned rb=hash_row_bytes(right);
    for(unsigned i=0;i<count;++i) {
        const struct qs_cell *row=(struct qs_cell *)out->data+(size_t)i*right->nout;
        hash_store(o->hash_data+(size_t)(at+i)*rb,row,right);
        hash_link(n,o,right,row,at+i);
    }
    s->metrics.build_rows+=count; out->rows-=count;
    memmove(out->data,(struct qs_cell *)out->data+(size_t)count*right->nout,
        (size_t)out->rows*right->nout*sizeof(struct qs_cell));
}
static int64_t hash_build_impl(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    unsigned row_bytes=hash_row_bytes(right);
    if(o->external) return external_build(s,id);
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
        o->hash_capacity=capacity; hash_reindex(s,id);
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
                unsigned char *retained=prefix?prefix:o->hash_data;
                if(n->join_type==QP_INNER)
                    o->external=cs_work_calloc(s->context,1,sizeof(*o->external));
                if(o->external) {
                    o->external->prefix=retained; o->external->prefix_rows=o->hash_rows;
                    o->hash_data=NULL; o->hash_capacity=o->hash_rows=0; o->hash_fallback=2;
                    return external_build(s,id);
                }
                o->right_sort.prefix=retained; o->right_sort.prefix_rows=o->hash_rows;
                o->hash_data=NULL; o->hash_fallback=1; return 0;
            }
            o->hash_data=data; o->hash_capacity*=2; hash_reindex(s,id);
        }
        int64_t status=next_row(s,n->right,o->right);
        if(status==QP_WAIT || status<0 || status>2) return status;
        if(status==2) {
            unsigned at=o->hash_rows++;
            hash_store(o->hash_data+(size_t)at*row_bytes,o->right,right);
            hash_link(n,o,right,o->right,at); ++s->metrics.build_rows; continue;
        }
        o->hash_phase=1;
    }
    return 0;
}
static int64_t hash_build(struct execution *s,unsigned id)
{
    if(!s->profiling || s->nodes[id].hash_phase) return hash_build_impl(s,id);
    struct blocking_timer timer=blocking_begin(s);
    int64_t status=hash_build_impl(s,id); blocking_end(s,QS_JOIN_BUILD,timer); return status;
}
/* An Index Join above this producer can reuse the retained Heap window.
 * Flush before advancing that window, but finish any live vector/match chain. */
static int hash_input_boundary(struct execution *s,unsigned id)
{
    int scan=qp_probe_scan(s->plan,id); if(scan<0 || s->source_node>=0) return 0;
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,&s->plan->nodes[scan]));
    if(!chunk->rows || s->nodes[scan].scan_position<chunk->selected) return 0;
    for(int at=id;at>=0;at=s->plan->nodes[at].left) {
        const struct operator_state *o=&s->nodes[at];
        if(o->projection && ((at!=(int)id && o->projection->input_count) ||
            o->projection->position<o->projection->count)) return 0;
        /* An Index cursor owns lookup batches, not the scan's row window.
         * Its continuation must finish before another consumer uses that
         * boundary; do not infer an Index boundary from the distant scan. */
        if(s->plan->nodes[at].kind==QP_INDEX_JOIN) return 0;
        if(o->hash_match || (o->have_left && !o->hash_matched &&
            (s->plan->nodes[at].join_type==QP_LEFT || s->plan->nodes[at].join_type==QP_ANTI))) return 0;
        if(at==scan) return 1;
    }
    return 0;
}
#define QP_BATCH_END 4
static int64_t hash_match_next(struct execution *s,unsigned id,int boundary)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned rn=s->plan->nodes[n->right].expr.nout,ln=s->plan->nodes[n->left].expr.nout;
    const struct qs_plan *right_plan=&s->plan->nodes[n->right].expr;
    unsigned row_bytes=hash_row_bytes(right_plan);
restart:
    if(!o->hash_phase && o->hash_fallback!=1) { int64_t status=hash_build(s,id); if(status) return status; }
    if(o->finished) return 0;
    if(o->hash_fallback==1) return 3;
    o->hash_null_right=0;
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*row_bytes),*buckets=next+o->hash_capacity;
    for(;;) {
        if(!o->have_left) {
            if(boundary && hash_input_boundary(s,id)) return QP_BATCH_END;
            int64_t status=hash_probe_input(s,id,buckets);
            if(status!=2) {
                if(!status && o->external) { external_advance(s,id); goto restart; }
                if(!status) { if(!o->hash_shared) cs_work_free(s->context,o->hash_data); o->hash_data=NULL; o->finished=1; }
                return status;
            }
            o->have_left=1; o->hash_matched=0;
        }
        while(o->hash_match) {
            unsigned at=o->hash_match-1; o->hash_match=next[at]==UINT_MAX?0:next[at]+1;
            hash_load(o->right,o->hash_data+(size_t)at*row_bytes,right_plan);
            const struct qs_cell *right=o->right;
            if(key_compare(s,n,o->left,right)) continue;
            int null=0; for(unsigned i=0;i<n->nkeys;++i) null|=right[n->right_keys[i]].isnull;
            if(null) continue;
            if(n->join_type==QP_ANTI && n->join_qual<0) {
                o->hash_matched=1; o->hash_match=0; ++s->metrics.join_rows; break;
            }
            struct qs_cell match={.value=1};
            if(n->join_qual>=0) {
                int payload=hash_left_payload(s,id); if(payload) return payload;
                struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values)); memcpy(values+ln,right,rn*sizeof(*values));
                struct qs_eval e; init_eval(&e,&n->expr,values,NULL);
                if(evaluate(s,&e,n->join_qual,&match)) return -5;
            }
            if(match.isnull || !match.value) continue;
            o->hash_matched=1; ++s->metrics.join_rows;
            if(n->join_type==QP_ANTI) { o->hash_match=0; break; }
            if(n->join_type==QP_SEMI) { o->hash_match=0; o->have_left=0; }
            return 2;
        }
        int unmatched=!o->hash_matched && (n->join_type==QP_LEFT || n->join_type==QP_ANTI);
        o->have_left=0;
        if(unmatched) { o->hash_null_right=1; return 2; }
    }
}
/* Scalar adapter for unsupported projections and very small budgets. */
static int64_t hash_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned ln=s->plan->nodes[n->left].expr.nout,rn=s->plan->nodes[n->right].expr.nout;
    for(;;) {
        int64_t status=hash_match_next(s,id,0);
        if(status==3) return join_next(s,id,row);
        if(status!=2) return status;
        int payload=hash_left_payload(s,id); if(payload) return payload;
        struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
        for(unsigned col=0;col<rn;++col) values[ln+col]=o->hash_null_right?(struct qs_cell){.isnull=1}:o->right[col];
        int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
    }
}
static int64_t spool_append_bytes(struct execution *s,struct spool *p,const void *row,unsigned row_bytes)
{
    unsigned block_rows=QS_SORT_IO_BYTES/row_bytes;
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
static int64_t spool_row_bytes(struct execution *s,struct spool *p,uint64_t index,unsigned row_bytes,void *row)
{
    unsigned block_rows=QS_SORT_IO_BYTES/row_bytes,block=index/block_rows;
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
static int64_t spool_append(struct execution *s,struct spool *p,const struct qs_cell *row,unsigned cols)
{ return spool_append_bytes(s,p,row,cols*sizeof(*row)); }
static int64_t spool_row(struct execution *s,struct spool *p,uint64_t index,unsigned cols,struct qs_cell *row)
{ return spool_row_bytes(s,p,index,cols*sizeof(*row),row); }
static void external_advance(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    if(o->hash_data) cs_work_free(s->context,o->hash_data);
    o->hash_data=NULL; o->hash_rows=o->hash_capacity=o->hash_phase=o->hash_match=0;
    o->have_left=0; o->external->probe_at=0; o->external->phase=o->external->direct?4:2;
    if(o->external->direct) o->finished=1;
}
static int64_t external_probe(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct external_hash *e=s->nodes[id].external;
    const struct qs_plan *p=&s->plan->nodes[n->left].expr;
    if(e->direct) return next_row(s,n->left,row);
    struct spool *input=&e->probe[e->partition];
    if(e->probe_at==input->rows) return 0;
    unsigned char packed[QS_OUT*19];
    int64_t status=spool_row_bytes(s,input,e->probe_at,hash_row_bytes(p),packed); if(status) return status;
    hash_load(row,packed,p); ++e->probe_at; return 2;
}
static int64_t external_build(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    struct external_hash *e=o->external;
    if(e->phase>=3) return 0;
    const struct qs_plan *right=&s->plan->nodes[n->right].expr,*left=&s->plan->nodes[n->left].expr;
    unsigned rb=hash_row_bytes(right),lb=hash_row_bytes(left);
    unsigned char packed[QS_OUT*19];
    while(e->phase<2) {
        int build=e->phase==0;
        if(!o->have_right) {
            int64_t status;
            if(build && e->prefix_at<e->prefix_rows) {
                hash_load(o->right,e->prefix+(size_t)e->prefix_at++*rb,right); status=2;
            } else {
                if(e->prefix) { cs_work_free(s->context,e->prefix); e->prefix=NULL; }
                status=next_row(s,build?n->right:n->left,o->right);
            }
            if(status==QP_WAIT || status<0 || status>2) return status;
            if(!status) {
                ++e->phase;
                if(build) {
                    /* The blocking child has now released its table. If all
                     * partitions fit, load them and Probe the original
                     * input in parallel without partitioning it first. Only
                     * the bucket count needs a power-of-two capacity. */
                    uint64_t rows=0;
                    for(unsigned i=0;i<HASH_PARTITIONS;++i) rows+=e->build[i].rows;
                    if(!rows) { e->phase=4; o->finished=1; return 0; }
                    if(rows<=UINT_MAX/4) {
                        unsigned capacity=(rows+127)&~127U,buckets=256;
                        while(buckets<capacity*2) buckets*=2;
                        size_t bytes=(size_t)capacity*(rb+4)+(size_t)buckets*4;
                        struct cs_work_context_stats m;
                        if(cs_work_get_context_stats(s->context,&m)) return -4;
                        size_t reserve=2*QS_SORT_IO_BYTES;
                        if(s->plan->workers>1) reserve+=s->plan->workers*(sizeof(*s)+s->plan->count*sizeof(*s->nodes)+QS_SORT_IO_BYTES+256);
                        size_t available=m.limit_bytes-m.used_bytes;
                        if(available>reserve && bytes<=available-reserve &&
                           (o->hash_data=cs_work_alloc(s->context,bytes))) {
                            o->hash_capacity=capacity; o->hash_buckets=buckets; e->direct=1; e->phase=2;
                        }
                    }
                }
                continue;
            }
            o->have_right=1;
        }
        /* INNER join NULL keys cannot match, on either side. */
        int null=0;
        for(unsigned k=0;k<n->nkeys;++k) null|=o->right[build?n->right_keys[k]:n->left_keys[k]].isnull;
        if(!null) {
            unsigned part=(row_hash(n,o->right,build)>>32)&(HASH_PARTITIONS-1);
            hash_store(packed,o->right,build?right:left);
            int64_t status=spool_append_bytes(s,build?&e->build[part]:&e->probe[part],packed,build?rb:lb);
            if(status) return status;
        }
        o->have_right=0;
    }
    while(e->phase==2) {
        if(e->partition==HASH_PARTITIONS) {
            if(e->direct) break;
            e->phase=4; o->finished=1; return 0;
        }
        struct spool *build=&e->build[e->partition],*probe=&e->probe[e->partition];
        if(e->build_at==build->rows || (!e->direct && !probe->rows)) {
            if(build->cache) { cs_work_free(s->context,build->cache); build->cache=NULL; }
            if(build->tail) { cs_work_free(s->context,build->tail); build->tail=NULL; }
            if(probe->cache) { cs_work_free(s->context,probe->cache); probe->cache=NULL; }
            if(probe->tail) { cs_work_free(s->context,probe->tail); probe->tail=NULL; }
            ++e->partition; e->build_at=e->probe_at=0; continue;
        }
        if(!o->hash_data) {
            struct cs_work_context_stats m;
            if(cs_work_get_context_stats(s->context,&m)) return -4;
            size_t reserve=2*QS_SORT_IO_BYTES;
            if(s->plan->workers>1) reserve+=s->plan->workers*(sizeof(*s)+s->plan->count*sizeof(*s->nodes)+QS_SORT_IO_BYTES+256);
            size_t available=m.limit_bytes-m.used_bytes;
            available=available>reserve?available-reserve:available/2;
            unsigned capacity=128;
            while(capacity<build->rows-e->build_at && (size_t)capacity*2*(rb+12)<=available && capacity<UINT_MAX/4) capacity*=2;
            while(!(o->hash_data=cs_work_alloc(s->context,(size_t)capacity*(rb+12)))) {
                if(capacity==128) return -4;
                capacity/=2;
            }
            o->hash_capacity=capacity;
        }
        while(o->hash_rows<o->hash_capacity && e->build_at<build->rows) {
            int64_t status=spool_row_bytes(s,build,e->build_at,rb,o->hash_data+(size_t)o->hash_rows*rb);
            if(status) return status;
            ++e->build_at; ++o->hash_rows;
        }
        if(!e->direct) { o->hash_buckets=2*o->hash_capacity; break; }
    }
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*rb),*buckets=next+o->hash_capacity;
    memset(buckets,0xff,(size_t)o->hash_buckets*sizeof(*buckets));
    for(unsigned i=0;i<o->hash_rows;++i) {
        hash_load(o->right,o->hash_data+(size_t)i*rb,right);
        unsigned bucket=row_hash(n,o->right,1)&(o->hash_buckets-1);
        next[i]=buckets[bucket]; buckets[bucket]=i;
    }
    e->phase=3; o->hash_phase=1;
    return 0;
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
/* Filter retained Heap values by selection vectors. Byte views avoid interning
 * rejected strings; InitPlan parameters retain their full numeric precision. */
static int scan_filter_init(struct execution *s,const struct qs_plan *p,struct operator_state *o)
{
    if(!o->scan_filter_checked) {
        o->scan_filter_checked=1; uint8_t needed[QS_EXPRS]={0};
        if(p->predicate<0 && !o->defer_strings) return 0;
        struct qs_plan *filter=cs_work_alloc(s->context,sizeof(*p)); if(!filter) return 0;
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
            c->deferred=known?QS_FILTER_ONLY:QS_DEFERRED;
            for(unsigned i=0;i<filter->nexprs;++i) if(projected[i] && filter->exprs[i].op==Q_COLUMN && filter->exprs[i].column==(int)col)
                c->deferred=QS_DEFERRED;
        }
        for(unsigned i=0;i<filter->nexprs;++i) if(needed[i] && filter->exprs[i].op==Q_CONST && filter->exprs[i].type==Q_STRING && !filter->exprs[i].isnull) {
            const struct qs_string *literal=&filter->strings[filter->exprs[i].constant];
            int id=qs_dict_insert(s->dictionary,literal->bytes,literal->length);
            if(id<0) { cs_work_free(s->context,filter); return -3; }
            filter->exprs[i].constant=id;
        }
        o->scan_filter=filter; return 0;
scalar:
        cs_work_free(s->context,filter);
    }
    return 0;
}
static int64_t scan_chunk_next(struct execution *s,unsigned id)
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
        if(!o->index_task.index_pending && o->scan_position<chunk->selected) return 2;
        if(n->ordered_index) {
            if(s->worker && (n->ordered_index&8) && !o->input_done) {
                uint64_t first=atomic_fetch_add_explicit(&s->owner->next_offset,16*QS_PAGE,memory_order_relaxed)/QS_PAGE;
                unsigned pages=n->index_bytes/QS_PAGE;
                if(first>=pages) return 0;
                qs_index_scan_range(&o->index_control,first?first:1,first+16<pages?first+16:pages);
                o->input_done=1;
            }
            if(o->index_task.index_pending) {
                if(!s->control->ready) return QP_WAIT;
                s->control->ready=s->control->index_nreads=0;
            } else chunk->rows=chunk->selected=0;
            int64_t status=qs_index_scan(chunk,&o->index_task,&o->index_control,s->arena+input_offset(s,n),
                n->bytes/QS_PAGE,n->index_bytes/QS_PAGE,n->ordered_index);
            if(status==QS_INDEX_WAIT) {
                s->control->node=id; s->control->index_nreads=o->index_task.index_nreads;
                memcpy(s->control->index_reads,o->index_task.index_reads,sizeof(s->control->index_reads));
                return QP_WAIT;
            }
            if(status) return status;
            o->scan_position=0; s->metrics.scanned+=chunk->rows;
            if(!chunk->rows) {
                if(s->worker && (n->ordered_index&8)) { o->input_done=0; continue; }
                return 0;
            }
            continue;
        }
        if(s->control->ready && s->control->node==id && !s->control->index_nreads) {
            /* The retained heap input must not own the shared page-request
             * record: an Index Join may use that record between vectors. */
            o->scan_bytes=s->control->bytes;
            s->control->ready=s->control->bytes=0;
        }
        if(o->scan_bytes) {
            /* Keep the I/O block and fill vectors with complete heap pages.
             * Line-pointer counts conservatively include unused entries;
             * one page always fits the minimum capacity (QS_PAGE/24). */
            if(chunk->capacity<QS_PAGE/24 || o->scan_input>=o->scan_bytes) return -1;
            unsigned bytes=0,rows=0,remaining=o->scan_bytes-o->scan_input;
            if(remaining%QS_PAGE) return -1;
            const unsigned char *input=s->arena+input_offset(s,n)+o->scan_input;
            while(bytes<remaining) {
                uint16_t lower; memcpy(&lower,input+bytes+12,sizeof(lower));
                unsigned items=lower>=24?(lower-24)/4:0;
                if(bytes && rows+items>chunk->capacity) break;
                bytes+=QS_PAGE; rows+=items;
            }
            struct qs_task task={.input_bytes=bytes};
            int64_t status=scan_filter_init(s,p,o); if(status) return status;
            void *mr[]={s->arena+input_offset(s,n)+o->scan_input,o->scan_filter?o->scan_filter:(void *)p,chunk,&task,s->dictionary};
            long long len[]={bytes,sizeof(*p),qs_chunk_bytes(chunk->capacity,chunk->ncols),sizeof(task),0};
            struct femu_csd_args a={.numr=5,.mr_addr=mr,.mr_len=len};
            o->scan_raw=o->scan_input; o->scan_raw_bytes=bytes;
            status=timed_call(s,pg_sina_scan_impl,&a,QS_SCAN); if(status) return status;
            s->metrics.scanned+=chunk->rows; s->metrics.input_bytes+=task.input_bytes; ++s->metrics.chunks;
            if(o->scan_filter && p->predicate>=0) {
                void *fm[]={chunk,o->scan_filter,mr[0],s->dictionary,s->params};
                long long fl[]={len[2],sizeof(*p),task.input_bytes,0,sizeof(s->params)};
                struct femu_csd_args filter={.numr=5,.mr_addr=fm,.mr_len=fl};
                status=timed_call(s,pg_sina_filter_impl,&filter,QS_FILTER);
                if(status==-4) {
                    /* Keep the query budget: if vector scratch does not fit,
                     * decode this retained input eagerly and use scalar SQL. */
                    cs_work_free(s->context,o->scan_filter); o->scan_filter=NULL; mr[1]=(void *)p;
                    status=timed_call(s,pg_sina_scan_impl,&a,QS_SCAN);
                }
                if(status) return status;
            }
            o->scan_position=0; o->position+=task.input_bytes; o->scan_input+=task.input_bytes;
            if(o->scan_input==o->scan_bytes) o->scan_input=o->scan_bytes=0;
            continue;
        }
        if(s->host_scan && id==(unsigned)qp_probe_scan(s->plan,s->root)) {
            s->scan_drained=true;
            return o->position>=n->bytes?0:QP_WAIT;
        }
        if(s->worker && id==(unsigned)qp_probe_scan(s->plan,s->root))
            o->position=atomic_fetch_add_explicit(&s->owner->next_offset,s->plan->scan_bytes,memory_order_relaxed);
        if(o->position>=n->bytes) return 0;
        s->control->node=id; s->control->offset=o->position;
        s->control->bytes=n->bytes-o->position<s->plan->scan_bytes?n->bytes-o->position:s->plan->scan_bytes;
        return QP_WAIT;
    }
}
static int64_t scan_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; const struct qs_plan *p=&n->expr;
    struct operator_state *o=&s->nodes[id];
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n));
    for(;;) {
        int64_t status=scan_chunk_next(s,id); if(status!=2) return status;
        unsigned at=qs_selection(chunk)[o->scan_position++]; struct qs_cell values[QS_COLS];
        for(unsigned i=0;i<p->nattrs;++i) if(p->cols[i].slot>=0) {
            unsigned slot=p->cols[i].slot;
            values[i]=(struct qs_cell){.value=qs_values(chunk)[(size_t)slot*chunk->capacity+at],
                .scale=p->cols[i].scale,.dscale=qs_dscales(chunk)[(size_t)slot*chunk->capacity+at],
                .isnull=(qs_nulls(chunk)[(size_t)slot*((chunk->capacity+7)/8)+at/8]>>(at%8))&1};
        }
        int count=project_filtered(s,n,values,NULL,row,o->scan_filter!=NULL); if(count<0) return count;
        if(count) { ++s->metrics.matched; return 2; }
    }
}
/* Translate a projection chain into one expression DAG over the original
 * scan columns. This preserves PG's tree and avoids row materialization
 * between adjacent projections. Unsupported expressions keep the old path. */
struct projection_import {
    struct execution *execution;
    struct projection_batch *batch;
    unsigned nodes[QS_EXPRS],expressions[QS_EXPRS];
};
static int projection_import(struct projection_import *in,unsigned node,int index)
{
    const struct qp_node *n=&in->execution->plan->nodes[node];
    if(index<0 || (unsigned)index>=n->expr.nexprs) return -1;
    struct qs_plan *p=&in->batch->plan;
    for(unsigned i=0;i<p->nexprs;++i) if(in->nodes[i]==node && in->expressions[i]==(unsigned)index) return i;
    struct qs_expr e=n->expr.exprs[index];
    if(e.op>Q_CASE && e.op!=Q_YEAR && e.op!=Q_LIKE && e.op!=Q_SUBSTRING) return -1;
    if(e.op==Q_COLUMN && (n->kind==QP_PROJECT || n->kind==QP_AGGREGATE)) {
        const struct qp_node *child=&in->execution->plan->nodes[n->left];
        if(e.column<0 || (unsigned)e.column>=child->expr.nout) return -1;
        return projection_import(in,n->left,child->expr.outputs[e.column].index);
    }
    if(e.op!=Q_COLUMN && e.op!=Q_CONST) {
        if(e.left<0 || e.left>=index) return -1;
        e.left=projection_import(in,node,e.left); if(e.left<0) return -1;
        if(e.op!=Q_NOT && e.op!=Q_IS_NULL && e.op!=Q_IS_NOT_NULL && e.op!=Q_YEAR) {
            if(e.right<0 || e.right>=index) return -1;
            e.right=projection_import(in,node,e.right); if(e.right<0) return -1;
        }
        if(e.op==Q_CASE || e.op==Q_SUBSTRING) {
            if(e.otherwise<0 || e.otherwise>=index) return -1;
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
        int id=qs_dict_insert(in->execution->dictionary,v->bytes,v->length); if(id<0) return -1;
        e.constant=id;
    }
    if(p->nexprs==QS_EXPRS) return -1;
    unsigned at=p->nexprs++; p->exprs[at]=e; in->nodes[at]=node; in->expressions[at]=index;
    return at;
}
static int projection_init(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id]; if(o->projection_checked) return 0;
    o->projection_checked=1;
    const struct qp_node *root=&s->plan->nodes[id];
    int aggregate=root->kind==QP_AGGREGATE;
    if(aggregate && (root->distinct || root->distinct_aggs || root->left<0 ||
        (root->nkeys && (!root->hash || !qp_hash_aggregate_supported(root))))) return 0;
    int grouped=aggregate && root->nkeys;
    int scan=aggregate?root->left:(int)id;
    while(s->plan->nodes[scan].kind==QP_PROJECT) {
        const struct qp_node *n=&s->plan->nodes[scan];
        if(n->left<0 || n->expr.predicate>=0 || (!s->worker && s->plan->workers>1 && n->parallel)) return 0;
        scan=n->left;
    }
    const struct qp_node *leaf=&s->plan->nodes[scan];
    int joined=leaf->expr.predicate<0 &&
        (leaf->kind==QP_INDEX_JOIN || (leaf->kind==QP_JOIN && leaf->hash));
    if((leaf->kind!=QP_SCAN && !joined) || (!s->worker && s->plan->workers>1 && leaf->parallel)) return 0;
    if(!joined && leaf->expr.predicate>=0) {
        if(leaf->ordered_index) return 0;
        int status=scan_filter_init(s,&leaf->expr,&s->nodes[scan]); if(status) return status;
        if(!s->nodes[scan].scan_filter) return 0;
    }
    struct projection_batch *b=cs_work_calloc(s->context,1,sizeof(*b)); if(!b) return 0;
    b->plan=leaf->expr; b->plan.nexprs=0; b->plan.nout=grouped?s->plan->nodes[root->left].expr.nout:
        aggregate?root->expr.naggs:root->expr.nout; b->scan=scan; b->joined=joined;
    struct projection_import import={.execution=s,.batch=b};
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
    if(joined) {
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
    o->projection=b; return 0;
}
static void projection_append(struct projection_batch *b,const struct qs_cell *values)
{
    for(unsigned col=0;col<b->plan.nattrs;++col) {
        int slot=b->plan.cols[col].slot;
        if(slot>=0) b->cells[(size_t)slot*QS_VECTOR_ROWS+b->input_count]=values[col];
    }
    ++b->input_count;
}
static int64_t projection_join_fill(struct execution *s,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[b->scan]; struct operator_state *o=&s->nodes[b->scan];
    unsigned ln=s->plan->nodes[n->left].expr.nout;
    if(b->error) { s->control->error=b->error; s->control->error_node=b->error_node; return b->error; }
    if(n->kind==QP_INDEX_JOIN) {
        int previous=s->control->error; unsigned previous_node=s->control->error_node;
        int64_t status=index_next(s,b->scan,NULL,b);
        if(status<0 && b->input_count) {
            b->error=status; b->error_node=s->control->error?s->control->error_node:b->scan;
            s->control->error=previous; s->control->error_node=previous_node;
        } else if(status!=2) return status;
        goto evaluate_batch;
    }
    while(b->input_count<QS_VECTOR_ROWS) {
        int previous=s->control->error; unsigned previous_node=s->control->error_node;
        int64_t status=hash_match_next(s,b->scan,b->keep_window && b->input_count);
        if(status==QP_BATCH_END) break;
        if(status!=2) {
            /* The shared page request must be serviced before another
             * operator can issue I/O. Keep this partial column batch across
             * the wait instead of exposing it and requesting the page twice. */
            if(status==QP_WAIT || status>2) return status;
            if(status<0) {
                b->error=status; b->error_node=s->control->error?s->control->error_node:b->scan;
                s->control->error=previous; s->control->error_node=previous_node;
                if(!b->input_count) { s->control->error=status; s->control->error_node=b->error_node; return status; }
            }
            if(!b->input_count) return status;
            break;
        }
        for(unsigned col=0;col<b->plan.nattrs;++col) {
            int slot=b->plan.cols[col].slot; if(slot<0) continue;
            struct qs_cell value;
            if(col<ln) {
                int payload=hash_left_cell(s,b->scan,col,&value);
                if(payload) {
                    b->error=payload; b->error_node=b->scan;
                    if(!b->input_count) return payload;
                    goto evaluate_batch;
                }
            } else value=o->hash_null_right?(struct qs_cell){.isnull=1}:o->right[col-ln];
            b->cells[(size_t)slot*QS_VECTOR_ROWS+b->input_count]=value;
        }
        ++b->input_count;
    }
evaluate_batch:
    b->count=b->input_count; b->input_count=b->position=0;
    struct qs_vector_input input={.cells=b->cells,.dictionary=s->dictionary};
    /* COUNT(*) can have no payload columns; it still consumes cardinality. */
    if(b->plan.nexprs && qs_vector_execute_input(&b->plan,NULL,NULL,0,b->count,b->needed,b->vectors,input)) return -5;
    return 2;
}
/* Return 3 only for an internal scalar fallback, before consuming input. */
static int64_t projection_fill(struct execution *s,unsigned id)
{
    struct projection_batch *b=s->nodes[id].projection;
    if(b->position==b->count) {
        int64_t status=b->joined?projection_join_fill(s,b):scan_chunk_next(s,b->scan);
        if(status!=2) {
            if(!status || (b->joined && status==3)) {
                cs_work_free(s->context,b->cells);
                cs_work_free(s->context,b->vectors); cs_work_free(s->context,b);
                s->nodes[id].projection=NULL;
            }
            return status;
        }
        /* Filter may have fallen back after exhausting its scratch budget. */
        if(!b->joined && s->plan->nodes[b->scan].expr.predicate>=0 && !s->nodes[b->scan].scan_filter) {
            cs_work_free(s->context,b->vectors); cs_work_free(s->context,b);
            s->nodes[id].projection=NULL; return 3;
        }
        if(!b->joined) {
            struct operator_state *scan=&s->nodes[b->scan];
            struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,&s->plan->nodes[b->scan]));
            b->count=chunk->selected-scan->scan_position; if(b->count>QS_VECTOR_ROWS) b->count=QS_VECTOR_ROWS;
            const struct qp_node *source=&s->plan->nodes[b->scan];
            if(scan->defer_strings && source->expr.predicate<0)
                for(unsigned col=0;col<b->plan.nattrs;++col) if(b->plan.cols[col].type==Q_STRING)
                    b->plan.cols[col].deferred=scan->scan_filter?scan->scan_filter->cols[col].deferred:QS_EAGER;
            const void *raw=scan->scan_raw_bytes?s->arena+input_offset(s,source)+scan->scan_raw:NULL;
            if(qs_vector_execute_raw_dict(&b->plan,chunk,qs_selection(chunk),scan->scan_position,b->count,b->needed,b->vectors,raw,scan->scan_raw_bytes,s->dictionary)) return -5;
            scan->scan_position+=b->count; b->position=0;
        }
        /* A retained output must outlive this vector's byte views. Predicates
         * do not intern their temporary slices; projected text does. */
        for(unsigned o=0;!b->lazy_text && o<b->plan.nout;++o) {
            int index=b->plan.outputs[o].index; if(index<0) continue;
            if(qs_vector_materialize_text(&b->vectors[index],b->count,s->dictionary)) return -3;
        }
    }
    return 2;
}
static int64_t projection_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    int64_t status=projection_fill(s,id);
    if(status==3) return next_row_impl(s,id,row);
    if(status!=2) return status;
    struct projection_batch *b=s->nodes[id].projection;
    for(unsigned i=0;i<b->plan.nout;++i) {
        unsigned index=b->plan.outputs[i].index;
        if(b->vectors[index].errors[b->position]) return -5;
        row[i]=qs_vector_cell(&b->plan,b->vectors,index,b->position);
    }
    ++b->position; ++s->metrics.matched; return 2;
}
/* Probe the bucket array as a batch, keeping column payloads borrowed until
 * a candidate needs them. Match-chain state remains in the join operator, so
 * one input can produce arbitrarily many output batches without rereading it. */
static int64_t hash_probe_input(struct execution *s,unsigned id,const uint32_t *buckets)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    o->hash_payload=0;
    const struct qp_node *left=&s->plan->nodes[n->left];
    /* Aggregate projection state describes sink inputs, never final rows. */
    int eligible=(left->kind==QP_SCAN || left->kind==QP_PROJECT || left->kind==QP_JOIN || left->kind==QP_INDEX_JOIN) &&
        !o->external && s->source_node<0 && !(!s->worker && s->plan->workers>1 && left->parallel);
    if(eligible) {
        int status=projection_init(s,n->left); if(status) return status;
        struct projection_batch *b=s->nodes[n->left].projection;
        if(b && !o->hash_probe) {
            o->hash_probe=cs_work_calloc(s->context,1,sizeof(*o->hash_probe));
            if(o->hash_probe) {
                b->lazy_text=1;
                const struct qp_node *source=&s->plan->nodes[b->scan];
                if(!b->joined && !source->ordered_index && source->expr.predicate<0)
                    for(unsigned col=0;col<source->expr.nattrs;++col)
                        if(source->expr.cols[col].slot>=0 && source->expr.cols[col].type==Q_STRING)
                            s->nodes[b->scan].defer_strings=1;
            }
        }
        if(b && o->hash_probe) {
            int64_t ready=projection_fill(s,n->left);
            if(ready!=2) {
                if(ready!=3) {
                    if(!ready) { cs_work_free(s->context,o->hash_probe); o->hash_probe=NULL; }
                    return ready;
                }
                cs_work_free(s->context,o->hash_probe); o->hash_probe=NULL;
            } else {
                b=s->nodes[n->left].projection;
                struct hash_probe_batch *probe=o->hash_probe;
                if(!b->position || !probe->ready) {
                    for(unsigned k=0;k<n->nkeys;++k) {
                        int index=b->plan.outputs[n->left_keys[k]].index;
                        if(qs_vector_materialize_text(&b->vectors[index],b->count,s->dictionary)) return -3;
                    }
                    for(unsigned r=0;r<b->count;++r) {
                        uint64_t hash=UINT64_C(0x9e3779b97f4a7c15); unsigned null=0,error=0;
                        for(unsigned col=0;col<b->plan.nout;++col)
                            error|=b->vectors[b->plan.outputs[col].index].errors[r];
                        for(unsigned k=0;!error && k<n->nkeys;++k) {
                            int index=b->plan.outputs[n->left_keys[k]].index;
                            struct qs_cell key=qs_vector_cell(&b->plan,b->vectors,index,r);
                            null|=key.isnull; hash=hash_key(hash,key);
                        }
                        probe->matches[r]=null || error?UINT_MAX:hash&(o->hash_buckets-1);
                        probe->errors[r]=error;
                    }
                    /* Separate hashing from random bucket loads so the CPU
                     * can overlap independent lookups across this vector. */
                    for(unsigned r=0;r<b->count;++r) {
                        unsigned slot=probe->matches[r],at=slot==UINT_MAX?UINT_MAX:buckets[slot];
                        probe->matches[r]=at==UINT_MAX?0:at+1;
                    }
                    probe->ready=1;
                }
                unsigned r=b->position++;
                /* Report expression errors only when this input row is
                 * requested: a LIMIT may never consume later error rows. */
                if(probe->errors[r]) return -5;
                o->hash_payload=(1U<<b->plan.nout)-1;
                for(unsigned k=0;k<n->nkeys;++k) {
                    unsigned col=n->left_keys[k]; int index=b->plan.outputs[col].index;
                    o->left[col]=qs_vector_cell(&b->plan,b->vectors,index,r);
                    o->hash_payload&=~(1U<<col);
                }
                o->hash_match=probe->matches[r];
                ++s->metrics.matched; return 2;
            }
        }
    }
    int64_t status=o->external?external_probe(s,id,o->left):next_row(s,n->left,o->left);
    if(status==2) {
        unsigned at=keys_null(n,o->left)?UINT_MAX:buckets[row_hash(n,o->left,0)&(o->hash_buckets-1)];
        o->hash_match=at==UINT_MAX?0:at+1;
    }
    return status;
}
/* A payload text view becomes a canonical ID only for a matching row.
 * Cache it with the other retained cells for duplicate build-side matches. */
static int hash_left_cell(struct execution *s,unsigned id,unsigned col,struct qs_cell *value)
{
    struct operator_state *o=&s->nodes[id];
    if(o->hash_payload&(1U<<col)) {
        struct projection_batch *b=s->nodes[s->plan->nodes[id].left].projection;
        unsigned r=b->position-1; int index=b->plan.outputs[col].index;
        const struct qs_vector *v=&b->vectors[index];
        if(v->errors[r]) return -5;
        if(v->kind==QS_TEXT) {
            struct qs_cell cell={.isnull=v->nulls[r]};
            if(!cell.isnull) {
                int key=qs_dict_insert(s->dictionary,v->strings[r].data,v->strings[r].length); if(key<0) return -3;
                cell.value=key;
            }
            o->left[col]=cell;
        } else o->left[col]=qs_vector_cell(&b->plan,b->vectors,index,r);
        o->hash_payload&=~(1U<<col);
    }
    *value=o->left[col]; return 0;
}
static int hash_left_payload(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    for(unsigned columns=o->hash_payload;columns;columns&=columns-1) {
        unsigned col=__builtin_ctz(columns); struct qs_cell value;
        int status=hash_left_cell(s,id,col,&value); if(status) return status;
    }
    return 0;
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
static inline int accumulate_value(struct execution *s,const struct qp_node *n,
    const struct qs_agg *agg,struct accumulator *a,struct qs_cell v)
{
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
    return 0;
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
        int status=accumulate_value(s,n,agg,a,v); if(status) return status;
    }
    return 0;
}
static int64_t aggregate_next(struct execution *,unsigned,struct qs_cell *);
static int group_add(struct group_value *g,struct qs_cell v,uint64_t count,unsigned kind)
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
static int group_update(struct execution *s,const struct qp_node *n,unsigned aggregate,
    struct group_value *g,struct qs_cell value,uint64_t count)
{
    const struct qs_agg *a=&n->expr.aggs[aggregate];
    if(count && (a->kind==Q_MIN || a->kind==Q_MAX)) {
        int cmp=g->count?compare_cell(s,value,g->sum,n->expr.exprs[a->expr].type):0;
        if(cmp==INT_MIN) return -5;
        if(!g->count || (a->kind==Q_MIN?cmp<0:cmp>0)) g->sum=value;
    }
    return group_add(g,value,count,a->kind);
}
static struct compact_group *group_at(struct operator_state *o,unsigned slot)
{ return (void *)((unsigned char *)o->groups+(size_t)slot*o->group_stride); }
static void group_keys(const struct qp_node *n,const struct operator_state *o,const struct compact_group *g,struct qs_cell *keys)
{
    int wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
    const void *stored=g->values+n->expr.naggs;
    if(wide && o->group_integer) {
        const int64_t *values=stored;
        for(unsigned k=0;k<n->nkeys;++k) keys[k]=(struct qs_cell){.value=values[k],.isnull=!!(g->occupied&(2U<<k))};
    } else if(wide) memcpy(keys,stored,n->nkeys*sizeof(*keys));
    else keys[0]=(struct qs_cell){.value=g->key,.isnull=g->occupied==2};
}
static uint64_t group_hash(const struct qp_node *n,const struct qs_cell *keys)
{
    uint64_t hash=UINT64_C(0x9e3779b97f4a7c15);
    for(unsigned k=0;k<n->nkeys;++k) {
        struct qs_cell v=keys[k];
        if(v.isnull) v=(struct qs_cell){0};
        while(v.scale && !(v.value%10)) { v.value/=10; --v.scale; }
        hash=mix_hash(hash^(uint64_t)v.value^(uint64_t)((unsigned __int128)v.value>>64)^((uint64_t)v.scale<<56));
    }
    return hash;
}
static int group_equal(struct execution *s,const struct qp_node *n,const struct qs_cell *a,const struct qs_cell *b)
{
    for(unsigned k=0;k<n->nkeys;++k) {
        int cmp=compare_cell(s,a[k],b[k],n->expr.exprs[n->expr.keys[k]].type);
        if(cmp==INT_MIN) return -5;
        if(cmp) return 0;
    }
    return 1;
}
static int group_result(struct execution *s,const struct qp_node *n,const struct group_value *groups,
    const struct qs_cell *keys,struct qs_cell *row)
{
    struct qs_cell input[QS_COLS],values[QS_AGGS];
    for(unsigned i=0;i<QS_COLS;++i) input[i]=(struct qs_cell){.isnull=1};
    for(unsigned k=0;k<n->nkeys;++k) input[n->left_keys[k]]=keys[k];
    for(unsigned a=0;a<n->expr.naggs;++a) {
        const struct group_value *g=&groups[a]; unsigned kind=n->expr.aggs[a].kind;
        struct qs_cell value={.isnull=!g->count};
        if(kind==Q_COUNT || kind==Q_COUNT_STAR) value=(struct qs_cell){.value=g->count};
        else if(g->count) {
            value=g->sum;
            if(kind==Q_AVG && qs_average(value,g->count,&value)) return -5;
        }
        values[a]=value;
    }
    int status=project(s,n,input,values,row);
    return status<0?status:status?2:0;
}
/* Published keys stay immutable throughout a CU wave. Probe them without
 * locking collisions; only publication and the matched aggregate are locked.
 * Tables are shared instead of replicated per CU: four copies of Q17's groups
 * would consume the entire device budget before scans and merge state. The
 * owner alone grows/flushes the table between CU waves; full tables leave raw
 * rows in the ordinary AFDM buffer for that existing bounded spill path. */
#define GROUP_LOCK (1U<<31)
static int group_sink_values(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *keys,const struct group_value *values,int shared)
{
    const struct qp_node *n=&s->plan->nodes[id];
    int wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
    struct qs_cell key=keys[0];
    if(!wide && !key.isnull && (key.scale || key.value<INT64_MIN || key.value>INT64_MAX)) return -5;
    unsigned occupied=key.isnull?2:1;
    if(wide && o->group_integer) {
        occupied=1;
        for(unsigned k=0;k<n->nkeys;++k) {
            if(!keys[k].isnull && (keys[k].scale || keys[k].value<INT64_MIN || keys[k].value>INT64_MAX)) return -5;
            if(keys[k].isnull) occupied|=2U<<k;
        }
    }
    uint64_t hash=wide?group_hash(n,keys):mix_hash(key.isnull?0:(uint64_t)key.value);
    unsigned mask=o->group_capacity-1,at=hash&mask;
    for(;;at=(at+1)&mask) {
        struct compact_group *g=group_at(o,at);
retry_bucket:
        ;
        unsigned state=atomic_load_explicit(&g->occupied,shared?memory_order_acquire:memory_order_relaxed);
        /* A creator has not published its keys yet. An updater keeps the
         * occupied bits, so other probes can already compare immutable keys. */
        if(state==GROUP_LOCK) goto retry_bucket;
        unsigned old=state&~GROUP_LOCK;
        int equal=!old;
        if(old && wide && o->group_integer) {
            const int64_t *key_values=(const void *)(g->values+n->expr.naggs); equal=old==occupied;
            for(unsigned k=0;equal && k<n->nkeys;++k)
                if(!keys[k].isnull && key_values[k]!=(int64_t)keys[k].value) equal=0;
        } else if(old) equal=wide?group_equal(s,n,keys,(struct qs_cell *)(g->values+n->expr.naggs)):
            old==occupied && (key.isnull || g->key==(int64_t)key.value);
        if(equal<0) return equal;
        if(!equal) continue;
        if(shared) {
            if(state&GROUP_LOCK) goto retry_bucket;
            if(!atomic_compare_exchange_weak_explicit(&g->occupied,&state,state|GROUP_LOCK,
                memory_order_acquire,memory_order_relaxed)) goto retry_bucket;
        }
        if(!old) {
            unsigned count=atomic_load_explicit(&o->group_count,memory_order_relaxed);
            do {
                if(count>=o->group_capacity*7/8) {
                    if(shared) atomic_store_explicit(&g->occupied,0,memory_order_release);
                    return 0;
                }
            } while(!atomic_compare_exchange_weak_explicit(&o->group_count,&count,count+1,
                memory_order_relaxed,memory_order_relaxed));
            g->key=wide?(int64_t)hash:key.isnull?0:key.value;
            for(unsigned a=0;a<n->expr.naggs;++a) g->values[a].sum.isnull=1;
            if(wide && o->group_integer) {
                int64_t *key_values=(void *)(g->values+n->expr.naggs);
                for(unsigned k=0;k<n->nkeys;++k) key_values[k]=keys[k].isnull?0:(int64_t)keys[k].value;
            } else if(wide) memcpy(g->values+n->expr.naggs,keys,n->nkeys*sizeof(*keys));
        }
        int status=0;
        for(unsigned a=0;a<n->expr.naggs && !status;++a)
            status=group_update(s,n,a,&g->values[a],values[a].sum,values[a].count);
        atomic_store_explicit(&g->occupied,occupied,shared?memory_order_release:memory_order_relaxed);
        return status?status:1;
    }
}
static int group_sink_row(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *row,int shared)
{
    const struct qp_node *n=&s->plan->nodes[id];
    struct qs_cell keys[QS_KEYS]; struct group_value values[QS_AGGS];
    for(unsigned k=0;k<n->nkeys;++k) keys[k]=row[n->left_keys[k]];
    struct qs_eval eval; int initialized=0;
    for(unsigned a=0;a<n->expr.naggs;++a) {
        struct qs_cell value={.value=1};
        if(n->expr.aggs[a].kind!=Q_COUNT_STAR) {
            int index=n->expr.aggs[a].expr;
            if(n->expr.exprs[index].op==Q_COLUMN) value=row[n->expr.exprs[index].column];
            else {
                if(!initialized) { init_eval(&eval,&n->expr,row,NULL); initialized=1; }
                if(evaluate(s,&eval,index,&value)) return -5;
            }
        }
        values[a]=(struct group_value){.sum=value,.count=!value.isnull};
    }
    return group_sink_values(s,id,o,keys,values,shared);
}
/* Keep keys and aggregate arguments in vectors. Only rows rejected by a full
 * shared table enter the existing AFDM adapter, so its owner can grow/spill
 * between waves without losing the remainder of this vector. */
static int group_sink_vectors(struct execution *s,unsigned id,struct qs_result *out,unsigned capacity)
{
    struct projection_batch *b=s->nodes[id].projection;
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->owner->nodes[id];
    /* Unclustered keys would pay for comparisons without saving updates.
     * Inspect at most 16 adjacent rows before choosing local run combine. */
    int combine=0;
    unsigned sample=b->count-b->position; if(sample>16) sample=16;
    struct qs_cell previous[QS_KEYS];
    for(unsigned r=0;r<sample && !combine;++r) {
        struct qs_cell keys[QS_KEYS];
        for(unsigned k=0;k<n->nkeys;++k) {
            int index=b->group_keys[k]; if(b->vectors[index].errors[b->position+r]) return -5;
            keys[k]=qs_vector_cell(&b->plan,b->vectors,index,b->position+r);
        }
        if(r) { combine=group_equal(s,n,previous,keys); if(combine<0) return combine; }
        memcpy(previous,keys,n->nkeys*sizeof(*keys));
    }
    while(b->position<b->count && out->rows<capacity) {
        unsigned first=b->position,end=first;
        struct qs_cell keys[QS_KEYS];
        for(unsigned k=0;k<n->nkeys;++k) {
            int index=b->group_keys[k]; if(b->vectors[index].errors[first]) return -5;
            keys[k]=qs_vector_cell(&b->plan,b->vectors,index,first);
        }
        /* Combine adjacent equal keys locally before touching the shared
         * table. This needs only one partial state, not a table per CU. */
        struct group_value partial[QS_AGGS];
        for(unsigned a=0;a<n->expr.naggs;++a) partial[a]=(struct group_value){.sum={.isnull=1}};
        do {
            if(end!=first) {
                struct qs_cell next[QS_KEYS];
                for(unsigned k=0;k<n->nkeys;++k) {
                    int index=b->group_keys[k]; if(b->vectors[index].errors[end]) return -5;
                    next[k]=qs_vector_cell(&b->plan,b->vectors,index,end);
                }
                int equal=group_equal(s,n,keys,next); if(equal<0) return equal;
                if(!equal) break;
            }
            for(unsigned a=0;a<n->expr.naggs;++a) {
                struct qs_cell value={.value=1};
                if(b->group_values[a]>=0) {
                    int index=b->group_values[a]; if(b->vectors[index].errors[end]) return -5;
                    value=qs_vector_cell(&b->plan,b->vectors,index,end);
                }
                int status=group_update(s,n,a,&partial[a],value,!value.isnull); if(status) return status;
            }
            ++end;
        } while(combine && end<b->count && end-first<capacity-out->rows);
        int added=group_sink_values(s,id,o,keys,partial,1); if(added<0) return added;
        if(!added) {
            for(unsigned r=first;r<end;++r) {
                struct qs_cell *row=(struct qs_cell *)out->data+(size_t)out->rows*b->plan.nout;
                for(unsigned col=0;col<b->plan.nout;++col) {
                    int index=b->plan.outputs[col].index; if(b->vectors[index].errors[r]) return -5;
                    row[col]=qs_vector_cell(&b->plan,b->vectors,index,r);
                }
                ++out->rows;
            }
        }
        s->metrics.matched+=end-first; b->position=end;
    }
    return 0;
}
static int group_sink_batch(struct execution *s,struct qs_result *out)
{
    unsigned id=s->group_sink-1,columns=s->plan->nodes[s->root].expr.nout,kept=0;
    struct operator_state *o=&s->owner->nodes[id];
    for(unsigned i=0;i<out->rows;++i) {
        struct qs_cell *row=(struct qs_cell *)out->data+(size_t)i*columns;
        int status=group_sink_row(s,id,o,row,1); if(status<0) return status;
        if(!status) {
            memmove((struct qs_cell *)out->data+(size_t)kept++*columns,row,columns*sizeof(*row));
        }
    }
    out->rows=kept; return 0;
}
/* Retain the compact integer-key path. Other keys add only their actual cell
 * count and grow from a small table; spill value/count states, never raw rows. */
static size_t group_reserve(const struct execution *s,const struct qp_node *n)
{
    size_t reserve=2*s->sort_limit;
    /* Hash groups are allocated before their scan launches. Reserve the actual
     * worker state too, or a small query budget can lose
     * all spill space as soon as the workers start. Active workers are already
     * charged to the parent and must not be counted a second time. */
    if(s->plan->workers>1 && !s->parallel_started && s->plan->nodes[n->left].parallel) {
        int leaf=qp_probe_scan(s->plan,n->left);
        size_t index=leaf>=0 && s->plan->nodes[leaf].ordered_index?QS_INDEX_WORK_BYTES+sizeof(struct qs_plan):0;
        reserve+=s->plan->workers*(sizeof(*s)+s->plan->count*sizeof(*s->nodes)+sizeof(struct qs_plan)+index+256);
    }
    return reserve;
}
/* A parent must not reserve a large estimated group table while a blocking
 * child still needs to build/materialize its input. Start small there and grow
 * from real input after those dependencies run; all bytes share one context. */
static int group_input_pending(const struct execution *s,int id)
{
    if(id<0) return 0;
    const struct qp_node *n=&s->plan->nodes[id]; const struct operator_state *o=&s->nodes[id];
    if(n->kind==QP_AGGREGATE && n->nkeys) return !o->finished;
    if(n->kind==QP_SORT && !o->left_sort.drained) return 1;
    if(n->kind==QP_MATERIALIZED) return 1;
    return group_input_pending(s,n->left) || group_input_pending(s,n->right);
}
static int64_t hash_aggregate_next_impl(struct execution *s,unsigned id,struct qs_cell *row)
{
    struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    struct sorted_rows *sort=&o->left_sort;
    if(!qp_hash_aggregate_supported(n)) { n->hash=0; return aggregate_next(s,id,row); }
    int wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
    if(!o->group_capacity) {
        struct cs_work_context_stats memory;
        if(cs_work_get_context_stats(s->context,&memory)) return -4;
        size_t available=memory.limit_bytes-memory.used_bytes,reserve=group_reserve(s,n);
        available=available>reserve?available-reserve:available/2;
        o->group_integer=1;
        /* Runtime strings are canonical dictionary IDs, also 64-bit keys. */
        for(unsigned k=0;k<n->nkeys;++k) {
            unsigned type=n->expr.exprs[n->expr.keys[k]].type;
            if(type!=Q_INTEGER && type!=Q_STRING) o->group_integer=0;
        }
        o->group_stride=sizeof(*o->groups)+n->expr.naggs*sizeof(struct group_value)+
            (wide?n->nkeys*(o->group_integer?sizeof(int64_t):sizeof(struct qs_cell)):0);
        o->group_stride=(o->group_stride+15)&~15U;
        unsigned capacity=128;
        /* PG's pre-HAVING group estimate is distinct from output cardinality.
         * Use it for wide tables too, within the real remaining budget and
         * spill/worker reserve, instead of serializing waves from 128 buckets.
         * Legacy plans without that estimate retain the conservative path. */
        int key_only=n->expr.aggs[0].kind==Q_COUNT_STAR && n->expr.nout==n->nkeys;
        for(unsigned i=0;key_only && i<n->expr.nout;++i)
            if(n->expr.exprs[n->expr.outputs[i].index].op!=Q_COLUMN) key_only=0;
        double groups=n->estimated_groups>0?n->estimated_groups:n->estimated_rows;
        while(!group_input_pending(s,n->left) &&
              (!wide || n->estimated_groups>0 || s->plan->nodes[n->left].kind==QP_SCAN ||
              (key_only && s->plan->nodes[n->left].parallel)) && capacity<groups*8/7 &&
              (size_t)capacity*2*o->group_stride<=available && capacity<UINT_MAX/2)
            capacity*=2;
        for(;;) {
            o->groups=cs_work_calloc(s->context,capacity,o->group_stride);
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
            int added=group_sink_row(s,id,o,o->left,0);
            if(added<0) return added;
            if(!added) {
                unsigned capacity=o->group_capacity*2;
                struct cs_work_context_stats m;
                if(cs_work_get_context_stats(s->context,&m)) return -4;
                size_t available=m.limit_bytes-m.used_bytes,reserve=group_reserve(s,n);
                available=available>reserve?available-reserve:0;
                void *larger=capacity>o->group_capacity && (size_t)capacity*o->group_stride<=available?
                    cs_work_calloc(s->context,capacity,o->group_stride):NULL;
                if(larger) {
                    struct compact_group *old=o->groups; unsigned previous=o->group_capacity;
                    o->groups=larger; o->group_capacity=capacity;
                    for(unsigned i=0;i<previous;++i) {
                        struct compact_group *v=(void *)((unsigned char *)old+(size_t)i*o->group_stride);
                        if(!v->occupied) continue;
                        uint64_t hash=wide?(uint64_t)v->key:mix_hash(v->occupied==2?0:(uint64_t)v->key);
                        unsigned slot=hash&(capacity-1);
                        while(group_at(o,slot)->occupied) slot=(slot+1)&(capacity-1);
                        memcpy(group_at(o,slot),v,o->group_stride);
                    }
                    cs_work_free(s->context,old); continue;
                }
                o->group_phase=1; continue;
            }
            o->have_left=0; continue;
        }
        if(o->group_phase==1) {
            if(o->input_done && !o->hash_fallback) { o->group_phase=3; continue; }
            o->hash_fallback=1;
            struct qs_plan partial={.nout=n->nkeys+2*n->expr.naggs}; int columns[QS_KEYS];
            for(unsigned k=0;k<n->nkeys;++k) {
                columns[k]=k; partial.outputs[k]=s->plan->nodes[n->left].expr.outputs[n->left_keys[k]];
            }
            for(unsigned a=0;a<n->expr.naggs;++a) {
                /* Internal values, including canonical text IDs, stay cells;
                 * final output alone converts string IDs to exported text. */
                partial.outputs[n->nkeys+2*a].type_oid=1700;
                partial.outputs[n->nkeys+2*a+1].type_oid=20;
            }
            int64_t status=sort_init(s,sort,&partial,n->nkeys,columns,NULL); if(status) return status;
            if(sort->input_pending) {
                status=sort_call(s,sort,0); if(status) return status;
                sort->input_pending=0; sort->input->rows=0;
            }
            while(o->group_flush<o->group_capacity && sort->input->rows<QS_RESULT_ROWS) {
                struct compact_group *g=group_at(o,o->group_flush++); if(!g->occupied) continue;
                struct qs_cell *out=sort->input->data[sort->input->rows++].cells;
                group_keys(n,o,g,out);
                for(unsigned a=0;a<n->expr.naggs;++a) {
                    out[n->nkeys+2*a]=g->values[a].sum;
                    out[n->nkeys+2*a+1]=(struct qs_cell){.value=g->values[a].count};
                }
            }
            if(sort->input->rows) { sort->input_pending=1; continue; }
            if(o->input_done) {
                cs_work_free(s->context,o->groups); o->groups=NULL;
                sort->complete=1; sort->group.count=1; sort->group.states[0]=sort->task.state;
                cs_work_free(s->context,sort->input); sort->input=NULL;
                o->group_phase=2;
            } else {
                memset(o->groups,0,(size_t)o->group_capacity*o->group_stride);
                o->group_count=o->group_flush=0; o->group_phase=0;
            }
            continue;
        }
        if(o->group_phase==3) {
            while(o->group_flush<o->group_capacity) {
                struct compact_group *g=group_at(o,o->group_flush++); if(!g->occupied) continue;
                struct qs_cell keys[QS_KEYS]; group_keys(n,o,g,keys);
                int64_t result=group_result(s,n,g->values,keys,row); if(result) return result;
            }
            cs_work_free(s->context,o->groups); o->groups=NULL; o->finished=1; return 0;
        }
        /* Merge partial value/count pairs; AVG is divided only for final output. */
        if(!o->have_right && !o->group_ready) {
            if((s->pending_sort==sort || sort->position==sort->output->rows) && !sort->drained) {
                int64_t status=sort_call(s,sort,1); if(status) return status;
                sort->position=0; sort->drained=sort->output->finished;
            }
            if(sort->position<sort->output->rows) {
                memcpy(o->right,sort->output->data[sort->position++].cells,(n->nkeys+2*n->expr.naggs)*sizeof(*o->right)); o->have_right=1;
            } else o->group_ready=1;
        }
        if(!o->phase) {
            if(!o->have_right) { cs_work_free(s->context,sort->output); sort->output=NULL; o->finished=1; return 0; }
            memcpy(o->keys,o->right,n->nkeys*sizeof(*o->keys));
            memset(o->accum,0,sizeof(o->accum));
            for(unsigned a=0;a<n->expr.naggs;++a) o->accum[a].sum.isnull=1;
            o->phase=1;
        }
        struct group_value groups[QS_AGGS];
        for(unsigned a=0;a<n->expr.naggs;++a)
            groups[a]=(struct group_value){.sum=o->accum[a].sum,.count=o->accum[a].count};
        int equal=o->have_right?group_equal(s,n,o->right,o->keys):0; if(equal<0) return equal;
        if(equal) {
            for(unsigned a=0;a<n->expr.naggs;++a) {
                int status=group_update(s,n,a,&groups[a],o->right[n->nkeys+2*a],o->right[n->nkeys+2*a+1].value);
                if(status) return status;
                o->accum[a].sum=groups[a].sum; o->accum[a].count=groups[a].count;
            }
            o->have_right=0; continue;
        }
        o->phase=0;
        int64_t result=group_result(s,n,groups,o->keys,row); if(result) return result;
    }
}
static int64_t hash_aggregate_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    /* Pure in-memory output is one call per group: leave it untimed. */
    if(!s->profiling || s->nodes[id].group_phase==3) return hash_aggregate_next_impl(s,id,row);
    unsigned op=s->nodes[id].group_phase==2?QS_AGG_SOURCE:QS_AGG_SINK;
    struct blocking_timer timer=blocking_begin(s);
    int64_t status=hash_aggregate_next_impl(s,id,row); blocking_end(s,op,timer); return status;
}
static int64_t aggregate_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    if(n->hash) return hash_aggregate_next(s,id,row);
    /* Initialize before starting the child pipeline: parallel workers combine
     * their local states here before the first (and only) output row. */
    if(!n->nkeys && !o->phase && !o->emitted) {
        memset(o->accum,0,sizeof(o->accum)); o->phase=1;
    }
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
    struct projection_batch *b=index_columns;
    if(o->index_flush) { o->index_flush=0; return 0; }
    if(!b) { o->index_output->rows=0; o->index_position=0; }
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
        if(b) {
            /* Keep only columns needed by the fused expression DAG. The
             * index cursor retains the decoded chunk until this batch drains. */
            projection_append(b,values);
        } else {
            status=project(s,n,values,NULL,o->index_output->data[o->index_output->rows].cells);
            if(status<0) return status;
            if(status) ++o->index_output->rows;
        }
    }
    if(n->join_type==QP_SEMI || n->join_type==QP_ANTI) qs_index_skip(&o->index_control,0,o->index_matches);
    /* SEMI callbacks inspect one candidate at a time so they can skip its
     * remaining matches. Gather the accepted left rows until the lookup batch
     * finishes, rather than running the vector DAG for every single match. */
    unsigned rows=b?b->input_count:o->index_output->rows;
    if(rows && (!b || n->join_type!=QP_SEMI)) {
        o->index_flush=1; return QP_INDEX_YIELD;
    }
    return 0;
}
static int64_t index_next(struct execution *s,unsigned id,struct qs_cell *row,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    struct operator_state *o=&s->nodes[id]; unsigned ln=s->plan->nodes[n->left].expr.nout;
    if(!o->index_plan) {
        struct qs_plan *p=cs_work_calloc(s->context,1,sizeof(*p));
        if(!b) o->index_output=cs_work_calloc(s->context,1,sizeof(*o->index_output));
        o->index_left=cs_work_alloc(s->context,64*ln*sizeof(*o->index_left));
        if(!p || (!b && !o->index_output) || !o->index_left) return -4;
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
        if(!b) {
            if(o->index_position<o->index_output->rows) {
                memcpy(row,o->index_output->data[o->index_position++].cells,n->expr.nout*sizeof(*row)); return 2;
            }
            o->index_output->rows=0; o->index_position=0;
        }
        if(o->phase==2) {
            while(o->index_unmatched<o->index_count) {
                unsigned at=o->index_unmatched++;
                if((n->join_type!=QP_LEFT && n->join_type!=QP_ANTI) || (o->index_matches&(UINT64_C(1)<<at))) continue;
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
            unsigned batch=n->sorted_input?1:64;
            while(o->index_count<batch && !o->input_done) {
                int64_t status=next_row(s,n->left,o->index_left+(size_t)o->index_count*ln);
                if(status==QP_WAIT || status<0 || status>2) return status;
                if(!status) o->input_done=1; else ++o->index_count;
            }
            if(!o->index_count) { o->finished=1; return 0; }
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
        index_execution=s; index_node=id; index_columns=b;
        int64_t status=qs_index_probe(0,mr[0],p,&o->index_task,&o->index_control,&a,o->index_resume);
        index_columns=NULL; o->index_resume=1;
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
static int64_t next_row_impl(struct execution *s,unsigned id,struct qs_cell *row)
{
    if(id>=s->plan->count) return -1;
    struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    if(s->worker && (int)id==s->source_node) {
        unsigned bytes=hash_row_bytes(&n->expr),block_rows=QS_SORT_IO_BYTES/bytes;
        if(s->source_at==s->source_end) {
            s->source_at=atomic_fetch_add(&s->owner->next_offset,block_rows);
            if(s->source_at>=s->source.rows) return 0;
            s->source_end=s->source_at+block_rows;
            if(s->source_end>s->source.rows) s->source_end=s->source.rows;
        }
        unsigned char packed[QS_OUT*19];
        int64_t status=spool_row_bytes(s,&s->source,s->source_at,bytes,packed); if(status) return status;
        hash_load(row,packed,&n->expr); ++s->source_at; return 2;
    }
    if(o->finished && (!o->projection || o->projection->position==o->projection->count)) return 0;
    if(!s->worker && s->plan->workers>1 && n->parallel) return parallel_next(s,id,row);
    if(n->kind==QP_SCAN || (n->kind==QP_PROJECT && n->left>=0) || (n->kind==QP_JOIN && n->hash) || n->kind==QP_INDEX_JOIN) {
        int status=projection_init(s,id); if(status) return status;
        if(o->projection) return projection_next(s,id,row);
    }
    if(n->kind==QP_SCAN) return scan_next(s,id,row);
    if(n->kind==QP_JOIN) return n->hash?hash_next(s,id,row):join_next(s,id,row);
    if(n->kind==QP_INDEX_JOIN) return index_next(s,id,row,NULL);
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
/* DuckDB-style local ungrouped states, combined after the worker finishes.
 * AVG transfers sum/count; only the coordinator performs the final division. */
static int aggregate_combine(struct execution *s,unsigned id,const struct operator_state *from)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *to=&s->nodes[id];
    for(unsigned i=0;i<n->expr.naggs;++i) {
        const struct accumulator *a=&from->accum[i]; struct accumulator *b=&to->accum[i];
        if(!a->count) continue;
        unsigned kind=n->expr.aggs[i].kind;
        if(kind==Q_MIN || kind==Q_MAX) {
            int cmp=b->count?compare_cell(s,a->extreme,b->extreme,n->expr.exprs[n->expr.aggs[i].expr].type):0;
            if(cmp==INT_MIN) return -5;
            if(!b->count || (kind==Q_MIN?cmp<0:cmp>0)) b->extreme=a->extreme;
        }
        struct group_value partial={.sum=b->sum,.count=b->count};
        int status=group_add(&partial,a->sum,a->count,kind); if(status) return status;
        b->sum=partial.sum; b->count=partial.count;
    }
    return 0;
}
/* A scan/projection-only input can feed the local aggregate directly from
 * expression vectors. Row batches remain the adapter for joins and other
 * operators that have not yet acquired a column-batch interface. */
static int aggregate_sink_vectors(struct execution *s,unsigned id)
{
    struct projection_batch *b=s->nodes[id].projection;
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    for(unsigned a=0;a<n->expr.naggs;++a) {
        const struct qs_agg *agg=&n->expr.aggs[a]; int index=b->plan.outputs[a].index;
        if(agg->kind==Q_COUNT_STAR) { o->accum[a].count+=b->count; continue; }
        const struct qs_vector *v=&b->vectors[index];
        for(unsigned r=0;r<b->count;++r) {
            if(v->errors[r]) return -5;
            if(qs_vector_null(v,r)) continue;
            struct qs_cell value=qs_vector_cell(&b->plan,b->vectors,index,r);
            int status=accumulate_value(s,n,agg,&o->accum[a],value); if(status) return status;
        }
    }
    b->position=b->count; s->metrics.matched+=b->count; return 0;
}
static int aggregate_sink_batch(struct execution *s,struct qs_result *out)
{
    unsigned id=s->aggregate_sink-1,columns=s->plan->nodes[s->root].expr.nout;
    struct operator_state *o=&s->nodes[id];
    for(unsigned r=0;r<out->rows;++r) {
        memcpy(o->left,(struct qs_cell *)out->data+(size_t)r*columns,columns*sizeof(*o->left));
        int status=accumulate(s,&s->plan->nodes[id],o); if(status) return status;
    }
    out->rows=0; return 0;
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
restart:
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
            if(s->nodes[id].external && s->nodes[id].finished) {
                for(unsigned j=0;j<count;++j) {
                    struct operator_state *o=&s->nodes[builds[j]];
                    if(o->hash_data) cs_work_free(s->context,o->hash_data);
                    o->hash_data=NULL;
                }
                s->control->parallel=0; s->nodes[root].finished=1; return 0;
            }
            if(s->nodes[id].hash_fallback==1) {
                p->nodes[root].parallel=0;
                return next_row(s,root,row);
            }
        }
        unsigned hash_sink=0,group_sink=0,aggregate_sink=0;
        for(unsigned i=0;i<p->count;++i) {
            const struct qp_node *parent=&p->nodes[i];
            if(parent->kind==QP_AGGREGATE && parent->left==(int)root &&
               !parent->nkeys && !parent->distinct && !parent->distinct_aggs)
                aggregate_sink=i+1;
        }
        for(unsigned i=0;i<p->count;++i)
            if(p->nodes[i].kind==QP_AGGREGATE && p->nodes[i].left==(int)root &&
               p->nodes[i].hash && s->nodes[i].groups && !s->nodes[i].group_phase)
                group_sink=i+1;
        for(unsigned i=0;i<p->count;++i)
            if(p->nodes[i].kind==QP_JOIN && p->nodes[i].right==(int)root &&
               s->nodes[i].hash_data && !s->nodes[i].hash_phase && !s->nodes[i].hash_fallback)
                hash_sink=i+1;
        s->partition_join=-1;
        for(unsigned i=0;i<count;++i)
            if(s->nodes[builds[i]].external && !s->nodes[builds[i]].external->direct) { s->partition_join=builds[i]; break; }
        for(unsigned w=0;w<p->workers;++w) {
            struct cs_work_context_stats stats;
            if(cs_work_get_context_stats(s->context,&stats)) return -4;
            const struct qp_node *leaf=&p->nodes[qp_probe_scan(p,root)];
            size_t private_bytes=sizeof(*s)+p->count*sizeof(*s->nodes)+sizeof(struct qs_plan)+256;
            if(leaf->ordered_index) private_bytes+=QS_INDEX_WORK_BYTES+sizeof(struct qs_plan);
            size_t spill_reserve=0;
            for(unsigned i=0;i<p->count;++i)
                if(p->nodes[i].kind==QP_AGGREGATE && p->nodes[i].left==(int)root && s->nodes[i].group_capacity)
                    spill_reserve=2*s->sort_limit;
            /* Cursor/filter state is allocated lazily by the worker. Keep
             * that space and the existing spill reserve before launching;
             * otherwise a successful launch can fail on its first page. */
            if(stats.limit_bytes-stats.used_bytes<(p->workers-w)*private_bytes+spill_reserve) {
                close_workers(s); p->workers=1;
                return next_row(s,root,row);
            }
            cs_work_context *context=cs_work_begin_child(s->context,stats.limit_bytes);
            struct execution *e=context?cs_work_calloc(context,1,sizeof(*e)):NULL;
            if(e) e->nodes=cs_work_calloc(context,p->count,sizeof(*e->nodes));
            if(!e || !e->nodes) {
                if(context) cs_work_end(context);
                close_workers(s); p->nodes[root].parallel=0;
                return next_row(s,root,row);
            }
            e->context=context; e->plan=p; e->owner=s; e->worker=w+1; e->root=root;
            e->hash_sink=hash_sink; e->group_sink=group_sink; e->aggregate_sink=aggregate_sink;
            e->control=s->control+w+1; e->dictionary=s->dictionary; e->arena=s->arena;
            e->io_bytes=QS_SORT_IO_BYTES;
            e->sort_limit=s->sort_limit; e->profiling=s->profiling; e->initializer=p->ninit;
            e->source_node=-1;
            if(s->partition_join>=0) {
                const struct external_hash *h=s->nodes[s->partition_join].external;
                e->source_node=p->nodes[s->partition_join].left;
                e->source=h->probe[h->partition]; e->source.cache=NULL; e->source.loaded=UINT_MAX;
            }
            memcpy(e->params,s->params,sizeof(e->params));
            for(unsigned i=0;i<p->count;++i) {
                const struct operator_state *built=&s->nodes[i]; struct operator_state *local=&e->nodes[i];
                if(built->hash_phase) {
                    local->hash_data=built->hash_data; local->hash_rows=built->hash_rows;
                    local->hash_capacity=built->hash_capacity; local->hash_phase=1; local->hash_shared=1;
                    local->hash_buckets=built->hash_buckets;
                }
                if(built->set_ready) {
                    local->set=built->set; local->set_capacity=built->set_capacity;
                    local->set_count=built->set_count; local->set_null=built->set_null; local->set_ready=1;
                }
            }
            *e->control=(struct qp_control){.state=(uintptr_t)e}; s->workers[w]=e;
            *(struct qs_chunk *)(s->arena+vector_offset(e,leaf))=
                (struct qs_chunk){.capacity=leaf->ordered_index?256:qp_scan_capacity(p,leaf),.ncols=leaf->expr.ncols};
        }
        s->parallel_node=root; s->parallel_done=0;
        memset(s->parallel_position,0,sizeof(s->parallel_position)); atomic_store(&s->next_offset,0);
        s->control->parallel=hash_sink || group_sink?2:1; ++s->control->epoch; s->control->available=s->control->drained=0;
        s->control->scan_node=0;
        int scan=qp_probe_scan(p,root);
        bool scheduled=s->partition_join<0 && scan>=0 && !p->nodes[scan].ordered_index;
        for(int id=root;scheduled && id!=scan;id=p->nodes[id].left)
            if(p->nodes[id].kind==QP_INDEX_JOIN) scheduled=false;
        if(scheduled) s->control->scan_node=scan+1;
        s->parallel_started=1; s->used_workers=p->workers;
        if(s->profiling) { s->pipeline_start=now_ns(CLOCK_MONOTONIC); s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID); }
    }
    for(unsigned w=0;w<p->workers;++w) if(s->control->available&(UINT64_C(1)<<w)) {
        struct qs_result *out=&s->outputs[w+1];
        if(s->parallel_position[w]<out->rows) {
            unsigned cols=p->nodes[root].expr.nout;
            memcpy(row,(struct qs_cell *)out->data+(size_t)s->parallel_position[w]++*cols,cols*sizeof(*row));
            return 2;
        }
        if(out->finished) s->parallel_done|=UINT64_C(1)<<w;
        s->parallel_position[w]=0; s->control->available&=~(UINT64_C(1)<<w); s->control->drained|=UINT64_C(1)<<w;
    }
    if(s->parallel_done==qp_worker_mask(p->workers)) {
        struct qp_pipeline_profile *phase=&p->pipelines[root];
        if(s->profiling) {
            phase->wall_ns+=now_ns(CLOCK_MONOTONIC)-s->pipeline_start;
            phase->coordinator_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-s->pipeline_tick; ++phase->rounds;
        }
        for(unsigned w=0;w<p->workers;++w) {
            const struct qs_result *v=&s->outputs[w+1];
            if(s->workers[w]->aggregate_sink) {
                unsigned id=s->workers[w]->aggregate_sink-1;
                int status=aggregate_combine(s,id,&s->workers[w]->nodes[id]); if(status) return status;
            }
            if(s->profiling) {
                phase->worker_ns+=v->profile.operators[QS_JOIN_PROBE].cpu_ns;
                phase->sink_ns+=s->workers[w]->sink_ns; phase->sink_rows+=s->workers[w]->sink_rows;
            }
            s->metrics.build_rows+=v->metrics.build_rows;
            s->metrics.scanned+=v->metrics.scanned; s->metrics.matched+=v->metrics.matched;
            s->metrics.input_bytes+=v->metrics.input_bytes-v->metrics.index_pages*QS_PAGE;
            s->metrics.index_pages+=v->metrics.index_pages; s->metrics.index_tuples+=v->metrics.index_tuples;
            s->metrics.spill_read_bytes+=v->metrics.spill_read_bytes;
            s->metrics.spill_write_bytes+=v->metrics.spill_write_bytes;
            for(unsigned op=0;op<QS_OPERATORS;++op) {
                struct qs_timing *t=&s->profile.operators[op]; const struct qs_timing *x=&v->profile.operators[op];
                t->calls+=x->calls; t->wall_ns+=x->wall_ns; t->cpu_ns+=x->cpu_ns;
            }
        }
        close_workers(s);
        s->parallel_started=0; s->control->parallel=0;
        if(s->partition_join>=0) {
            external_advance(s,s->partition_join); s->partition_join=-1;
            goto restart;
        }
        for(int id=root;id>=0 && p->nodes[id].kind!=QP_SCAN;id=p->nodes[id].left) if(p->nodes[id].kind==QP_JOIN) {
            cs_work_free(s->context,s->nodes[id].hash_data); s->nodes[id].hash_data=NULL;
        }
        s->nodes[root].finished=1; return 0;
    }
    return QP_WAIT;
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
/* Resource discovery uses a normal zero-argument CSF and its 64-bit completion
 * value. SNIA CSE properties report CUs, but do not describe this allocator. */
int64_t pg_sina_workmem_capacity(struct femu_csd_args *a)
{
    struct cs_work_stats stats;
    if(!a || a->numr || cs_work_get_stats(&stats) || stats.capacity_bytes>INT64_MAX) return -1;
    return stats.capacity_bytes;
}
/* plan, lifecycle/page requests, result, input/dictionary arena, spill I/O.
 * A successful call either returns result rows or asks PG to LOAD pages.
 * NEED_STORE/LOAD is handled entirely by the existing SNIA batch scheduler. */
static void *dictionary_allocate(void *context,size_t bytes)
{ return cs_work_alloc(context,bytes); }
int64_t pg_sina_plan(struct femu_csd_args *a)
{
    if(!a || a->numr!=5 || a->mr_len[0]<(long long)sizeof(struct qp_plan) ||
        a->mr_len[1]<(long long)sizeof(struct qp_control) || a->mr_len[2]<(long long)sizeof(struct qs_result) ||
        a->mr_len[4]<QS_SORT_IO_BYTES) return -1;
    struct qp_plan *plan=a->mr_addr[0]; unsigned worker=((uint64_t)a->cparam1>>1)&127;
    if(plan->workers>QS_WORKERS || worker>plan->workers || a->mr_len[1]<(long long)((worker+1)*sizeof(struct qp_control)) ||
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
        s->context=context; s->plan=plan; s->control=control;
        const struct qs_dict *config=a->mr_addr[3];
        s->dictionary=cs_work_calloc(context,1,sizeof(*s->dictionary));
        if(!s->dictionary) { cs_work_end(context); return -4; }
        *s->dictionary=(struct qs_dict){.entries=config->entries,.capacity=config->entries,.data=config->data,
            .context=context,.allocate=dictionary_allocate};
        /* An input can already contain dictionary IDs (for example a
         * materialized column batch). Preserve those IDs while moving its
         * backing strings into this query's budget. PG's config is empty. */
        for(unsigned id=1;id<=config->count;++id) {
            const char *bytes; unsigned length;
            if(qs_dict_get(config,id,&bytes,&length) || qs_dict_insert(s->dictionary,bytes,length)!=(int)id) {
                cs_work_end(context); return -3;
            }
        }
        s->partition_join=s->source_node=-1;
        s->arena=a->mr_addr[3]; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
        s->nodes=cs_work_calloc(context,plan->count,sizeof(*s->nodes));
        if(!s->nodes) { cs_work_end(context); return -4; }
        unsigned depth=sort_depth(plan,plan->root);
        s->sort_limit=budget/(depth+2); if(s->sort_limit<512*1024) { cs_work_end(context); return -4; }
        s->profiling=a->cparam1&1; s->outputs=a->mr_addr[2]; control->state=(uintptr_t)s;
    }
    s->args=a;
    if(!csIsReentry(a) && ((uint64_t)a->cparam1&QP_PAGES_INPUT) && !((uint64_t)a->cparam1&QP_SCAN_INPUT))
        control->ready=1;
    if(((uint64_t)a->cparam1&QP_SCAN_INPUT) && !csIsReentry(a)) {
        int id=qp_probe_scan(plan,s->root);
        uint64_t page=(uint64_t)a->cparam1>>QP_SCAN_PAGE_SHIFT;
        if(!worker || id<0 || plan->nodes[id].ordered_index || page>UINT64_MAX/QS_PAGE) return -1;
        struct qp_node *n=&plan->nodes[id]; struct operator_state *o=&s->nodes[id];
        uint64_t offset=page*QS_PAGE;
        if(offset>n->bytes || (s->host_scan && !s->scan_drained)) return -1;
        s->host_scan=true; s->scan_drained=false;
        o->position=offset; o->scan_position=o->scan_input=o->scan_bytes=0;
        struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n)); chunk->rows=chunk->selected=0;
        control->node=id; control->offset=offset;
        control->bytes=n->bytes-offset<plan->scan_bytes?n->bytes-offset:plan->scan_bytes;
        control->ready=control->bytes!=0;
    }
    /* The last AFDM is private to this CU, also for SNIA NEED_LOAD/STORE. */
    s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
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
    if(s->profiling && !worker) s->pipeline_tick=cpu;
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
produce:
    if(worker && s->group_sink) {
        unsigned id=s->group_sink-1;
        status=projection_init(s,id); if(status<0) goto produced;
        while(s->nodes[id].projection) {
            status=projection_fill(s,id);
            if(status!=2) {
                if(!status) out->finished=control->finished=1;
                if(status!=3) goto produced;
                break;
            }
            struct projection_batch *b=s->nodes[id].projection;
            unsigned before=b->position,rows=out->rows;
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            status=group_sink_vectors(s,id,out,capacity);
            if(s->profiling) {
                s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick;
                s->sink_rows+=b->position-before-(out->rows-rows);
            }
            if(status<0) goto produced;
            status=2; if(out->rows) goto produced;
        }
        status=2;
    }
    if(worker && s->aggregate_sink) {
        unsigned id=s->aggregate_sink-1;
        status=projection_init(s,id); if(status<0) goto produced;
        while(s->nodes[id].projection) {
            status=projection_fill(s,id);
            if(status!=2) {
                if(!status) out->finished=control->finished=1;
                if(status!=3) goto produced;
                break;
            }
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            unsigned rows=s->nodes[id].projection->count;
            status=aggregate_sink_vectors(s,id);
            if(s->profiling) { s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick; s->sink_rows+=rows; }
            if(status<0) goto produced;
        }
        status=2;
    }
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
produced:
    if(worker && (s->hash_sink || s->group_sink || s->aggregate_sink) && status>=0 && status<=2) {
        uint64_t cpu=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
        unsigned rows=out->rows;
        if(s->hash_sink) hash_sink_batch(s,out);
        if(s->group_sink) { int added=group_sink_batch(s,out); if(added<0) status=added; }
        if(s->aggregate_sink) { int added=aggregate_sink_batch(s,out); if(added<0) status=added; }
        if(s->profiling) { s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu; s->sink_rows+=rows-out->rows; }
        /* A direct sink can consume another output batch from the same input
         * without asking PG to submit another compute request. */
        if(s->host_scan && !out->rows && status==2) goto produce;
    }
    if(s->profiling) {
        struct qs_timing *t=&s->profile.operators[QS_JOIN_PROBE];
        if(!worker && s->parallel_started)
            plan->pipelines[s->parallel_node].coordinator_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-s->pipeline_tick;
        ++t->calls; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-start; t->cpu_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
        atomic_fetch_sub(&owner->active_cus,1); control->peak_cus=atomic_load(&owner->peak_cus);
    }
    control->output_rows=out->rows;
    out->metrics=s->metrics; out->metrics.workers=s->used_workers?s->used_workers:1; out->profile=s->profile;
    out->metrics.dictionary_entries=__atomic_load_n(&s->dictionary->count,__ATOMIC_ACQUIRE);
    out->metrics.dictionary_bytes=__atomic_load_n(&s->dictionary->bytes,__ATOMIC_RELAXED);
    for(unsigned i=0;i<plan->count;++i) if(plan->nodes[i].kind==QP_INDEX_JOIN || plan->nodes[i].ordered_index) {
        out->metrics.index_pages+=s->nodes[i].index_task.index_pages;
        out->metrics.index_tuples+=s->nodes[i].index_task.index_tuples;
    }
    out->metrics.input_bytes+=out->metrics.index_pages*QS_PAGE;
    struct cs_work_context_stats memory;
    if(!cs_work_get_context_stats(s->context,&memory)) out->metrics.work_peak=memory.peak_used_bytes;
    if(status<0) return status;
    if(worker && s->host_scan && status<=2)
        return (out->rows?QP_OUTPUT_READY:0) | (s->scan_drained?QP_INPUT_DRAINED:0) |
            (out->finished?QP_WORKER_FINISHED:0);
    if(out->finished && !worker) close_execution(s);
    return status==QP_WAIT?0:status>2?status:0;
}
