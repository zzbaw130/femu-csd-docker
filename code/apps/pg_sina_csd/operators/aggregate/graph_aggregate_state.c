#include "graph_state.h"
#include <stdio.h>

static struct aggregate_input *aggregate_input(struct execution *s,struct operator_state *o)
{
    if(!o->aggregate_input) o->aggregate_input=cs_work_calloc(s->context,1,sizeof(*o->aggregate_input));
    return o->aggregate_input;
}
/* active: 1 initializes group keys, 2 compares this row with the group,
 * 3 evaluates/accumulates arguments, 4 evaluates hash-aggregate arguments.
 * Only one of these can wait per row. */
static int initialize_group_keys(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    struct qs_eval flat,*e=&flat; struct aggregate_input *input=NULL;
    unsigned first=0;
    if(s->text) {
        input=aggregate_input(s,o); if(!input) return -4;
        if(!input->active) { input->active=1; input->argument=0; init_eval(&input->evaluation,&n->expr,o->left,NULL); }
        if(input->active!=1) return -5;
        e=&input->evaluation; first=input->argument;
    } else init_eval(e,&n->expr,o->left,NULL);
    for(unsigned k=first;k<n->expr.nkeys;++k) {
        if(evaluate(s,e,n->expr.keys[k],&o->keys[k])) return -5;
        if(input) input->argument=k+1;
    }
    if(input) input->active=0;
    return 0;
}
static int group_same(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    struct qs_eval flat,*e=&flat; struct aggregate_input *input=NULL;
    unsigned first=0;
    if(s->text) {
        input=aggregate_input(s,o); if(!input) return -4;
        if(!input->active) { input->active=2; input->argument=0; init_eval(&input->evaluation,&n->expr,o->left,NULL); }
        if(input->active!=2) return -5;
        e=&input->evaluation; first=input->argument;
    } else init_eval(e,&n->expr,o->left,NULL);
    for(unsigned k=first;k<n->expr.nkeys;++k) {
        struct qs_cell key; if(evaluate(s,e,n->expr.keys[k],&key)) return -1;
        int equal=equal_cell(s,key,o->keys[k],n->expr.exprs[n->expr.keys[k]].type);
        if(equal<=0) { if(input) input->active=0; return equal; }
        if(input) input->argument=k+1;
    }
    if(input) input->active=0;
    return 1;
}
static int accumulate_paged(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    struct aggregate_input *input=aggregate_input(s,o); if(!input) return -4;
    if(!input->active) {
        input->active=3; input->ready=0; input->argument=input->aggregate=0;
        init_eval(&input->evaluation,&n->expr,o->left,NULL);
    }
    if(input->active!=3) return -5;
    for(;!input->ready && input->argument<n->expr.naggs;++input->argument) {
        unsigned a=input->argument; const struct qs_agg *agg=&n->expr.aggs[a];
        input->values[a]=(struct qs_cell){.value=1};
        if(agg->kind!=Q_COUNT_STAR && evaluate(s,&input->evaluation,agg->expr,&input->values[a])) return -5;
    }
    input->ready=1;
    for(;input->aggregate<n->expr.naggs;++input->aggregate) {
        unsigned i=input->aggregate; const struct qs_agg *agg=&n->expr.aggs[i];
        struct accumulator *a=&o->accum[i]; struct qs_cell v=input->values[i];
        if(v.isnull) continue;
        if((n->distinct_aggs&(1U<<i)) && a->has_last) {
            int equal=equal_cell(s,a->last,v,n->expr.exprs[agg->expr].type);
            if(equal<0) return -5;
            if(equal) continue;
        }
        int status=accumulate_value(s,n,agg,a,v); if(status) return status;
        /* Publish DISTINCT's last value only after MIN/MAX comparison succeeds. */
        if(n->distinct_aggs&(1U<<i)) { a->last=v; a->has_last=1; }
    }
    input->active=input->ready=0; return 0;
}
static int accumulate(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    if(s->text) return accumulate_paged(s,n,o);
    struct qs_eval e; init_eval(&e,&n->expr,o->left,NULL);
    struct qs_cell values[QS_AGGS];
    for(unsigned i=0;i<n->expr.naggs;++i) {
        values[i]=(struct qs_cell){.value=1};
        if(n->expr.aggs[i].kind!=Q_COUNT_STAR && evaluate(s,&e,n->expr.aggs[i].expr,&values[i])) return -5;
    }
    for(unsigned i=0;i<n->expr.naggs;++i) {
        const struct qs_agg *agg=&n->expr.aggs[i]; struct accumulator *a=&o->accum[i]; struct qs_cell v=values[i];
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
/* Dependent payload keeps exact Numeric values and opaque text references.
 * Plain scalar payload needs only its 64-bit value plus NULL, not a full cell. */
static unsigned group_key_bytes(const struct qp_node *n,const struct operator_state *o)
{
    unsigned keys=qp_group_keys(n),bytes=keys*(o->group_integer?8:sizeof(struct qs_cell));
    for(unsigned k=keys;k<n->nkeys;++k) bytes+=group_payload_bytes(n,k);
    return bytes;
}
void group_keys(const struct qp_node *n,const struct operator_state *o,const struct compact_group *g,struct qs_cell *keys)
{
    int wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
    const unsigned char *stored=(const void *)(g->values+n->expr.naggs);
    unsigned count=qp_group_keys(n);
    if(wide && o->group_integer) {
        const int64_t *values=(const void *)stored;
        for(unsigned k=0;k<count;++k) keys[k]=(struct qs_cell){.value=values[k],.isnull=!!(g->key_flags&(2U<<k))};
    } else if(wide) memcpy(keys,stored,count*sizeof(*keys));
    else keys[0]=(struct qs_cell){.value=g->key,.isnull=g->key_flags==2};
    stored+=count*(o->group_integer?8:sizeof(*keys));
    for(unsigned k=count;k<n->nkeys;++k) {
        unsigned bytes=group_payload_bytes(n,k); keys[k]=(struct qs_cell){0};
        if(bytes==9) { int64_t value; memcpy(&value,stored,8); keys[k].value=value; keys[k].isnull=stored[8]; }
        else { memcpy(&keys[k].value,stored,16); keys[k].scale=stored[16]; keys[k].dscale=stored[17]; keys[k].isnull=stored[18]; }
        stored+=bytes;
    }
}
static uint64_t group_hash(const struct qp_node *n,const struct qs_cell *keys)
{
    uint64_t hash=UINT64_C(0x9e3779b97f4a7c15);
    for(unsigned k=0;k<qp_group_keys(n);++k) {
        struct qs_cell v=keys[k];
        if(v.isnull) v=(struct qs_cell){0};
        while(v.scale && !(v.value%10)) { v.value/=10; --v.scale; }
        hash=mix_hash(hash^(uint64_t)v.value^(uint64_t)((unsigned __int128)v.value>>64)^((uint64_t)v.scale<<56));
    }
    return hash;
}
int group_equal(struct execution *s,const struct qp_node *n,const struct qs_cell *a,const struct qs_cell *b)
{
    for(unsigned k=0;k<qp_group_keys(n);++k) {
        int equal=equal_cell(s,a[k],b[k],n->expr.exprs[n->expr.keys[k]].type);
        if(equal<0) return -5;
        if(!equal) return 0;
    }
    return 1;
}
int group_final_value(const struct qs_agg *agg,const struct group_value *g,struct qs_cell *value)
{
    *value=(struct qs_cell){.isnull=!g->count};
    if(agg->kind==Q_COUNT || agg->kind==Q_COUNT_STAR) *value=(struct qs_cell){.value=g->count};
    else if(g->count) {
        *value=g->sum;
        if(agg->kind==Q_AVG && qs_average(*value,g->count,value)) return -5;
    }
    return 0;
}
static int group_result(struct execution *s,const struct qp_node *n,const struct group_value *groups,
    const struct qs_cell *keys,struct qs_cell *row,int raw)
{
    struct qs_cell input[QS_COLS],values[QS_AGGS];
    for(unsigned a=0;a<n->expr.naggs;++a) {
        int status=group_final_value(&n->expr.aggs[a],&groups[a],&values[a]); if(status) return status;
    }
    if(raw) {
        memcpy(row,keys,n->nkeys*sizeof(*row));
        memcpy(row+n->nkeys,values,n->expr.naggs*sizeof(*row)); return 2;
    }
    for(unsigned i=0;i<QS_COLS;++i) input[i]=(struct qs_cell){.isnull=1};
    for(unsigned k=0;k<n->nkeys;++k) input[n->left_keys[k]]=keys[k];
    int status=project(s,n,input,values,row);
    return status<0?status:status?2:0;
}
const struct compact_group *group_source_next(struct execution *s,unsigned id)
{
    const struct operator_state *source=&s->owner->nodes[id];
    if(s->source_at==s->source_end) {
        s->source_at=atomic_fetch_add_explicit(&s->owner->next_offset,64,memory_order_relaxed);
        unsigned rows=atomic_load_explicit(source->group_count,memory_order_relaxed);
        if(s->source_at>=rows) { s->source_end=s->source_at; return NULL; }
        s->source_end=s->source_at+64;
        if(s->source_end>rows) s->source_end=rows;
    }
    return group_at(source,s->source_at++);
}
/* Separate Source columns from the aggregate's input vectors. Read-only CU
 * Sources claim records; serial/stream/spilled Sources advance their original
 * group iterator. Retain partial batches while its I/O request is serviced. */
/* Published keys stay immutable throughout a CU wave. The bounded shared
 * table also accepts rows that do not fit a CU-local table. Only the owner
 * grows/flushes it between CU waves; full tables retain the original rows. */
/* DuckDB's hash salts reject collisions in the compact lookup array before
 * fetching a much larger aggregate record. Low bits retain NULL/key flags. */
uint64_t group_values_hash(const struct qp_node *n,const struct qs_cell *keys)
{
    return n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER?group_hash(n,keys):
        mix_hash(keys[0].isnull?0:(uint64_t)keys[0].value);
}
int group_text_extreme(const struct execution *s,const struct qp_node *n)
{
    if(!s->text) return 0;
    for(unsigned a=0;a<n->expr.naggs;++a) {
        const struct qs_agg *agg=&n->expr.aggs[a];
        if((agg->kind==Q_MIN || agg->kind==Q_MAX) && n->expr.exprs[agg->expr].type==Q_STRING) return 1;
    }
    return 0;
}
struct operator_state *group_worker_local(struct execution *s,unsigned id)
{
    struct operator_state *local=s->worker?s->nodes[id].group_local:NULL;
    if(local && !local->phase) {
        memset(local->group_entries,0,(size_t)local->group_capacity*sizeof(*local->group_entries)); local->phase=1;
    }
    return local;
}
static int group_sink_row_paged(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *row,int shared)
{
    const struct qp_node *n=&s->plan->nodes[id];
    struct aggregate_input *input=aggregate_input(s,&s->nodes[id]); if(!input) return -4;
    if(s->group_update && s->group_update->active) {
        int status=group_update_continue(s,id); if(status>=0) input->active=0;
        return status;
    }
    if(!input->active) {
        input->active=4; input->argument=0;
        init_eval(&input->evaluation,&n->expr,row,NULL);
    }
    if(input->active!=4) return -5;
    for(;input->argument<n->expr.naggs;++input->argument) {
        unsigned a=input->argument; input->values[a]=(struct qs_cell){.value=1};
        if(n->expr.aggs[a].kind!=Q_COUNT_STAR &&
           evaluate(s,&input->evaluation,n->expr.aggs[a].expr,&input->values[a])) return -5;
    }
    struct qs_cell keys[QS_KEYS]; struct group_value values[QS_AGGS];
    for(unsigned k=0;k<n->nkeys;++k) keys[k]=row[n->left_keys[k]];
    for(unsigned a=0;a<n->expr.naggs;++a)
        values[a]=(struct group_value){.sum=input->values[a],.count=!input->values[a].isnull};
    int status=group_worker_values(s,id,o,keys,values,shared,group_values_hash(n,keys));
    if(status>=0) input->active=0;
    return status;
}
static int group_sink_row(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *row,int shared)
{
    if(s->text) return group_sink_row_paged(s,id,o,row,shared);
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
    return group_worker_values(s,id,o,keys,values,shared,group_values_hash(n,keys));
}
/* The numeric value of a COUNT partial is its count. A single integer
 * group key needs only its original key, NULL flag and the accumulated count. */
struct radix_count_partial { int64_t key; uint64_t count,isnull; };

static bool radix_count_only(const struct qp_node *n)
{
#ifdef QP_SPLIT_RUNTIME
    return n->nkeys==1 && n->expr.exprs[n->expr.keys[0]].type==Q_INTEGER && n->expr.naggs==1 &&
        (n->expr.aggs[0].kind==Q_COUNT || n->expr.aggs[0].kind==Q_COUNT_STAR);
#else
    (void)n;
    return false;
#endif
}

/* A full private table is flushed by its own CU, never by the coordinator.
 * The partial aggregate records and stream metadata outlive the Sink workers. */
int64_t radix_flush(struct execution *s,unsigned id)
{
    struct operator_state *local=s->nodes[id].group_local;
    struct radix_group *radix=local->group_radix;
    const struct qp_node *n=&s->plan->nodes[id];
    int wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
    bool packed_count=radix_count_only(n);
    unsigned rows=atomic_load_explicit(local->group_count,memory_order_relaxed);
    while(local->group_flush<rows) {
        struct compact_group *g=group_at(local,local->group_flush);
        uint64_t hash=wide?(uint64_t)g->key:mix_hash(g->key_flags==2?0:(uint64_t)g->key);
        unsigned partition=(hash>>32)&(radix->partitions-1);
        struct spool *stream=&radix->streams[(s->worker-1)*radix->partitions+partition];
        struct radix_count_partial count={0};
        if(packed_count) {
            count.key=g->key; count.count=g->values[0].count; count.isnull=g->key_flags==2;
        }
        int64_t status=spool_append_context(s,stream,packed_count?(const void *)&count:g,
            packed_count?sizeof(count):local->group_stride,s->owner->context,radix->resident_reserve);
        if(status) return status;
        ++local->group_flush;
    }
    memset(local->group_entries,0,(size_t)local->group_capacity*sizeof(*local->group_entries));
    atomic_store_explicit(local->group_count,0,memory_order_relaxed);
    local->group_flush=local->group_phase=0;
    return 0;
}
/* Reduce a group column by column when its arguments have a fixed scale.
 * Equal SUM/AVG arguments can share the same partial sum and count. */
int group_reduce_run(const struct qp_node *n,const struct projection_batch *b,
    const uint16_t *rows,unsigned count,struct group_value *values)
{
    for(unsigned a=0;a<n->expr.naggs;++a) {
        int expr=b->group_values[a];
        if(expr<0) { values[a].count=count; continue; }
        unsigned kind=n->expr.aggs[a].kind;
        bool sum=kind==Q_SUM || kind==Q_AVG;
        unsigned prior=0;
        for(;prior<a;++prior) {
            unsigned previous=n->expr.aggs[prior].kind;
            if(b->group_values[prior]==expr && sum==(previous==Q_SUM || previous==Q_AVG)) break;
        }
        if(prior<a) { values[a]=values[prior]; continue; }
        const struct qs_vector *v=&b->vectors[expr];
        __int128 total=0; uint64_t valid=0; unsigned dscale=0;
        for(unsigned i=0;i<count;++i) {
            unsigned r=rows[i];
            if(v->errors[r]) return -5;
            if(qs_vector_null(v,r)) continue;
            ++valid;
            if(sum) {
                if(__builtin_add_overflow(total,qs_vector_value(v,r),&total)) return -5;
                unsigned scale=qs_vector_dscale(v,r);
                if(scale>dscale) dscale=scale;
            }
        }
        values[a]=(struct group_value){.count=valid,.sum={
            .value=total,.scale=b->plan.exprs[expr].scale,.dscale=dscale,.isnull=!valid}};
    }
    return 0;
}
/* With few groups, assign the vector's rows once and reduce each
 * group columnwise. The existing local table still owns the state and spill. */
/* Keep keys and aggregate arguments in vectors. Only rows rejected by a full
 * shared table enter the existing AFDM adapter, so its owner can grow/spill
 * between waves without losing the remainder of this vector. */
static int64_t group_sink_batch(struct execution *s,struct qs_result *out)
{
    unsigned id=s->group_sink-1,columns=s->plan->nodes[s->root].expr.nout;
    struct operator_state *o=&s->owner->nodes[id],*local=&s->nodes[id];
    if(local->group_local && local->group_local->group_radix && local->group_local->group_phase) {
        int64_t status=radix_flush(s,id); if(status) return status;
    }
    while(out->begin<out->rows) {
        struct qs_cell *row=(struct qs_cell *)out->data+(size_t)out->begin*columns;
        int status=group_sink_row(s,id,o,row,1); if(status<0) return status;
        if(!status) {
            if(local->group_local && local->group_local->group_radix) {
                local->group_local->group_phase=1;
                int64_t flush=radix_flush(s,id); if(flush) return flush;
                continue;
            }
            memmove((struct qs_cell *)out->data+(size_t)local->group_batch_kept++*columns,row,columns*sizeof(*row));
        }
        ++out->begin;
    }
    out->rows=local->group_batch_kept; out->begin=local->group_batch_kept=0; return 0;
}
static void group_drop_local(struct execution *s,struct operator_state *local)
{
    if(!local) return;
    cs_work_free(s->context,local->groups); cs_work_free(s->context,local->group_entries);
    cs_work_free(s->context,local);
}
/* Called only after the producing CUs have finished. Keep the current partial
 * state until its merge succeeds, including across table growth and spill. */
static struct compact_group *group_local_next(struct execution *s,struct operator_state *o)
{
    while(o->group_local_slot<o->group_local_count) {
        struct operator_state *local=o->group_locals[o->group_local_slot];
        unsigned rows=atomic_load_explicit(local->group_count,memory_order_relaxed);
        if(o->group_local_row<local->group_flush) o->group_local_row=local->group_flush;
        if(rows && !o->group_local_row && !atomic_load(o->group_count) &&
           local->group_capacity==o->group_capacity) {
            /* Transfer the first table instead of copying all its groups. */
            cs_work_free(s->context,o->groups); cs_work_free(s->context,o->group_entries);
            o->groups=local->groups; o->group_entries=local->group_entries;
            local->groups=NULL; local->group_entries=NULL; atomic_store(o->group_count,rows);
            o->group_local_row=rows;
        }
        if(o->group_local_row<rows) return group_at(local,o->group_local_row);
        group_drop_local(s,local); o->group_locals[o->group_local_slot++]=NULL; o->group_local_row=0;
    }
    cs_work_free(s->context,o->group_locals); o->group_locals=NULL;
    o->group_local_count=o->group_local_slot=0; return NULL;
}

static size_t group_reserve(const struct execution *s,const struct qp_node *n)
{
    size_t reserve=sort_spill_reserve(s);
    /* Hash groups are allocated before their scan launches. Reserve the actual
     * worker state too, or a small query budget can lose
     * all spill space as soon as the workers start. Active workers are already
     * charged to the parent and must not be counted a second time. */
    if(s->plan->workers>1 && !s->parallel_started && s->plan->nodes[n->left].parallel && !s->nodes[n->left].finished) {
        int leaf=qp_probe_scan(s->plan,n->left);
        size_t index=leaf>=0 && s->plan->nodes[leaf].ordered_index?QS_INDEX_WORK_BYTES+sizeof(struct qs_plan):0;
        reserve+=s->plan->workers*(sizeof(*s)+s->plan->count*sizeof(*s->nodes)+sizeof(struct qs_plan)+index+256);
    }
    return reserve;
}
/* A parent must not reserve a large estimated group table while a blocking
 * child still needs to build/materialize its input. Start small there and grow
 * from real input after those dependencies run; all bytes share one context. */
static int group_input_pending(const struct execution *s,int id,bool defer_probe)
{
    if(id<0) return 0;
    const struct qp_node *n=&s->plan->nodes[id]; const struct operator_state *o=&s->nodes[id];
    if(n->kind==QP_AGGREGATE && n->nkeys) return !o->finished;
    /* A ready hash build still does not establish the number of distinct
     * probe rows. Keep the small initial table for a key-only dedup sink. */
    if(n->kind==QP_JOIN && n->hash && !o->hash_fallback &&
        (defer_probe || (!o->hash_phase && !graph_state_owner(s,id)->hash_phase))) return 1;
    if(n->kind==QP_SORT && !o->left_sort.drained) return 1;
    if(n->kind==QP_MATERIALIZED) return 1;
    return group_input_pending(s,n->left,defer_probe) || group_input_pending(s,n->right,defer_probe);
}
static void group_partial_values(const struct qp_node *n,const struct group_value *values,struct qs_cell *row)
{
    unsigned counts=n->nkeys+n->expr.naggs;
    for(unsigned a=0;a<n->expr.naggs;++a) {
        row[n->nkeys+a]=values[a].sum;
        if(!(a&1)) row[counts+a/2]=(struct qs_cell){.value=values[a].count};
        else row[counts+a/2].value=(unsigned __int128)row[counts+a/2].value |
            (unsigned __int128)values[a].count<<64;
    }
}
static struct group_value group_partial_value(const struct qp_node *n,const struct qs_cell *row,unsigned a)
{
    unsigned __int128 counts=row[n->nkeys+n->expr.naggs+a/2].value;
    return (struct group_value){.sum=row[n->nkeys+a],.count=(uint64_t)(counts>>((a&1)*64))};
}
static bool radix_claim_growth(struct radix_group *radix,size_t bytes)
{
    size_t available=atomic_load_explicit(&radix->source_growth,memory_order_relaxed);
    while(available>=bytes)
        if(atomic_compare_exchange_weak_explicit(&radix->source_growth,&available,available-bytes,
            memory_order_relaxed,memory_order_relaxed)) return true;
    return false;
}

static int64_t radix_input(struct execution *s,unsigned id,struct operator_state *o)
{
    struct radix_group *radix=o->group_radix;
    cs_work_context *owner=s->worker?s->owner->context:s->context;
    const struct qp_node *n=&s->plan->nodes[id];
    bool packed_count=radix_count_only(n);
    unsigned bytes=packed_count?sizeof(struct radix_count_partial):o->group_stride;
    while(o->radix_writer<radix->workers) {
        struct spool *p=&radix->streams[o->radix_writer*radix->partitions+o->radix_partition];
        unsigned block_rows=spool_block_bytes(p)/bytes;
        if(o->radix_row && !(o->radix_row%block_rows)) {
            unsigned block=o->radix_row/block_rows-1;
            if(block<p->blocks) {
                struct spool_block *page=&p->pages[block];
                if(page->data) {
                    cs_work_free(owner,page->data); page->data=NULL;
                    if(s->worker && radix->bounded_source)
                        atomic_fetch_add_explicit(&radix->source_growth,spool_block_bytes(p),memory_order_relaxed);
                } else if(page->token) {
                    uint64_t token=page->token; page->token=0;
                    return csNeedFree(token);
                }
            }
        }
        if(o->radix_row==p->rows) {
            size_t released=(p->tail?spool_block_bytes(p):0)+(size_t)p->capacity*sizeof(*p->pages);
            cs_work_free(s->context,p->cache); p->cache=NULL;
            cs_work_free(owner,p->tail); p->tail=NULL;
            cs_work_free(owner,p->pages); p->pages=NULL;
            if(s->worker && radix->bounded_source)
                atomic_fetch_add_explicit(&radix->source_growth,released,memory_order_relaxed);
            ++o->radix_writer; o->radix_row=0; continue;
        }
        _Alignas(16) unsigned char storage[sizeof(struct compact_group)+QS_AGGS*sizeof(struct group_value)+QS_KEYS*sizeof(struct qs_cell)+32];
        if(bytes>sizeof(storage)) return -5;
        int64_t status=spool_row_bytes(s,p,o->radix_row,bytes,storage); if(status) return status;
        if(packed_count) {
            const struct radix_count_partial *count=(const void *)storage;
            const struct group_value partial={.count=count->count};
            o->right[0]=(struct qs_cell){.value=count->key,.isnull=count->isnull};
            group_partial_values(n,&partial,o->right);
        } else {
            const struct compact_group *g=(const void *)storage;
            group_keys(n,o,g,o->right);
            group_partial_values(n,g->values,o->right);
        }
        return 2;
    }
    return 0;
}
static int64_t hash_aggregate_state(struct execution *s,unsigned id,struct operator_state *o,
    struct qs_cell *row,enum group_read_mode prepare,int raw)
{
    struct qp_node *n=&s->plan->nodes[id];
    struct sorted_rows *sort=&o->left_sort;
    if(!qp_hash_aggregate_supported(n)) { n->hash=0; return prepare?3:aggregate_next_impl(s,id,row,raw); }
    int wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
    if(!o->group_capacity) {
        /* Keep the insertion counter away from read-mostly capacity/stride
         * fields. The root context owns this small aligned allocation until
         * query close; table resize does not move it or change its count. */
        if(!o->group_count) {
            void *storage=cs_work_alloc(s->context,128);
            if(!storage) { n->hash=0; return prepare?3:aggregate_next_impl(s,id,row,raw); }
            o->group_count=(void *)(((uintptr_t)storage+63)&~(uintptr_t)63);
            atomic_init(o->group_count,0);
        }
        struct cs_work_context_stats memory;
        if(cs_work_get_context_stats(s->context,&memory)) return -4;
        size_t available=cs_work_available(s->context),reserve=group_reserve(s,n);
        available=available>reserve?available-reserve:available/2;
        o->group_integer=1;
        /* Runtime strings are canonical dictionary IDs, also 64-bit keys. */
        for(unsigned k=0;k<qp_group_keys(n);++k) {
            unsigned type=n->expr.exprs[n->expr.keys[k]].type;
            if(type!=Q_INTEGER && type!=Q_STRING) o->group_integer=0;
        }
        o->group_stride=sizeof(*o->groups)+n->expr.naggs*sizeof(struct group_value)+
            (wide?group_key_bytes(n,o):0);
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
        size_t bytes_per_slot=o->group_stride-o->group_stride/8+sizeof(struct group_entry);
        while(!group_input_pending(s,n->left,s->stream_waves && key_only) &&
              (!wide || n->estimated_groups>0 || s->plan->nodes[n->left].kind==QP_SCAN ||
              (key_only && s->plan->nodes[n->left].parallel)) && capacity<groups*8/7 &&
              (size_t)capacity*2*bytes_per_slot<=available && capacity<UINT_MAX/2)
            capacity*=2;
        for(;;) {
            /* Only the compact lookup array needs clearing. New dense
             * records are initialized by the CU that publishes each key.
             * The 7/8 load limit also bounds the number of stored records. */
            o->groups=cs_work_alloc(s->context,(size_t)(capacity-capacity/8)*o->group_stride);
            o->group_entries=o->groups?cs_work_calloc(s->context,capacity,sizeof(*o->group_entries)):NULL;
            if(o->group_entries) break;
            cs_work_free(s->context,o->groups); o->groups=NULL;
            if(capacity==128) break;
            capacity/=2;
        }
        if(!o->groups) { n->hash=0; return prepare?3:aggregate_next_impl(s,id,row,raw); }
        o->group_capacity=capacity;
    }
    if(prepare==GROUP_ALLOCATE) return 3;
    for(;;) {
        if(o->group_phase==0) {
            if(!o->have_left) {
                if(o->radix_source) {
                    int64_t status=radix_input(s,id,o);
                    if(status!=2) {
                        if(status) return status;
                        o->input_done=1; o->group_phase=1; continue;
                    }
                    o->have_left=1;
                } else if(o->group_locals && !o->group_radix && !s->parallel_started) {
                    if(!group_local_next(s,o)) continue;
                    o->have_left=o->group_partial=1;
                } else {
                    /* An explicit sink drains only its submitted worker
                     * wave, including any rows retained when its table filled. */
                    int64_t status=o->graph_wave_input?
                        (o->graph_wave_drained?0:aggregate_wave_next(s,n->left,o->left,id)):
                        next_row(s,n->left,o->left);
                    if(status==QP_WAIT || status<0 || status>2) return status;
                    o->have_left=status==2;
                    if(!status) {
                        if(o->graph_wave_input) o->graph_wave_drained=true;
                        if(o->group_locals && !o->group_radix) continue;
                        o->input_done=1; o->group_phase=1; continue;
                    }
                }
            }
            int added;
            if(o->radix_source) {
                struct group_value values[QS_AGGS];
                for(unsigned a=0;a<n->expr.naggs;++a)
                    values[a]=group_partial_value(n,o->right,a);
                added=group_sink_values(s,id,o,o->right,values,0);
            } else if(o->group_partial) {
                struct operator_state *local=o->group_locals[o->group_local_slot];
                struct compact_group *g=group_at(local,o->group_local_row); struct qs_cell keys[QS_KEYS];
                group_keys(n,local,g,keys); added=group_sink_values(s,id,o,keys,g->values,0);
            } else added=group_sink_row(s,id,o,o->left,0);
            if(added<0) return added;
            if(!added) {
                unsigned capacity=o->group_capacity*2;
                struct cs_work_context_stats m;
                if(cs_work_get_context_stats(s->context,&m)) return -4;
                cs_work_context *context=s->worker && o->radix_source?s->owner->context:s->context;
                size_t available=cs_work_available(context),reserve=group_reserve(s,n);
                if(o->radix_source)
                    reserve+=o->group_radix->sources*qs_sort_min_workspace();
                /* Keep spill/worker space after replacing the old buffers.
                 * Realloc may grow in place; the allocator, rather than a
                 * pessimistic quota, enforces the actual transient peak. */
                size_t bytes_per_slot=o->group_stride-o->group_stride/8+sizeof(struct group_entry);
                size_t released=(size_t)o->group_capacity*bytes_per_slot;
                available+=released;
                available=available>reserve?available-reserve:0;
                struct radix_group *radix=s->worker && o->radix_source && o->group_radix->bounded_source?
                    o->group_radix:NULL;
                size_t old_entries=(size_t)o->group_capacity*sizeof(*o->group_entries);
                size_t growth=released+old_entries;
                bool admitted=capacity>o->group_capacity && (radix?radix_claim_growth(radix,growth):
                    (size_t)capacity*bytes_per_slot<=available);
                struct group_entry *entries=admitted?cs_work_calloc(context,capacity,sizeof(*entries)):NULL;
                void *larger=entries?cs_work_realloc(context,o->groups,(size_t)(capacity-capacity/8)*o->group_stride):NULL;
                if(larger) {
                    struct group_entry *old=o->group_entries;
                    o->groups=larger; o->group_capacity=capacity;
                    o->group_entries=entries;
                    if(o->radix_source) {
                        struct execution *owner=s->worker?s->owner:s;
                        struct operator_state *table=owner->nodes[id].group_locals[s->worker?s->worker-1:0];
                        table->groups=larger; table->group_capacity=capacity; table->group_entries=entries;
                    }
                    unsigned rows=atomic_load_explicit(o->group_count,memory_order_relaxed);
                    for(unsigned i=0;i<rows;++i) {
                        const struct compact_group *v=group_at(o,i);
                        uint64_t hash=wide?(uint64_t)v->key:mix_hash(v->key_flags==2?0:(uint64_t)v->key);
                        unsigned slot=hash&(capacity-1);
                        while(atomic_load_explicit(&entries[slot].state,memory_order_relaxed)) slot=(slot+1)&(capacity-1);
                        entries[slot].row=i;
                        atomic_store_explicit(&entries[slot].state,group_entry_state(v->key_flags,hash),memory_order_relaxed);
                    }
                    cs_work_free(context,old);
                    if(radix) atomic_fetch_add_explicit(&radix->source_growth,old_entries,memory_order_relaxed);
                    continue;
                }
                cs_work_free(context,entries);
                if(radix && admitted)
                    atomic_fetch_add_explicit(&radix->source_growth,growth,memory_order_relaxed);
                o->group_phase=1; continue;
            }
            if(o->group_partial) { ++o->group_local_row; o->group_partial=0; }
            if(o->radix_source) ++o->radix_row;
            o->have_left=0; continue;
        }
        if(o->group_phase==1) {
            if(o->group_radix && !o->radix_source) { o->group_phase=4; continue; }
            if(o->input_done && !o->hash_fallback) { o->group_phase=3; continue; }
            o->hash_fallback=1;
            struct qs_plan partial={.nout=qp_group_partial_width(n->nkeys,n->expr.naggs)}; int columns[QS_KEYS];
            for(unsigned k=0;k<n->nkeys;++k) {
                columns[k]=k; partial.outputs[k]=s->plan->nodes[n->left].expr.outputs[n->left_keys[k]];
            }
            /* The sorter carries partial states as opaque wide cells. */
            for(unsigned a=n->nkeys;a<partial.nout;++a) partial.outputs[a].type_oid=1700;
            /* The radix wave admits one minimum sort workspace per Source.
             * An individual sorter must stay within that share. */
            if(o->radix_source && !sort->initialized)
                sort->plan.work_mem_bytes=qs_sort_min_workspace();
            int64_t status=sort_init(s,sort,&partial,qp_group_keys(n),columns,NULL); if(status) return status;
            if(sort->input_pending) {
                status=sort_call(s,sort,0); if(status) return status;
                sort->input_pending=0; sort->input->rows=0;
            }
            while(o->group_flush<atomic_load_explicit(o->group_count,memory_order_relaxed) && sort->input->rows<QS_RESULT_ROWS) {
                unsigned slot=o->group_flush++;
                struct compact_group *g=group_at(o,slot);
                struct qs_cell *out=sort->input->data[sort->input->rows++].cells;
                group_keys(n,o,g,out);
                group_partial_values(n,g->values,out);
            }
            if(sort->input->rows) { sort->input_pending=1; continue; }
            if(o->input_done) {
                if(!o->radix_source) {
                    cs_work_free(s->context,o->groups); o->groups=NULL;
                    cs_work_free(s->context,o->group_entries); o->group_entries=NULL;
                }
                sort->complete=1; sort->group.count=1; sort->group.states[0]=sort->task.state;
                cs_work_free(s->context,sort->input); sort->input=NULL;
                o->group_phase=2;
            } else {
                memset(o->group_entries,0,(size_t)o->group_capacity*sizeof(*o->group_entries));
                atomic_store_explicit(o->group_count,0,memory_order_relaxed);
                o->group_flush=0; o->group_phase=0;
            }
            continue;
        }
        if(o->group_phase==3) {
            if(prepare) return 3;
            while(o->group_flush<atomic_load_explicit(o->group_count,memory_order_relaxed)) {
                unsigned slot=o->group_flush++;
                struct compact_group *g=group_at(o,slot);
                struct qs_cell keys[QS_KEYS]; group_keys(n,o,g,keys);
                int64_t result=group_result(s,n,g->values,keys,row,raw); if(result) return result;
            }
            if(!o->radix_source) {
                cs_work_free(s->context,o->groups); o->groups=NULL;
                cs_work_free(s->context,o->group_entries); o->group_entries=NULL;
            }
            o->finished=1; return 0;
        }
        if(prepare) return 3;
        if(o->group_phase==4) return radix_source_next(s,id,row,raw);
        /* Merge partial value/count pairs; AVG is divided only for final output. */
        if(!o->have_right && !o->group_ready) {
            if((s->pending_sort==sort || sort->position==sort->output->rows) && !sort->drained) {
                int64_t status=sort_call(s,sort,1); if(status) return status;
                sort->position=0; sort->drained=sort->output->finished;
            }
            if(sort->position<sort->output->rows) {
                memcpy(o->right,sort->output->data[sort->position++].cells,qp_group_partial_width(n->nkeys,n->expr.naggs)*sizeof(*o->right)); o->have_right=1;
            } else o->group_ready=1;
        }
        if(!o->phase) {
            if(!o->have_right) {
                cs_work_context *owner=s->worker && o==graph_state_owner(s,id)?s->owner->context:s->context;
                cs_work_free(owner,sort->output); sort->output=NULL; o->finished=1; return 0;
            }
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
            for(;o->aggregate_position<n->expr.naggs;++o->aggregate_position) {
                unsigned a=o->aggregate_position;
                struct group_value partial=group_partial_value(n,o->right,a);
                int status=group_update(s,n,a,&groups[a],partial.sum,partial.count);
                if(status) return status;
                o->accum[a].sum=groups[a].sum; o->accum[a].count=groups[a].count;
            }
            o->aggregate_position=0; o->have_right=0; continue;
        }
        o->phase=0;
        int64_t result=group_result(s,n,groups,o->keys,row,raw); if(result) return result;
    }
}
static void release_aggregate_source(struct execution *s,unsigned id);
static void group_drop_local(struct execution *s,struct operator_state *local);

#ifdef QP_SPLIT_RUNTIME
static void start_radix_source(struct execution *s,unsigned id)
{
    const struct operator_state *o=&s->nodes[id];
    struct radix_group *radix=o->group_radix;
    size_t available=cs_work_available(s->context);
    size_t reserve=(qs_sort_min_workspace()+4*QS_SORT_IO_BYTES)*radix->sources;
    atomic_store_explicit(&radix->source_growth,available>reserve?available-reserve:0,memory_order_relaxed);
}

/* A partition is the unit of Source work. Its partial count bounds its
 * groups; use the pre-HAVING estimate to allow for duplicates across writers. */
static void prepare_radix_source(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    struct radix_group *radix=o->group_radix;
    if(radix->bounded_source && !o->source_workers) {
        uint64_t largest=0,total=0;
        unsigned nonempty=0;
        for(unsigned part=0;part<radix->partitions;++part) {
            uint64_t rows=0;
            for(unsigned w=0;w<radix->workers;++w) rows+=radix->streams[w*radix->partitions+part].rows;
            if(rows>largest) largest=rows;
            total+=rows;
            nonempty+=rows!=0;
        }
        const struct qp_node *n=&s->plan->nodes[id];
        double groups=n->estimated_groups;
        double distinct=total && groups>0 && groups<total?largest*(groups/total):largest;
        size_t workspace=qs_sort_min_workspace();
        double bytes=distinct*(o->group_stride+sizeof(*o->group_entries))*8/7;
        double per_source=bytes>2*workspace?bytes:2*workspace;
        size_t available=cs_work_available(s->context);
        size_t writer_bytes=(size_t)o->group_locals[0]->group_capacity*
            (o->group_stride-o->group_stride/8+sizeof(*o->group_entries));
        size_t source_budget=available+writer_bytes*o->group_local_count;
        unsigned sources=(source_budget-source_budget/4)/per_source;
        if(sources>nonempty) sources=nonempty;
        if(sources>s->plan->workers) sources=s->plan->workers;
        radix->sources=sources?sources:1;
        for(unsigned w=radix->sources;w<o->group_local_count;++w) {
            group_drop_local(s,o->group_locals[w]);
            o->group_locals[w]=NULL;
        }
        o->start_source=start_radix_source;
        if(s->profiling)
            fprintf(stderr,"CSD radix source node=%u partitions=%u slots=%u available=%zu largest_partial=%llu\n",
                id,nonempty,radix->sources,available,(unsigned long long)largest);
    }
    o->source_workers=radix->sources;
    o->source_workspace=radix->sources*qs_sort_min_workspace()*(radix->bounded_source?2:1);
}

static int admit_aggregate_source(const struct execution *s,unsigned id,bool direct)
{
    const struct operator_state *o=&s->nodes[id];
    if(o->group_phase<2) return -1;
    if(o->group_phase==2 && !o->group_radix) return 0;
    if(!o->group_radix && (o->group_phase!=3 || !o->groups ||
       (direct && atomic_load(o->group_count)<QP_VECTOR_ROWS))) return 3;
    return 0;
}
#endif

int64_t hash_aggregate_next_impl(struct execution *s,unsigned id,struct qs_cell *row,
    enum group_read_mode prepare,int raw)
{
    struct operator_state *o=&s->nodes[id];
    int64_t status=hash_aggregate_state(s,id,o,row,prepare,raw);
    if(prepare==GROUP_PREPARE && status==3) {
#ifdef QP_SPLIT_RUNTIME
        o->admit_source=admit_aggregate_source;
        if(o->group_radix) prepare_radix_source(s,id);
#endif
        o->finish_source=release_aggregate_source;
        o->read_projection=projection_group_fill;
    }
    return status;
}

static void close_radix_source(struct operator_state *state)
{ graph_close_sort(&state->group_radix->serial->left_sort); }

int64_t radix_source_next(struct execution *s,unsigned id,struct qs_cell *row,int raw)
{
    struct execution *owner=s->worker?s->owner:s;
    struct operator_state *global=&owner->nodes[id],*o=&s->nodes[id];
    struct radix_group *radix=global->group_radix;
    if(s->worker>radix->sources) return 0;
    if(!s->worker) {
        if(!radix->serial) radix->serial=cs_work_calloc(s->context,1,sizeof(*o));
        if(!radix->serial) return -4;
        global->close=close_radix_source;
        o=radix->serial;
    }
    if(!o->radix_source) {
        struct operator_state *table=global->group_locals[s->worker?s->worker-1:0];
        o->groups=table->groups; o->group_entries=table->group_entries; o->group_count=table->group_count;
        o->group_capacity=table->group_capacity; o->group_stride=table->group_stride; o->group_integer=table->group_integer;
        o->group_radix=radix; o->radix_source=1; o->radix_partition=UINT_MAX;
    }
    for(;;) {
        if(o->radix_partition==UINT_MAX) {
            unsigned part=atomic_fetch_add_explicit(&radix->next,1,memory_order_relaxed);
            if(part>=radix->partitions) return 0;
            o->radix_partition=part; o->radix_writer=0; o->radix_row=0;
            o->phase=o->group_phase=o->group_flush=o->input_done=o->hash_fallback=0;
            o->have_left=o->have_right=o->group_ready=o->finished=o->aggregate_position=0;
            atomic_store_explicit(o->group_count,0,memory_order_relaxed);
            memset(o->group_entries,0,(size_t)o->group_capacity*sizeof(*o->group_entries));
            o->left_sort=(struct sorted_rows){0};
        }
        int64_t status=hash_aggregate_state(s,id,o,row,GROUP_READ,raw); if(status) return status;
        o->radix_partition=UINT_MAX;
    }
}

/* Sorted partials have one merge cursor. Keep that cursor with the query
 * and let one worker advance it, using the worker's own I/O buffer. */
int64_t aggregate_sorted_source_next(struct execution *s,unsigned id,struct qs_cell *row,int raw)
{
    struct operator_state *source=&s->owner->nodes[id];
    if(s->worker!=1 || source->finished) return 0;
    return hash_aggregate_state(s,id,source,row,GROUP_READ,raw);
}
static int64_t hash_aggregate_next(struct execution *s,unsigned id,struct qs_cell *row,int raw)
{
    /* Pure in-memory output is one call per group: leave it untimed. */
    if(!s->profiling || s->nodes[id].group_phase==3 ||
       (s->group_source_profiling && s->nodes[id].group_phase==2))
        return hash_aggregate_next_impl(s,id,row,GROUP_READ,raw);
    unsigned op=s->nodes[id].group_phase==2?QS_AGG_SOURCE:QS_AGG_SINK;
    struct blocking_timer timer=blocking_begin(s);
    int64_t status=hash_aggregate_next_impl(s,id,row,GROUP_READ,raw); blocking_end(s,op,timer); return status;
}
/* Consume a contiguous key range from expression columns. The next group's
 * first row stays in the batch; the current accumulators survive I/O and
 * vector boundaries. Return 2 for a completed group, 3 for scalar fallback. */
static int64_t stream_group_vectors(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    for(;;) {
        int64_t status=projection_fill(s,id,0);
        if(status!=2) {
            if(!status) { o->input_done=1; return o->phase?2:0; }
            return status;
        }
        struct projection_batch *b=o->projection;
        unsigned first=b->position,end=first;
        while(end<b->count) {
            struct qs_cell keys[QS_KEYS];
            for(unsigned k=0;k<n->nkeys;++k) {
                int index=b->group_keys[k]; if(b->vectors[index].errors[end]) return -5;
                keys[k]=qs_vector_cell(&b->plan,b->vectors,index,end);
            }
            if(!o->phase) {
                for(unsigned col=0;col<b->plan.nout;++col) {
                    int index=b->plan.outputs[col].index; if(b->vectors[index].errors[end]) return -5;
                    o->first[col]=qs_vector_cell(&b->plan,b->vectors,index,end);
                }
                memcpy(o->keys,keys,n->nkeys*sizeof(*keys));
                memset(o->accum,0,sizeof(o->accum)); o->phase=1;
            } else {
                int equal=group_equal(s,n,keys,o->keys); if(equal<0) return equal;
                if(!equal) break;
            }
            ++end;
        }
        if(end==first) return 2;
        for(unsigned a=b->aggregate_position;a<n->expr.naggs;++a,b->aggregate_position=a) {
            const struct qs_agg *agg=&n->expr.aggs[a];
            if(agg->kind==Q_COUNT_STAR) { o->accum[a].count+=end-first; continue; }
            int index=b->group_values[a];
            if(agg->kind==Q_COUNT && b->vectors[index].all_valid) {
                o->accum[a].count+=end-first; continue;
            }
            for(unsigned r=b->aggregate_row?b->aggregate_row:first;r<end;++r,b->aggregate_row=r) {
                if(b->vectors[index].errors[r]) return -5;
                struct qs_cell value=qs_vector_cell(&b->plan,b->vectors,index,r);
                if(!value.isnull) { int added=accumulate_value(s,n,agg,&o->accum[a],value); if(added) return added; }
            }
            b->aggregate_row=0;
        }
        b->aggregate_position=0;
        b->position=end; s->metrics.matched+=end-first;
        if(end<b->count) return 2;
    }
}
int64_t aggregate_next_impl(struct execution *s,unsigned id,struct qs_cell *row,int raw)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    if(o->finished) return 0;
    if(n->hash) return hash_aggregate_next(s,id,row,raw);
    if(n->nkeys && n->left>=0 && !n->sorted_input && !n->distinct && !o->phase && !o->emitted &&
       input_ordered(s->plan,n->left,n->nkeys,n->left_keys,NULL)) s->plan->nodes[id].sorted_input=1;
    if(n->nkeys && n->sorted_input && !n->distinct && !n->distinct_aggs && !o->input_done && !o->have_left &&
       n->left>=0 && !(!s->hash_input && !s->worker && s->plan->workers>1 && s->plan->nodes[n->left].parallel)) {
        int status=projection_init(s,id,0); if(status) return status;
    }
    /* Initialize before starting the child pipeline: parallel workers combine
     * their local states here before the first (and only) output row. */
    if(!n->nkeys && !o->phase && !o->emitted) {
        memset(o->accum,0,sizeof(o->accum)); o->phase=1;
    }
    if(!n->nkeys && !n->distinct && !n->distinct_aggs && !o->input_done && !o->emitted && n->left>=0 &&
       !(!s->hash_input && !s->worker && s->plan->workers>1 && s->plan->nodes[n->left].parallel)) {
        int64_t status=projection_init(s,id,0); if(status) return status;
        while(o->projection) {
            status=projection_fill(s,id,0);
            if(!status) { o->input_done=1; break; }
            if(status==3) break; /* Existing scalar fallback retains its cursor. */
            if(status!=2) return status;
            struct blocking_timer timer={0}; if(s->profiling) timer=blocking_begin(s);
            status=aggregate_sink_vectors(s,id);
            if(s->profiling) blocking_end(s,QS_AGG_SINK,timer);
            if(status) return status;
        }
    }
    for(;;) {
        int ready=0;
        if(n->nkeys && o->projection) {
            /* One call per output group: use CSF/enclosing sink timings,
             * rather than adding clock reads for every small group. */
            int64_t status=stream_group_vectors(s,id);
            if(status!=2 && status!=3) return status;
            ready=status==2;
        }
        if(!ready && !o->have_left && !o->input_done) {
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
                int status=initialize_group_keys(s,n,o); if(status) return status;
            }
            o->phase=1;
        }
        if(o->have_left) {
            int same=o->aggregate_input && o->aggregate_input->active==3?1:group_same(s,n,o); if(same<0) return -5;
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
        if(raw) {
            memcpy(row,o->keys,n->nkeys*sizeof(*row));
            memcpy(row+n->nkeys,values,n->expr.naggs*sizeof(*row)); return 2;
        }
        int count=project(s,n,o->first,values,row); if(count<0) return count;
        if(count) return 2;
    }
}

int64_t aggregate_worker_source(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->nodes[id];
    const struct operator_state *source=&s->owner->nodes[id];
    int64_t status=projection_init(s,id,1); if(status) return status;
    if(o->source_projection) return projection_next(s,id,row,1);
    if(source->group_radix) return radix_source_next(s,id,row,0);
    if(source->group_phase==2) return aggregate_sorted_source_next(s,id,row,0);
    for(;;) {
        const struct compact_group *g=group_source_next(s,id); if(!g) return 0;
        struct qs_cell keys[QS_KEYS]; group_keys(n,source,g,keys);
        status=group_result(s,n,g->values,keys,row,0); if(status) return status;
    }
}

/* DuckDB-style local ungrouped states, combined after the worker finishes.
 * AVG transfers sum/count; only the coordinator performs the final division. */
int aggregate_combine(struct execution *s,unsigned id,const struct operator_state *from)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *to=&s->nodes[id];
    for(unsigned i=to->aggregate_position;i<n->expr.naggs;++i,to->aggregate_position=i) {
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
    to->aggregate_position=0; return 0;
}
/* A scan/projection-only input can feed the local aggregate directly from
 * expression vectors. Row batches remain the adapter for joins and other
 * operators that have not yet acquired a column-batch interface. */
static int aggregate_sink_batch(struct execution *s,struct qs_result *out)
{
    unsigned id=s->aggregate_sink-1,columns=s->plan->nodes[s->root].expr.nout;
    struct operator_state *o=&s->nodes[id];
    for(;out->begin<out->rows;++out->begin) {
        memcpy(o->left,(struct qs_cell *)out->data+(size_t)out->begin*columns,columns*sizeof(*o->left));
        int status=accumulate(s,&s->plan->nodes[id],o); if(status) return status;
    }
    out->rows=out->begin=0; return 0;
}
/* Local Hash Aggregate tables use the same layout/update functions as the
 * shared table. Admission uses the query's actual free memory, retaining the
 * existing spill reserve; no per-CU fraction is taken from the query limit. */
static void aggregate_drop_worker_tables(struct execution *s,struct operator_state *o);

static int parallel_radix_begin(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id]; const struct qp_node *n=&s->plan->nodes[id];
    double groups=n->estimated_groups>0?n->estimated_groups:n->estimated_rows;
    if(o->group_locals || atomic_load_explicit(o->group_count,memory_order_relaxed)) return 0;
    size_t per_slot=o->group_stride-o->group_stride/8+sizeof(*o->group_entries);
    size_t available=cs_work_available(s->context)+(size_t)o->group_capacity*per_slot;
    unsigned workers=s->plan->workers;
    size_t reserve=sort_spill_reserve(s),workspace=qs_sort_min_workspace();
    unsigned sources=workers;
    if(available<=reserve+workers*workspace) {
        if(available<=reserve+workspace) return 0;
        /* All Sinks can reduce the scan concurrently. Source partitions
         * are independent and need not all hold a sorter at once. */
        sources=(available-reserve)/(4*workspace);
        if(!sources) sources=1;
    }
    reserve+=sources*workspace;
    unsigned resident=o->group_capacity;
    while(resident<UINT_MAX/2 && resident<groups*8/7) resident*=2;
    bool external=(size_t)resident*per_slot>available-reserve;
    const unsigned local_limit=4096;
    /* A large, already sized shared table can absorb all estimated groups.
     * Small private tables would repeatedly flush the same keys in this case.
     * Keep partition streams when the shared table would need to grow. */
    if(!external && resident>local_limit && resident==o->group_capacity) return 0;
    /* Keep a bounded lookup table even when the estimated groups fit.
     * Finished chunks can stay resident instead of requiring a shared table. */
    bool bounded=sources<workers;
    unsigned block_bytes=bounded?QS_SORT_IO_BYTES/4:QS_SORT_IO_BYTES;
    unsigned capacity=128,partitions=2;
    unsigned target=external?UINT_MAX/2:(resident<local_limit?resident:local_limit);
    size_t table_budget=(available-reserve)*3/5/workers;
    while(capacity<target && (size_t)capacity*2*per_slot<=table_budget) capacity*=2;
    while(partitions<64 && (partitions<workers || partitions<groups/(capacity-capacity/8))) partitions*=2;
    size_t tails=(size_t)workers*partitions*block_bytes;
    size_t tail_budget=(available-reserve)/(bounded?2:4);
    while(partitions>2 && tails>tail_budget) { partitions/=2; tails/=2; }
    if(bounded && tails<available-reserve) {
        table_budget=(available-reserve-tails)/2/workers;
        while(capacity>128 && (size_t)capacity*per_slot>table_budget) capacity/=2;
    }
    if((size_t)capacity*per_slot*workers+tails+reserve>available) return 0;
    struct radix_group *radix=cs_work_calloc(s->context,1,sizeof(*radix));
    if(!radix) return -4;
    radix->partitions=partitions; radix->workers=workers; radix->sources=sources;
    atomic_init(&radix->next,0);
    atomic_init(&radix->source_growth,0);
#ifdef QP_SPLIT_RUNTIME
    radix->bounded_source=bounded;
#endif
    /* An external table hands its partitions to another pipeline. Keeping
     * completed blocks would pin memory needed by its Sources and consumers.
     * When the estimate fits, retain blocks only beyond their reserve. */
    if(!external)
        radix->resident_reserve=reserve+tails+(size_t)workers*2*QS_SORT_IO_BYTES;
    radix->streams=cs_work_calloc(s->context,(size_t)workers*partitions,sizeof(*radix->streams));
    o->group_locals=cs_work_calloc(s->context,workers,sizeof(*o->group_locals));
    if(!radix->streams || !o->group_locals) return -4;
    for(unsigned i=0;i<workers*partitions;++i) radix->streams[i].block_bytes=block_bytes;
    if(s->profiling)
        fprintf(stderr,"CSD aggregate radix node=%u writers=%u sources=%u partitions=%u block=%u capacity=%u stride=%u\n",
            id,workers,sources,partitions,block_bytes,capacity,o->group_stride);
    cs_work_free(s->context,o->groups); cs_work_free(s->context,o->group_entries);
    o->groups=NULL; o->group_entries=NULL; o->group_local_count=workers; o->group_radix=radix;
    for(unsigned w=0;w<workers;++w) {
        struct operator_state *local=cs_work_calloc(s->context,1,sizeof(*local));
        if(!local) return -4;
        o->group_locals[w]=local;
        local->group_capacity=capacity; local->group_stride=o->group_stride; local->group_integer=o->group_integer;
        local->group_count=&local->hash_rows; atomic_init(local->group_count,0);
        local->groups=cs_work_alloc(s->context,(size_t)(capacity-capacity/8)*o->group_stride);
        local->group_entries=cs_work_calloc(s->context,capacity,sizeof(*local->group_entries));
        if(!local->groups || !local->group_entries) return -4;
        local->group_radix=radix;
        s->workers[w]->nodes[id].group_local=local;
    }
    return 1;
}
int parallel_group_begin(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id]; unsigned workers=s->plan->workers;
    if(s->partition_join>=0) return 0;
    int radix=parallel_radix_begin(s,id); if(radix) return radix<0?radix:0;
    if(!o->group_locals) {
        struct cs_work_context_stats m;
        if(cs_work_get_context_stats(s->context,&m)) return 0;
        size_t data=(size_t)(o->group_capacity-o->group_capacity/8)*o->group_stride;
        size_t entries=(size_t)o->group_capacity*sizeof(*o->group_entries);
        size_t bytes=workers*(data+entries+sizeof(*o)+3*128+sizeof(o)); /* allocation/alignment headroom */
        size_t available=cs_work_available(s->context),reserve=sort_spill_reserve(s);
        if(available<reserve || bytes>available-reserve) return 0;
        o->group_locals=cs_work_calloc(s->context,workers,sizeof(*o->group_locals));
        if(!o->group_locals) return 0;
        o->group_local_count=workers;
        for(unsigned w=0;w<workers;++w) {
            struct operator_state *local=cs_work_calloc(s->context,1,sizeof(*local));
            o->group_locals[w]=local;
            if(!local) goto fallback;
            local->group_capacity=o->group_capacity; local->group_stride=o->group_stride; local->group_integer=o->group_integer;
            local->group_count=&local->hash_rows; atomic_init(local->group_count,0);
            local->groups=cs_work_alloc(s->context,data);
            local->group_entries=cs_work_alloc(s->context,entries);
            if(!local->groups || !local->group_entries) goto fallback;
        }
    }
    if(o->group_local_count==workers)
        for(unsigned w=0;w<workers;++w) s->workers[w]->nodes[id].group_local=o->group_locals[w];
    return 0;
fallback:
    aggregate_drop_worker_tables(s,o);
    return 0;
}
static void parallel_group_combine(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    /* With an empty target, each hash partition has one owning CU. Lookups
     * share publication, but matching aggregate states need no update lock.
     * Keep inputs immutable so a full target can restart through normal spill. */
    s->group_shard=!atomic_load(o->group_count);
    s->group_combine=id+1; s->parallel_done=0;
    memset(s->parallel_position,0,sizeof(s->parallel_position));
    for(unsigned w=0;w<s->plan->workers;++w) {
        struct execution *e=s->workers[w];
        e->group_shard=s->group_shard; e->group_merge_full=false;
        e->group_combine=id+1; e->host_scan=false;
        e->nodes[id].group_local_slot=e->nodes[id].group_local_row=0; e->metrics=(struct qs_metrics){0};
        e->profile=(struct qs_profile){0}; e->local_profile_cpu=e->sink_ns=e->sink_rows=e->sink_returned_rows=0;
        memset(e->wait_ns,0,sizeof(e->wait_ns)); memset(e->wait_calls,0,sizeof(e->wait_calls)); e->wait_started=e->finished_ns=0;
        for(unsigned i=0;i<s->plan->count;++i) if(e->nodes[i].metrics)
            e->nodes[i].metrics(&e->nodes[i],NULL,NULL);
        *e->control=(struct qp_control){.state=(uintptr_t)e};
        memset(&s->outputs[w+1],0,sizeof(s->outputs[w+1]));
    }
    s->control->parallel=2; s->control->scan_node=0; s->control->available=s->control->drained=0;
    ++s->control->epoch;
    if(s->profiling) { s->pipeline_start=now_ns(CLOCK_MONOTONIC); s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID); }
}
static void aggregate_drop_worker_tables(struct execution *s,struct operator_state *o)
{
    for(unsigned w=0;w<o->group_local_count;++w) group_drop_local(s,o->group_locals[w]);
    cs_work_free(s->context,o->group_locals); o->group_locals=NULL; o->group_local_count=0;
}

int64_t aggregate_accept_result(struct execution *s,struct qs_result *out)
{
    return s->group_sink?group_sink_batch(s,out):aggregate_sink_batch(s,out);
}


int finish_group_workers(struct execution *s,unsigned id,unsigned root)
{
    const struct qp_plan *p=s->plan;
    if(s->profiling && !s->group_combine) {
        char counts[QS_WORKERS*22+1];
        size_t at=0;
        for(unsigned w=0;w<p->workers;++w)
            at+=snprintf(counts+at,sizeof(counts)-at,"%s%llu",w?",":"",
                (unsigned long long)s->outputs[w+1].metrics.scanned);
        fprintf(stderr,"CSD aggregate sink node=%u pipeline=%u scan_rows_by_slot=%s\n",id,root,counts);
    }
    if(!s->group_combine && s->nodes[id].group_locals && !s->nodes[id].group_radix) {
        parallel_group_combine(s,id); return QP_WAIT;
    }
    if(s->group_combine && s->group_shard) {
        struct operator_state *o=&s->nodes[s->group_combine-1]; bool full=false;
        for(unsigned w=0;w<p->workers;++w) full|=s->workers[w]->group_merge_full;
        if(full) {
            memset(o->group_entries,0,(size_t)o->group_capacity*sizeof(*o->group_entries));
            atomic_store(o->group_count,0);
        } else for(unsigned w=0;w<o->group_local_count;++w)
            o->group_locals[w]->group_flush=atomic_load(o->group_locals[w]->group_count);
    }
#ifndef QP_SPLIT_RUNTIME
    struct operator_state *o=&s->nodes[id];
    if(o->group_radix) for(unsigned w=o->group_radix->sources;w<o->group_local_count;++w) {
        group_drop_local(s,o->group_locals[w]);
        o->group_locals[w]=NULL;
    }
#endif
    s->group_combine=0;
    s->group_shard=false;
    return 0;
}

static void release_aggregate_source(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    cs_work_free(s->context,o->groups); o->groups=NULL;
    cs_work_free(s->context,o->group_entries); o->group_entries=NULL;
    if(o->group_radix) aggregate_drop_worker_tables(s,o);
    o->source_workspace=0;
    o->source_workers=0;
    o->start_source=NULL;
    o->finish_source=NULL;
    o->finished=1;
}

static int check_group_workers(struct execution *s,unsigned id,unsigned root,int source)
{
    (void)root;
    (void)source;
    const struct operator_state *o=&s->nodes[id];
    return o->groups && !o->group_phase?0:-1;
}

static int start_group_workers(struct execution *s,unsigned id,unsigned root)
{
    (void)root;
    return parallel_group_begin(s,id);
}

static int64_t merge_scalar_worker(struct execution *s,unsigned id,struct execution *worker)
{
    return aggregate_combine(s,id,&worker->nodes[id]);
}

int64_t aggregate_wave_next(struct execution *s,unsigned root,struct qs_cell *row,unsigned id)
{
    struct graph_wave_sink sink={.node=id};
    if(s->plan->nodes[id].nkeys) {
        sink.group=id+1;
        if(s->nodes[id].group_capacity) sink.reserve_bytes=sort_spill_reserve(s);
        sink.prepare=check_group_workers;
        sink.start=start_group_workers;
        sink.finish=finish_group_workers;
    } else {
        sink.aggregate=id+1;
        sink.merge=merge_scalar_worker;
    }
    return parallel_wave_next(s,root,row,&sink);
}
