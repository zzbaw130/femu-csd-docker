#include "graph_state.h"
#include "graph_batch.h"
#include <stdio.h>

static int64_t external_build(struct execution *s,unsigned id);

static int hash_exists_only(const struct qp_node *n)
{ return (n->join_type==QP_SEMI || n->join_type==QP_ANTI) && n->join_qual<0 && n->nkeys==1; }

static uint64_t hash_packed(const struct hash_key_layout *layout,const unsigned char *row,int *null)
{
    uint64_t hash=UINT64_C(0x9e3779b97f4a7c15); *null=0;
    for(unsigned k=0;k<layout->count;++k) {
        const unsigned char *from=row+layout->offsets[k]; unsigned bytes=layout->widths[k];
        struct qs_cell value={0};
        if(bytes==4) { int32_t v; memcpy(&v,from,4); value.value=v; }
        else if(bytes==8) { int64_t v; memcpy(&v,from,8); value.value=v; }
        else { memcpy(&value.value,from,16); value.scale=from[16]; }
        value.isnull=from[bytes+(bytes==16?2:0)]; *null|=value.isnull;
        hash=hash_key(hash,value);
    }
    return hash;
}

static void hash_link(const struct qp_node *n,struct operator_state *o,
    const struct qs_plan *right,const struct qs_cell *row,unsigned at)
{
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*hash_row_bytes(right));
    uint32_t *buckets=next+o->hash_capacity;
    unsigned bucket=row_hash(n,row,1)&(o->hash_buckets-1);
    next[at]=__atomic_exchange_n(&buckets[bucket],at,__ATOMIC_RELAXED);
}

static void *hash_memory(struct execution *s,unsigned id,void *old,size_t bytes)
{
    uint64_t started=s->profiling && !s->worker?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
    uintptr_t previous=(uintptr_t)old;
    void *data=old?cs_work_realloc(s->context,old,bytes):cs_work_alloc(s->context,bytes);
    if(started) {
        struct qp_pipeline_profile *p=&s->plan->pipelines[id];
        p->hash_memory_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-started;
        if(previous && data && (uintptr_t)data!=previous) {
            const struct qp_node *n=&s->plan->nodes[id];
            p->hash_moved_bytes+=(size_t)s->nodes[id].hash_capacity*
                (hash_row_bytes(&s->plan->nodes[n->right].expr)+3*sizeof(uint32_t));
        }
    }
    return data;
}

static void hash_reindex(struct execution *s,unsigned id,unsigned buckets_count)
{
    uint64_t started=s->profiling && !s->worker?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    unsigned rb=hash_row_bytes(right);
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*rb);
    o->hash_buckets=buckets_count;
    memset(next+o->hash_capacity,0xff,(size_t)o->hash_buckets*sizeof(uint32_t));
    struct hash_key_layout keys; hash_key_layout(&keys,n,right,1);
    uint32_t *buckets=next+o->hash_capacity;
    /* Initial setup and growth run before CUs start or after their wave
     * barrier. Only this owner writes buckets here; concurrent Sink inserts
     * retain hash_link()'s atomic publication. Decode keys, not payloads. */
    for(unsigned at=0;at<o->hash_rows;++at) {
        int null; uint64_t hash=hash_packed(&keys,o->hash_data+(size_t)at*rb,&null);
        unsigned bucket=hash&(o->hash_buckets-1);
        next[at]=buckets[bucket]; buckets[bucket]=at;
    }
    if(started) {
        struct qp_pipeline_profile *p=&s->plan->pipelines[id];
        p->hash_reindex_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-started;
        ++p->hash_reindexes; p->hash_reindexed_rows+=o->hash_rows;
    }
}

static unsigned hash_reserve_rows(struct operator_state *o,unsigned rows,unsigned *at)
{
    *at=atomic_load_explicit(&o->hash_rows,memory_order_relaxed);
    unsigned count;
    do {
        count=o->hash_capacity-1-*at;
        if(count>rows) count=rows;
        if(!count) return 0;
    } while(!atomic_compare_exchange_weak_explicit(&o->hash_rows,at,*at+count,
        memory_order_relaxed,memory_order_relaxed));
    return count;
}

static int hash_insert_exists(struct execution *s,const struct qp_node *n,struct operator_state *o,
    const struct qs_plan *right,const struct qs_cell *row)
{
    unsigned column=n->right_keys[0];
    uint64_t *seen=NULL,bit=0;
    uint64_t tag=0; unsigned cached=0;
    if(o->hash_cache && !row[column].isnull && !row[column].scale &&
       row[column].value>=0 && row[column].value<qp_hash_bitmap_keys(o->hash_cache_slots)) {
        uint64_t key=(uint64_t)row[column].value;
        seen=o->hash_cache+o->hash_cache_slots+key/64;
        bit=UINT64_C(1)<<(key%64);
        if(__atomic_load_n(seen,__ATOMIC_RELAXED)&bit) return 1;
    }
    uint64_t hash=row_hash(n,row,1);
    if(o->hash_cache && !seen && !row[column].isnull && !row[column].scale &&
       row[column].value>=INT64_MIN && row[column].value<=INT64_MAX) {
        tag=(uint64_t)(int64_t)row[column].value+1;
        cached=(hash>>32)&(o->hash_cache_slots-1);
        if(tag && __atomic_load_n(&o->hash_cache[cached],__ATOMIC_ACQUIRE)==tag) return 1;
    }
    unsigned bytes=hash_row_bytes(right),type=right->exprs[right->outputs[column].index].type;
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*bytes),*buckets=next+o->hash_capacity;
    unsigned bucket=hash&(o->hash_buckets-1);
    for(unsigned match=__atomic_load_n(&buckets[bucket],__ATOMIC_ACQUIRE);match!=UINT_MAX;match=next[match]) {
        struct qs_cell other[QS_OUT]; hash_load(other,o->hash_data+(size_t)match*bytes,right);
        int equal=equal_cell(s,row[column],other[column],type);
        if(equal<0) return -5;
        if(equal) {
            if(seen) __atomic_fetch_or(seen,bit,__ATOMIC_RELAXED);
            if(tag) __atomic_store_n(&o->hash_cache[cached],tag,__ATOMIC_RELEASE);
            return 1;
        }
    }
    unsigned at; if(!hash_reserve_rows(o,1,&at)) return 0;
    hash_store(o->hash_data+(size_t)at*bytes,row,right);
    unsigned head=__atomic_load_n(&buckets[bucket],__ATOMIC_ACQUIRE);
    do { next[at]=head; }
    while(!__atomic_compare_exchange_n(&buckets[bucket],&head,at,1,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));
    if(seen) __atomic_fetch_or(seen,bit,__ATOMIC_RELAXED);
    if(tag) __atomic_store_n(&o->hash_cache[cached],tag,__ATOMIC_RELEASE);
    return 1;
}

static int hash_append_batch(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *rows,unsigned length)
{
    const struct qp_node *n=&s->plan->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    if(!o->hash_data || o->hash_fallback) return 0;
    unsigned at,count=0;
    /* Without a key cache, a fully reserved build uses batch linking to avoid
     * per-row atomic contention. Elide duplicates in CUs when the
     * estimated build exceeds its current capacity. Growth retains fallback
     * compaction if the row estimate was too small. */
    if(hash_exists_only(n) && (o->hash_cache || s->plan->nodes[n->right].estimated_rows>=o->hash_capacity)) {
        for(;count<length;++count) {
            const struct qs_cell *row=rows+(size_t)count*right->nout;
            int added=hash_insert_exists(s,n,o,right,row);
            if(added<0) return added;
            if(!added) break;
        }
    } else {
        count=hash_reserve_rows(o,length,&at);
        unsigned rb=hash_row_bytes(right);
        for(unsigned i=0;i<count;++i) {
            const struct qs_cell *row=rows+(size_t)i*right->nout;
            hash_store(o->hash_data+(size_t)(at+i)*rb,row,right);
            if(!o->hash_defer_index) hash_link(n,o,right,row,at+i);
        }
    }
    s->metrics.build_rows+=count;
    return count;
}

int hash_sink_batch(struct execution *s,struct qs_result *out)
{
    unsigned id=s->hash_sink-1,child=s->plan->nodes[id].right;
    unsigned cols=s->plan->nodes[child].expr.nout;
    int count=hash_append_batch(s,id,&s->owner->nodes[id],(struct qs_cell *)out->data,out->rows);
    if(count<0) return count;
    out->rows-=count;
    memmove(out->data,(struct qs_cell *)out->data+(size_t)count*cols,
        (size_t)out->rows*cols*sizeof(struct qs_cell));
    return 0;
}

static int hash_compact_exists(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id];
    if(!hash_exists_only(n)) return 0;
    struct operator_state *o=&s->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    unsigned bytes=hash_row_bytes(right),column=n->right_keys[0];
    unsigned type=right->exprs[right->outputs[column].index].type;
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*bytes),*buckets=next+o->hash_capacity;
    memset(buckets,0xff,(size_t)o->hash_buckets*sizeof(*buckets));
    unsigned rows=o->hash_rows,kept=0;
    for(unsigned at=0;at<rows;++at) {
        struct qs_cell row[QS_OUT],other[QS_OUT];
        hash_load(row,o->hash_data+(size_t)at*bytes,right);
        unsigned bucket=row_hash(n,row,1)&(o->hash_buckets-1),found=0;
        for(unsigned match=buckets[bucket];match!=UINT_MAX;match=next[match]) {
            hash_load(other,o->hash_data+(size_t)match*bytes,right);
            int equal=equal_cell(s,row[column],other[column],type);
            if(equal<0) return -5;
            if(equal) { found=1; break; }
        }
        if(found) continue;
        if(kept!=at) memmove(o->hash_data+(size_t)kept*bytes,o->hash_data+(size_t)at*bytes,bytes);
        next[kept]=buckets[bucket]; buckets[bucket]=kept++;
    }
    o->hash_rows=kept;
    return kept<rows;
}

#ifdef QP_SPLIT_RUNTIME
static size_t build_wave_workspace(const struct execution *s,int root)
{
    const struct qp_plan *p=s->plan;
    int source=qp_pipeline_source(p,root);
    if(source<0) return 0;
    size_t worker=sizeof(*s)+p->count*sizeof(*s->nodes)+sizeof(struct qs_plan)+2*QS_SORT_IO_BYTES;
    size_t vectors=0;
    for(int id=root;id>=0;id=p->nodes[id].left) {
        const struct qs_plan *expr=&p->nodes[id].expr;
        vectors+=sizeof(struct projection_batch)+expr->nexprs*sizeof(struct qs_vector)+
            (size_t)expr->nattrs*QS_VECTOR_ROWS*sizeof(struct qs_cell);
        if(id==source) break;
    }
    const struct operator_state *input=&s->nodes[source];
    unsigned active=input->source_workers?input->source_workers:p->workers;
    return p->workers*worker+active*vectors+input->source_workspace;
}

/* The reloaded table remains resident throughout its probe pipeline.
 * Charge all private worker state and the downstream spill workspace
 * before choosing a direct reload. */
static size_t probe_wave_workspace(const struct execution *s,unsigned join)
{
    const struct qp_plan *p=s->plan;
    unsigned root=join;
    for(;;) {
        unsigned parent=0;
        for(;parent<p->count;++parent) {
            const struct qp_node *n=&p->nodes[parent];
            if(n->left==(int)root && (n->kind==QP_PROJECT ||
               (n->kind==QP_JOIN && n->hash) || n->kind==QP_INDEX_JOIN))
                break;
        }
        if(parent==p->count) break;
        root=parent;
    }
    return build_wave_workspace(s,root)+sort_spill_reserve(s);
}
#endif

static int64_t hash_start_spill(struct execution *s,unsigned id,unsigned row_bytes)
{
    struct operator_state *o=&s->nodes[id];
    cs_work_free(s->context,o->hash_cache); o->hash_cache=NULL;
    size_t used=(size_t)o->hash_rows*row_bytes;
    void *prefix=used?cs_work_realloc(s->context,o->hash_data,used):NULL;
    unsigned char *retained=prefix?prefix:o->hash_data;
    o->hash_defer_index=false;
    if(s->plan->nodes[id].join_type==QP_INNER)
        o->external=cs_work_calloc(s->context,1,sizeof(*o->external));
    if(o->external) {
        o->external->prefix=retained; o->external->prefix_rows=o->hash_rows;
        o->hash_data=NULL; o->hash_capacity=o->hash_rows=0; o->hash_fallback=2;
        return external_build(s,id);
    }
    o->right_sort.prefix=retained; o->right_sort.prefix_rows=o->hash_rows;
    o->hash_data=NULL; o->hash_fallback=1; return 0;
}

static int64_t hash_build_impl(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    unsigned row_bytes=hash_row_bytes(right);
    if(o->external) return external_build(s,id);
    if(o->hash_fallback) return 0;
#ifdef QP_SPLIT_RUNTIME
    if(!o->hash_capacity && s->hash_input && n->join_type==QP_INNER) {
        int source=qp_pipeline_source(s->plan,n->right);
        o->hash_defer_index=source>=0 && s->nodes[source].start_source;
    }
#endif
    size_t stride=row_bytes+(o->hash_defer_index?0:3*sizeof(uint32_t));
    /* Hash build is not a sort run: allow it to use the query budget while
     * retaining two sort quotas for a Merge fallback. The parent allocator
     * still enforces actual usage across all active nodes and workers. */
    struct cs_work_context_stats memory;
    if(cs_work_get_context_stats(s->context,&memory)) return -4;
    size_t hash_limit=memory.limit_bytes-2*s->sort_limit;
    if(!o->hash_capacity) {
#ifdef QP_SPLIT_RUNTIME
        if(s->hash_input) {
            size_t available=cs_work_available(s->context);
            size_t workspace=build_wave_workspace(s,n->right);
            available=available>workspace?available-workspace:0;
            if(available<hash_limit) hash_limit=available;
        }
#endif
        uint64_t estimate=s->plan->nodes[n->right].estimated_rows;
        /* Blocking children need their working space before producing rows. */
        if(s->plan->nodes[n->right].kind!=QP_SCAN && !s->hash_input) estimate=128;
        unsigned max=1; while((size_t)max*2*stride<=hash_limit && max<UINT_MAX/4) max*=2;
        unsigned capacity=128; while(capacity<estimate && capacity<max) capacity*=2;
        if(capacity>max) capacity=max;
        size_t bytes=capacity*stride;
        /* A filtered parallel build can outgrow PG's estimate after worker
         * allocations occupy the adjacent span. For residual SEMI/ANTI, a
         * failed growth sorts both inputs. Reserve the next tier up front
         * when it fits the hash budget but allocating it beside the old
         * table would exceed that budget. */
        if(capacity<max && s->plan->nodes[n->right].kind==QP_SCAN &&
           s->plan->nodes[n->right].parallel && right->predicate>=0 &&
           (n->join_type==QP_SEMI || n->join_type==QP_ANTI) && n->join_qual>=0 &&
           bytes>hash_limit/3) {
            o->hash_data=hash_memory(s,id,NULL,2*bytes);
            if(o->hash_data) capacity*=2;
        }
        if(!o->hash_data) o->hash_data=hash_memory(s,id,NULL,bytes);
        while(!o->hash_data && s->hash_input && capacity>128) {
            capacity/=2;
            o->hash_data=hash_memory(s,id,NULL,(size_t)capacity*stride);
        }
        if(!o->hash_data) { o->hash_fallback=1; return 0; }
        o->hash_capacity=capacity;
        if(!o->hash_defer_index) hash_reindex(s,id,2*capacity);
    }
    while(!o->hash_phase) {
        if(o->hash_rows==o->hash_capacity) {
            size_t bytes=(size_t)o->hash_capacity*2*stride;
            void *data=bytes<=hash_limit?hash_memory(s,id,o->hash_data,bytes):NULL;
            if(!data) {
                int compacted=hash_compact_exists(s,id);
                if(compacted<0) return compacted;
                if(compacted) continue;
                /* Reuse the packed prefix in the spill path. */
                return hash_start_spill(s,id,row_bytes);
            }
            o->hash_data=data; o->hash_capacity*=2;
            if(!o->hash_defer_index) hash_reindex(s,id,2*o->hash_capacity);
        }
        if(s->hash_input && s->hash_input_node==(unsigned)n->right &&
           s->hash_input_position<s->hash_input->rows) {
            unsigned at=s->hash_input->begin+s->hash_input_position;
            const struct qs_cell *rows=(const struct qs_cell *)s->hash_input->data+(size_t)at*right->nout;
            int count=hash_append_batch(s,id,o,rows,s->hash_input->rows-s->hash_input_position);
            if(count<0) return count;
            if(count) { s->hash_input_position+=count; continue; }
        }
        int64_t status=hash_build_input(s,n->right,o->right);
        if(status==QP_WAIT || status<0 || status>2) return status;
        if(status==2) {
            unsigned at=o->hash_rows++;
            hash_store(o->hash_data+(size_t)at*row_bytes,o->right,right);
            if(!o->hash_defer_index) hash_link(n,o,right,o->right,at);
            ++s->metrics.build_rows; continue;
        }
        /* Source and its worker tables are gone. Build the index once from
         * the real row count instead of reserving buckets during production. */
        if(o->hash_defer_index) {
            unsigned capacity=(o->hash_rows+127)&~127U,buckets=256;
            if(capacity<128) capacity=128;
            while(buckets<capacity*2) buckets*=2;
            size_t bytes=(size_t)capacity*(row_bytes+sizeof(uint32_t))+(size_t)buckets*sizeof(uint32_t);
            void *data=hash_memory(s,id,o->hash_data,bytes);
            if(!data) return hash_start_spill(s,id,row_bytes);
            o->hash_data=data; o->hash_capacity=capacity; o->hash_defer_index=false;
            hash_reindex(s,id,buckets);
            if(s->profiling)
                fprintf(stderr,"CSD hash deferred index node=%u rows=%u bytes=%zu\n",id,o->hash_rows,bytes);
        }
        o->hash_phase=1;
        cs_work_free(s->context,o->hash_cache); o->hash_cache=NULL;
    }
    return 0;
}

int64_t hash_build(struct execution *s,unsigned id)
{
    if(!s->profiling || s->nodes[id].hash_phase) return hash_build_impl(s,id);
    struct blocking_timer timer=blocking_begin(s);
    int64_t status=hash_build_impl(s,id); blocking_end(s,QS_JOIN_BUILD,timer); return status;
}

static int external_reload_alloc(cs_work_context *ctx,struct operator_state *o,
    unsigned capacity,unsigned buckets,unsigned row_bytes)
{
    size_t index_bytes=((size_t)capacity+buckets)*sizeof(uint32_t);
    o->hash_data=cs_work_alloc(ctx,(size_t)capacity*row_bytes+index_bytes);
    if(o->hash_data) {
        o->hash_capacity=capacity; o->hash_buckets=buckets;
        return 1;
    }
    /* Keep a single index but let the packed rows occupy smaller free spans. */
    unsigned count=(capacity+HASH_RELOAD_BLOCK_ROWS-1)/HASH_RELOAD_BLOCK_ROWS;
    o->hash_data=cs_work_alloc(ctx,index_bytes);
    if(!o->hash_data) return 0;
    o->hash_blocks=cs_work_alloc(ctx,(size_t)count*sizeof(*o->hash_blocks));
    if(!o->hash_blocks) { hash_release(ctx,o); return 0; }
    o->hash_capacity=0;
    while(o->hash_capacity<capacity) {
        unsigned rows=capacity-o->hash_capacity;
        if(rows>HASH_RELOAD_BLOCK_ROWS) rows=HASH_RELOAD_BLOCK_ROWS;
        unsigned char *block=cs_work_alloc(ctx,(size_t)rows*row_bytes);
        if(!block) {
            hash_release(ctx,o); o->hash_capacity=0;
            return 0;
        }
        o->hash_blocks[o->hash_capacity/HASH_RELOAD_BLOCK_ROWS]=block;
        o->hash_capacity+=rows;
    }
    o->hash_buckets=buckets;
    return 1;
}

static void partition_start(struct execution *s,unsigned id)
{
    if(s->worker || !s->parallel_started || s->control->parallel!=2 ||
       s->parallel_node!=(unsigned)s->plan->nodes[id].right) return;
    unsigned workers=s->plan->workers;
    struct external_hash *external=s->nodes[id].external;
    if(s->workers[0]->partition_sink || external->partition_attempted) return;
    external->partition_attempted=1;
    int active=0;
    for(unsigned w=0;w<workers;++w) active|=!s->workers[w]->control->finished;
    if(!active) return;
    size_t needed=(size_t)workers*HASH_PARTITIONS*(QS_SORT_IO_BYTES+sizeof(struct spool));
    if(needed>cs_work_available(s->context)) return;
    for(unsigned w=0;w<workers;++w) {
        struct execution *e=s->workers[w];
        if(e->hash_sink!=id+1 || (!e->control->finished &&
           !(s->plan->nodes[e->root].kind==QP_AGGREGATE?e->nodes[e->root].source_projection:e->nodes[e->root].projection))) return;
    }
    for(unsigned w=0;w<workers;++w) {
        struct execution *e=s->workers[w];
        e->partitions=cs_work_calloc(e->context,HASH_PARTITIONS,sizeof(*e->partitions));
        if(!e->partitions) {
            for(unsigned j=0;j<w;++j) { cs_work_free(s->workers[j]->context,s->workers[j]->partitions); s->workers[j]->partitions=NULL; }
            return;
        }
    }
    atomic_init(&external->prefix_next,external->prefix_at); external->prefix_parallel=1;
    for(unsigned w=0;w<workers;++w) { s->workers[w]->hash_sink=0; s->workers[w]->partition_sink=id+1; }
}

static int64_t merge_partition_worker(struct execution *s,unsigned id,struct execution *worker)
{
    (void)id;
    return worker->partition_sink?external_partition_combine(s,worker):0;
}

static int64_t external_build(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    struct external_hash *e=o->external;
    if(e->phase>=3) return 0;
    const struct qs_plan *right=&s->plan->nodes[n->right].expr,*left=&s->plan->nodes[n->left].expr;
    unsigned rb=hash_row_bytes(right),lb=hash_row_bytes(left);
    unsigned char packed[QS_OUT*19];
    struct hash_key_layout keys; hash_key_layout(&keys,n,right,1);
    while(e->phase<2) {
        int build=e->phase==0;
        /* A sequential probe input can use the same CU-local partition
         * sink. Its ordinary scan buffers are already in this pipeline's
         * layout; do not serialize the whole input through the owner. */
        if(!build && !s->hash_input && s->plan->workers>1 && s->plan->nodes[n->left].kind==QP_SCAN &&
           !s->plan->nodes[n->left].ordered_index) {
            if(!e->probe_parallel) {
                e->probe_parallel_saved=s->plan->nodes[n->left].parallel;
                s->plan->nodes[n->left].parallel=1; e->probe_parallel=1;
            }
#ifdef QP_SPLIT_RUNTIME
            const struct graph_wave_sink sink={.node=id,.partition=id+1,.merge=merge_partition_worker};
            int64_t status=parallel_wave_next(s,n->left,NULL,&sink);
#else
            int64_t status=parallel_next(s,n->left,NULL);
#endif
            if(status) return status;
            s->plan->nodes[n->left].parallel=e->probe_parallel_saved;
            e->probe_parallel=0; ++e->phase; continue;
        }
        if(build && !s->hash_input) partition_start(s,id);
        if(build && !e->prefix_parallel && !o->have_right && e->prefix_at<e->prefix_rows) {
            const unsigned char *row=e->prefix+(size_t)e->prefix_at*rb;
            int null; uint64_t hash=hash_packed(&keys,row,&null);
            if(!null) {
                int64_t status=spool_append_bytes(s,&e->build[(hash>>32)&(HASH_PARTITIONS-1)],row,rb);
                if(status) return status;
            }
            ++e->prefix_at; continue;
        }
        if(!o->have_right) {
            int64_t status;
            if(e->prefix && !e->prefix_parallel) { cs_work_free(s->context,e->prefix); e->prefix=NULL; }
            if(build && !s->hash_input) partition_start(s,id);
            status=hash_build_input(s,build?n->right:n->left,o->right);
            if(status==QP_WAIT || status<0 || status>2) return status;
            if(!status) {
                ++e->phase;
                if(build) {
                    if(e->prefix_parallel) {
                        cs_work_free(s->context,e->prefix); e->prefix=NULL;
                        e->prefix_at=e->prefix_rows; e->prefix_parallel=0;
                    }
                    /* The blocking child has now released its table. If all
                     * partitions fit, load them and Probe the original
                     * input in parallel without partitioning it first. Only
                     * the bucket count needs a power-of-two capacity. */
                    uint64_t rows=0;
                    for(unsigned i=0;i<HASH_PARTITIONS;++i) rows+=e->build[i].rows;
                    if(!rows && !s->hash_input) { e->phase=4; o->finished=1; return 0; }
                    if(rows<=UINT_MAX/4) {
                        unsigned capacity=rows?(rows+127)&~127U:128,buckets=256;
                        while(buckets<capacity*2) buckets*=2;
                        size_t bytes=(size_t)capacity*(rb+4)+(size_t)buckets*4;
                        size_t reserve=2*QS_SORT_IO_BYTES;
                        if(s->plan->workers>1) reserve+=s->plan->workers*(sizeof(*s)+s->plan->count*sizeof(*s->nodes)+QS_SORT_IO_BYTES+256);
#ifdef QP_SPLIT_RUNTIME
                        size_t probe_workspace=probe_wave_workspace(s,id);
                        if(probe_workspace>reserve) reserve=probe_workspace;
#endif
                        size_t available=cs_work_available(s->context);
#ifdef QP_SPLIT_RUNTIME
                        /* A denser chain index may fit while leaving the same
                         * probe reserve. Its bucket count still covers every row. */
                        if(available>reserve && bytes>available-reserve && buckets>256) {
                            buckets/=2;
                            bytes-=(size_t)buckets*sizeof(uint32_t);
                        }
#endif
                        if(available>reserve && bytes<=available-reserve &&
                           external_reload_alloc(s->context,o,capacity,buckets,rb)) {
                            e->direct=1; e->phase=2;
                        }
                        if(s->profiling) {
                            struct cs_work_stats pool={0};
                            cs_work_get_stats(&pool);
                            fprintf(stderr,"CSD_HASH_RELOAD node=%u rows=%llu width=%u bytes=%zu available=%zu reserve=%zu pool_free=%zu largest=%zu direct=%u segmented=%u\n",
                                id,(unsigned long long)rows,rb,bytes,available,reserve,
                                pool.free_bytes,pool.largest_free_payload,(unsigned)e->direct,o->hash_blocks!=NULL);
                        }
                    }
                    /* The right-side graph ended. The caller must wire the
                     * left stream before phase 1 can consume any input. */
                    if(s->hash_input && !e->direct) return rows?QP_WAIT:-4;
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
            /* Read caches are local to the consumer. Partition tails are
             * owned by the query that combined the partition writers. */
            cs_work_context *owner=s->owner?s->owner->context:s->context;
            if(build->cache) { cs_work_free(s->context,build->cache); build->cache=NULL; }
            if(build->tail) { cs_work_free(owner,build->tail); build->tail=NULL; }
            if(probe->cache) { cs_work_free(s->context,probe->cache); probe->cache=NULL; }
            if(probe->tail) { cs_work_free(owner,probe->tail); probe->tail=NULL; }
            ++e->partition; e->build_at=e->probe_at=0; continue;
        }
        if(!o->hash_data) {
            size_t reserve=2*QS_SORT_IO_BYTES;
            if(s->plan->workers>1) reserve+=s->plan->workers*(sizeof(*s)+s->plan->count*sizeof(*s->nodes)+QS_SORT_IO_BYTES+256);
            size_t available=cs_work_available(s->context);
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
            int64_t status=spool_row_bytes(s,build,e->build_at,rb,hash_row(o,o->hash_rows,rb));
            if(status) return status;
            ++e->build_at; ++o->hash_rows;
        }
        if(!e->direct) { o->hash_buckets=2*o->hash_capacity; break; }
    }
    uint32_t *next=hash_chain(o,rb),*buckets=next+o->hash_capacity;
    memset(buckets,0xff,(size_t)o->hash_buckets*sizeof(*buckets));
    for(unsigned i=0;i<o->hash_rows;++i) {
        int null;
        unsigned bucket=hash_packed(&keys,hash_row(o,i,rb),&null)&(o->hash_buckets-1);
        next[i]=buckets[bucket]; buckets[bucket]=i;
    }
    e->phase=3; o->hash_phase=1;
    return 0;
}

int hash_sink_vectors(struct execution *s,unsigned id)
{
    struct projection_batch *b=s->plan->nodes[id].kind==QP_AGGREGATE?
        s->nodes[id].source_projection:s->nodes[id].projection;
    const struct qp_node *n=&s->plan->nodes[s->hash_sink-1];
    struct operator_state *o=&s->owner->nodes[s->hash_sink-1];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    if(!o->hash_data || o->hash_fallback) return 0;
    if(s->paged && materialize_outputs(s,b)) return -3;
    if(hash_exists_only(n) && (o->hash_cache || s->plan->nodes[n->right].estimated_rows>=o->hash_capacity)) {
        unsigned first=b->position;
        while(b->position<b->count) {
            struct qs_cell row[QS_OUT];
            for(unsigned col=0;col<right->nout;++col) {
                int index=b->plan.outputs[col].index;
                if(b->vectors[index].errors[b->position]) return -5;
                row[col]=qs_vector_cell(&b->plan,b->vectors,index,b->position);
            }
            int added=hash_insert_exists(s,n,o,right,row);
            if(added<0) return added;
            if(!added) break;
            ++b->position;
        }
        s->metrics.matched+=b->position-first; s->metrics.build_rows+=b->position-first;
        return 0;
    }
    unsigned at,count=hash_reserve_rows(o,b->count-b->position,&at);
    if(!count) return 0;
    uint64_t hashes[QS_VECTOR_ROWS];
    if(!o->hash_defer_index) {
        for(unsigned r=0;r<count;++r) hashes[r]=UINT64_C(0x9e3779b97f4a7c15);
        for(unsigned k=0;k<n->nkeys;++k) {
            int index=b->plan.outputs[n->right_keys[k]].index;
            for(unsigned r=0;r<count;++r) {
                unsigned row=b->position+r;
                if(b->vectors[index].errors[row]) return -5;
                hashes[r]=hash_key(hashes[r],qs_vector_cell(&b->plan,b->vectors,index,row));
            }
        }
    }
    unsigned stride=hash_row_bytes(right),offset=0;
    for(unsigned col=0;col<right->nout;++col) {
        int index=b->plan.outputs[col].index;
        unsigned bytes=hash_value_bytes(right,col);
        for(unsigned r=0;r<count;++r) {
            unsigned row=b->position+r;
            if(b->vectors[index].errors[row]) return -5;
            struct qs_cell value=qs_vector_cell(&b->plan,b->vectors,index,row);
            hash_store_value(o->hash_data+(size_t)(at+r)*stride+offset,&value,bytes);
        }
        offset+=bytes+(bytes<16?1:3);
    }
    if(!o->hash_defer_index) {
        uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*stride);
        uint32_t *buckets=next+o->hash_capacity;
        for(unsigned r=0;r<count;++r) {
            unsigned bucket=hashes[r]&(o->hash_buckets-1);
            next[at+r]=__atomic_exchange_n(&buckets[bucket],at+r,__ATOMIC_RELAXED);
        }
    }
    b->position+=count; s->metrics.matched+=count; s->metrics.build_rows+=count;
    return 0;
}

int64_t partition_sink_prefix(struct execution *s)
{
    unsigned id=s->partition_sink-1;
    const struct qp_node *n=&s->plan->nodes[id];
    struct external_hash *e=s->owner->nodes[id].external;
    if(!e->prefix_parallel) return 0;
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    unsigned bytes=hash_row_bytes(right); struct hash_key_layout keys;
    hash_key_layout(&keys,n,right,1);
    for(;;) {
        if(s->partition_prefix_at>=s->partition_prefix_end) {
            s->partition_prefix_at=atomic_fetch_add_explicit(&e->prefix_next,64,memory_order_relaxed);
            if(s->partition_prefix_at>=e->prefix_rows) return 0;
            s->partition_prefix_end=s->partition_prefix_at+64;
            if(s->partition_prefix_end>e->prefix_rows) s->partition_prefix_end=e->prefix_rows;
        }
        const unsigned char *row=e->prefix+(size_t)s->partition_prefix_at*bytes;
        int null; uint64_t hash=hash_packed(&keys,row,&null);
        if(!null) {
            int64_t status=spool_append_bytes(s,&s->partitions[(hash>>32)&(HASH_PARTITIONS-1)],row,bytes);
            if(status) return status;
        }
        ++s->partition_prefix_at;
    }
}

int64_t partition_sink_vectors(struct execution *s,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[s->partition_sink-1];
    int build=!s->owner->nodes[s->partition_sink-1].external->phase;
    const struct qs_plan *right=&s->plan->nodes[build?n->right:n->left].expr;
    unsigned bytes=hash_row_bytes(right);
    if(s->paged && materialize_outputs(s,b)) return -3;
    for(;b->position<b->count;++b->position) {
        struct qs_cell row[QS_OUT]; int null=0;
        for(unsigned col=0;col<right->nout;++col) {
            int index=b->plan.outputs[col].index;
            if(b->vectors[index].errors[b->position]) return -5;
            row[col]=qs_vector_cell(&b->plan,b->vectors,index,b->position);
        }
        for(unsigned k=0;k<n->nkeys;++k) null|=row[build?n->right_keys[k]:n->left_keys[k]].isnull;
        if(!null) {
            uint64_t hash=row_hash(n,row,build); unsigned char packed[QS_OUT*19];
            hash_store(packed,row,right);
            int64_t status=spool_append_bytes(s,&s->partitions[(hash>>32)&(HASH_PARTITIONS-1)],packed,bytes);
            if(status) return status;
        }
        if(build) ++s->metrics.build_rows;
    }
    return 0;
}

int64_t external_partition_combine(struct execution *s,struct execution *worker)
{
    unsigned id=worker->partition_sink-1;
    struct external_hash *e=s->nodes[id].external;
    unsigned bytes=hash_row_bytes(&s->plan->nodes[e->phase?s->plan->nodes[id].left:s->plan->nodes[id].right].expr);
    unsigned block_rows=QS_SORT_IO_BYTES/bytes;
    for(;worker->partition_merged<HASH_PARTITIONS;++worker->partition_merged) {
        struct spool *from=&worker->partitions[worker->partition_merged],*to=e->phase?&e->probe[worker->partition_merged]:&e->build[worker->partition_merged];
        if(from->blocks) {
            unsigned count=to->blocks+from->blocks,capacity=to->capacity?to->capacity:8;
            while(capacity<count) capacity*=2;
            if(capacity>to->capacity) {
                void *pages=cs_work_realloc(s->context,to->pages,(size_t)capacity*sizeof(*to->pages));
                if(!pages) return -4;
                to->pages=pages; to->capacity=capacity;
            }
            memcpy(to->pages+to->blocks,from->pages,(size_t)from->blocks*sizeof(*to->pages));
            to->blocks+=from->blocks; to->rows+=(uint64_t)from->blocks*block_rows;
            from->blocks=0; /* Tokens now belong to the parent; never adopt twice. */
        }
        for(;worker->partition_merge_row<from->used;++worker->partition_merge_row) {
            int64_t status=spool_append_bytes(s,to,from->tail+(size_t)worker->partition_merge_row*bytes,bytes);
            if(status) return status;
        }
        worker->partition_merge_row=0;
        cs_work_free(worker->context,from->tail); from->tail=NULL; from->used=0;
        cs_work_free(worker->context,from->pages); from->pages=NULL;
    }
    return 0;
}

#ifndef QP_SPLIT_RUNTIME
int64_t pg_sina_graph_hash_build(struct femu_csd_args *a)
{
    if(!a || a->numr!=5 || a->mr_len[0]<(long long)sizeof(struct qp_plan) ||
       a->mr_len[1]<(long long)sizeof(struct qp_control) ||
       a->mr_len[2]<(long long)sizeof(struct qs_result) ||
       a->mr_len[3]<(long long)sizeof(struct qs_dict) || a->mr_len[4]<QS_SORT_IO_BYTES) return -1;
    struct qp_plan *plan=a->mr_addr[0];
    struct qp_control *control=a->mr_addr[1];
    struct execution *s=(void *)(uintptr_t)control->state;
    if(control->close) { if(s) close_execution(s); return 0; }
    uint64_t id=(uint64_t)a->cparam1>>QP_GRAPH_HASH_NODE_SHIFT;
    bool left=((uint64_t)a->cparam1&QP_GRAPH_HASH_LEFT)!=0;
    if(plan->count>QP_NODES || id>=plan->count || plan->nparams>QP_PARAMS ||
       (uint64_t)a->mr_len[0]<sizeof(*plan)+qp_host_parameter_bytes(plan)) return -1;
    const struct qp_node *n=&plan->nodes[id];
    if(n->kind!=QP_JOIN || !n->hash || n->left<0 || n->right<0 ||
       (unsigned)n->left>=plan->count || (unsigned)n->right>=plan->count) return -1;
    unsigned child=left?n->left:n->right,cols=plan->nodes[child].expr.nout;
    const struct qs_result *in=a->mr_addr[2];
    if(!cols || cols>QS_OUT || in->status || in->finished>1 ||
       (uint64_t)in->begin+in->rows>QS_RESULT_ROWS*QS_OUT/cols) return -1;
    if(!s) {
        if(left || csIsReentry(a)) return -1;
        int status=open_execution(a,plan,control,&graph_project_ops,graph_index_init,&s); if(status) return status;
    }
    if(s->plan!=plan || s->owner || s->closing || s->graph_source_pending) return -1;
    struct operator_state *o=&s->nodes[id];
    if(csIsReentry(a)) {
        if(s->hash_input_node!=child || s->hash_input_position>in->rows) return -1;
    } else {
        if(o->hash_phase || o->finished || o->hash_fallback==1 ||
           left!=(o->external && o->external->phase==1)) return -1;
        s->hash_input_position=0; control->drained=0;
    }
    s->hash_input_node=child;
    s->args=a; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
    wait_resume(s);
    int64_t status=initialize_dictionary(s,a);
    if(status==QS_DICTIONARY_BUSY) return QP_RESOURCE_WAIT;
    if(status) { wait_io(s,status); return status; }
    status=resume_spool(s,a); if(status) return status;
    s->hash_input=in;
    status=hash_build(s,id);
    s->hash_input=NULL;
    if(status<0 || status>2) { wait_io(s,status); return status; }
    unsigned flags=s->hash_input_position==in->rows?QP_INPUT_DRAINED:0;
    if(o->hash_phase || o->finished) flags|=QP_WORKER_FINISHED;
    else if(o->hash_fallback==1) flags|=QP_GRAPH_HASH_MERGE_REQUIRED;
    else if(o->external && o->external->phase==1) flags|=QP_GRAPH_HASH_NEEDS_LEFT;
    return flags;
}
#endif

int64_t block_hash_vectors(struct execution *s,unsigned id,struct projection_batch *b)
{
    int status=hash_sink_vectors(s,id); if(status) return status;
    while(b->position<b->count) {
        struct qs_cell row[QS_OUT];
        for(unsigned col=0;col<b->plan.nout;++col) {
            int index=b->plan.outputs[col].index;
            if(b->vectors[index].errors[b->position]) return -5;
            row[col]=qs_vector_cell(&b->plan,b->vectors,index,b->position);
        }
        int64_t stored=spool_append(s,&s->block_overflow,row,b->plan.nout);
        if(stored) return stored;
        ++b->position;
    }
    return 0;
}

static int64_t hash_build_batch(struct execution *s,unsigned id,struct projection_batch *b,struct qs_result *out)
{
    (void)out;
    return block_hash_vectors(s,id,b);
}

static int64_t partition_batch(struct execution *s,unsigned id,struct projection_batch *b,struct qs_result *out)
{
    (void)id;
    (void)out;
    return partition_sink_vectors(s,b);
}

static int64_t stream_partition(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    static const struct graph_sink_ops ops={.consume=partition_batch,.borrow_projection=true};
    return graph_stream_sink(s,scan,sink,out,&ops);
}

static int64_t stream_hash_build(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    if(s->partition_sink) return stream_partition(s,scan,sink,out);
    static const struct graph_sink_ops ops={.consume=hash_build_batch,.borrow_projection=true};
    return graph_stream_sink(s,scan,sink,out,&ops);
}

int64_t pg_sina_graph_block_hash_build(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_HASH,stream_hash_build); }

int64_t pg_sina_graph_block_partition(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_PARTITION,stream_partition); }

int prepare_hash_wave(struct execution *s,unsigned hash_sink,unsigned root,int source,bool explicit_sink)
{
    const struct qp_plan *p=s->plan;
    /* Build dependencies are now ready. Size the empty parent from PG's
     * output estimate, or the exact group count when preserved, instead
     * of stopping CU waves to grow the original 128-row table. DuckDB
     * sizes its pointer table from Count() after collecting build rows;
     * our streaming sink retains growth/spill for an underestimated size. */
    int counted=source;
    for(int id=root;id!=source;id=p->nodes[id].left)
        if(p->nodes[id].kind!=QP_PROJECT || p->nodes[id].expr.predicate>=0) { counted=-1; break; }
    if(s->partition_join<0) {
        struct operator_state *o=&s->nodes[hash_sink-1];
        struct cs_work_context_stats m;
        if(cs_work_get_context_stats(s->context,&m)) return -4;
        size_t reserve=2*s->sort_limit+p->workers*(sizeof(*s)+p->count*sizeof(*s->nodes)+sizeof(struct qs_plan)+256);
#ifdef QP_SPLIT_RUNTIME
        size_t workspace=build_wave_workspace(s,root);
        if(workspace>reserve) reserve=workspace;
#endif
        size_t available=cs_work_available(s->context);
        available=available>reserve?available-reserve:0;
        size_t stride=hash_row_bytes(&p->nodes[root].expr)+(o->hash_defer_index?0:3*sizeof(uint32_t));
        double rows=p->nodes[root].estimated_rows;
        if(counted>=0 && p->nodes[source].kind==QP_AGGREGATE && !s->nodes[source].group_radix && p->nodes[source].expr.predicate<0)
            rows=atomic_load(s->nodes[source].group_count);
        bool staged=explicit_sink && s->stream_waves;
        if(staged && counted<0) {
            /* A completed inner-join build exposes its real cardinality.
             * Reuse the largest underestimate on this producer path. */
            double correction=1;
            for(int at=root;at!=source;at=p->nodes[at].left) {
                const struct qp_node *probe=&p->nodes[at];
                if(probe->kind!=QP_JOIN || !probe->hash || probe->join_type!=QP_INNER) continue;
                double expected=p->nodes[probe->right].estimated_rows;
                if(expected>0) {
                    double ratio=atomic_load_explicit(&s->nodes[at].hash_rows,memory_order_relaxed)/expected;
                    if(ratio>correction) correction=ratio;
                }
            }
            rows*=correction;
        }
        unsigned capacity=o->hash_capacity;
        if(staged) available+=(size_t)capacity*stride;
        while(capacity<=rows && capacity<UINT_MAX/4 && (size_t)capacity*2*stride<=available) capacity*=2;
        /* A staged build cannot grow the shared table while CUs hold
         * it. Allow one more tier for uncertain join output when the
         * added bytes use at most a quarter of the remaining space. */
        if(staged && counted<0 && capacity<UINT_MAX/4 && (size_t)capacity*stride<=available/5)
            capacity*=2;
        if(!o->hash_rows && capacity>o->hash_capacity) {
            void *data=hash_memory(s,hash_sink-1,staged?o->hash_data:NULL,(size_t)capacity*stride);
            if(data) {
                if(!staged) cs_work_free(s->context,o->hash_data);
                o->hash_data=data;
                o->hash_capacity=capacity;
                if(!o->hash_defer_index) hash_reindex(s,hash_sink-1,2*capacity);
            }
        }
    }
    const struct qp_node *join=&p->nodes[hash_sink-1];
    struct operator_state *o=&s->nodes[hash_sink-1];
    const struct qs_plan *right=&p->nodes[join->right].expr;
    /* A cached complete integer key proves existence. Collisions or
     * the reserved -1 tag still consult the exact Hash chain. This
     * avoids random payload loads for duplicate build keys without
     * assuming a dense key domain or a particular query. */
    if(!o->hash_rows && !o->hash_cache && hash_exists_only(join) &&
       right->exprs[right->outputs[join->right_keys[0]].index].type==Q_INTEGER) {
        o->hash_cache_slots=qp_hash_cache_slots(o->hash_capacity);
        /* The suffix is an exact bitmap for low integer keys. Set
         * bits only after a key is present in the main Hash table.
         * Other keys use the existing tags and exact chain. */
        size_t words=qp_hash_cache_words(o->hash_cache_slots);
        size_t bytes=words*sizeof(*o->hash_cache);
        size_t reserve=2*s->sort_limit+p->workers*(sizeof(*s)+p->count*sizeof(*s->nodes)+sizeof(struct qs_plan)+256);
#ifdef QP_SPLIT_RUNTIME
        size_t workspace=build_wave_workspace(s,root);
        if(workspace>reserve) reserve=workspace;
#endif
        size_t available=cs_work_available(s->context);
        if(available>reserve && bytes<=available-reserve)
            o->hash_cache=cs_work_calloc(s->context,words,sizeof(*o->hash_cache));
    }
    return 0;
}

static int prepare_graph_hash_wave(struct execution *s,unsigned id,unsigned root,int source)
{
    const struct operator_state *o=&s->nodes[id];
    if(!o->hash_data || o->hash_phase) return -1;
    return prepare_hash_wave(s,id+1,root,source,true);
}

static void finish_hash_wave_input(struct execution *s,unsigned id)
{
    hash_release(s->context,&s->nodes[id]);
}

static int64_t hash_build_wave(struct femu_csd_args *a)
{
    int id=graph_wave_node(a); if(id<0) return -1;
    const struct qp_plan *p=a->mr_addr[0];
    const struct qp_node *n=&p->nodes[id];
    int root=n->right;
    if(n->kind!=QP_JOIN || !n->hash || root<0 || (unsigned)root>=p->count) return -1;
    struct execution *s;
    int64_t status=graph_wave_begin(a,&graph_project_ops,graph_index_init,&s); if(status) return status;
    static const struct qs_result open_input, end_input={.finished=1};
    struct operator_state *o=&s->nodes[id];
    uint32_t route=(uint64_t)a->cparam1>>QP_GRAPH_PIPELINE_ROUTE_SHIFT;
    if(route&QP_GRAPH_BLOCK_PARTITION) {
        if(!o->external || o->external->phase!=1 || n->left<0) return -1;
        const struct graph_wave_sink sink={.node=id,.partition=id+1,.merge=merge_partition_worker};
        if(s->profiling) s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID);
        status=parallel_wave_next(s,n->left,NULL,&sink);
        if(status==QP_WAIT) return 0;
        if(status) return status==2 || status==3?-4:status;
        o->external->phase=2;
        return QP_GRAPH_PHASE_DONE;
    }
    if(!o->graph_wave_drained && !s->parallel_started && !o->graph_wave_input) {
        s->outputs->rows=s->outputs->begin=s->outputs->finished=0;
        struct cs_work_context_stats memory;
        if(cs_work_get_context_stats(s->context,&memory)) return -4;
        /* If even the packed input is estimated to exceed the entire query
         * cap, let each Build EXEC write final partitions from its vectors.
         * Avoid spooling raw overflow only to partition it at the barrier. */
        if(n->join_type==QP_INNER &&
           p->nodes[root].estimated_rows*hash_row_bytes(&p->nodes[root].expr)>memory.limit_bytes) {
            o->external=cs_work_calloc(s->context,1,sizeof(*o->external));
            if(!o->external) return -4;
            o->hash_fallback=2;
            o->graph_wave_input=true;
        } else {
            s->hash_input=&open_input; s->hash_input_node=root; s->hash_input_position=0;
            status=hash_build(s,id);
            s->hash_input=NULL;
            if(status!=QP_WAIT || !o->hash_data || o->external || o->hash_fallback)
                return status==QP_WAIT || !status?-4:status;
        }
    }
    if(o->graph_wave_input && !o->graph_wave_drained) {
        const struct graph_wave_sink sink={.node=id,.partition=id+1,.merge=merge_partition_worker};
        if(s->profiling) s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID);
        status=parallel_wave_next(s,root,NULL,&sink);
        if(status==QP_WAIT) return 0;
        if(status) return status==2 || status==3?-4:status;
        o->graph_wave_drained=true;
        if(s->profiling) {
            uint64_t rows=0,blocks=0;
            for(unsigned i=0;i<HASH_PARTITIONS;++i) {
                rows+=o->external->build[i].rows;
                blocks+=o->external->build[i].blocks;
            }
            fprintf(stderr,"CSD hash direct partition node=%d rows=%llu spill_write_bytes=%llu\n",
                id,(unsigned long long)rows,(unsigned long long)(blocks*QS_SORT_IO_BYTES));
        }
    } else if(!o->graph_wave_input) {
        /* A shared build can fill before all slots finish. Each producer
         * retains its excess rows. Once the wave is idle, drain those rows
         * in bounded packets and grow the same HashBuild state. */
        struct qs_result *packet=s->outputs;
        unsigned cols=p->nodes[root].expr.nout;
        if(!cols || cols>QS_OUT) return -1;
        unsigned capacity=QS_RESULT_ROWS*QS_OUT/cols;
        if(s->profiling) s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID);
        const struct graph_wave_sink sink={.node=id,.hash=id+1,.prepare=prepare_graph_hash_wave};
        while(!o->graph_wave_drained || packet->rows) {
            /* An interrupted build still owns the packet and its consumer
             * cursor. Resume that input before asking the producer for a row. */
            if(packet->rows && (packet->rows>=capacity || o->graph_wave_drained)) {
                s->hash_input=packet; s->hash_input_node=root;
                status=hash_build(s,id);
                s->hash_input=NULL;
                if(status!=QP_WAIT || o->hash_fallback==1 ||
                   s->hash_input_position!=packet->rows)
                    return status==QP_WAIT || !status?-4:status;
                packet->rows=0; s->hash_input_position=0;
                continue;
            }
            struct qs_cell *row=qp_result_row(packet,packet->rows,cols,1);
            status=parallel_wave_next(s,root,row,&sink);
            if(status==QP_WAIT) return 0;
            if(status==3) return -4;
            if(status!=2 && status) return status;
            if(status==2) ++packet->rows;
            else o->graph_wave_drained=true;
        }
    }
    s->hash_input=&end_input; s->hash_input_node=root; s->hash_input_position=0;
    status=hash_build(s,id);
    s->hash_input=NULL;
    if(status==QP_WAIT && o->external && o->external->phase==1)
        return QP_GRAPH_HASH_NEEDS_LEFT;
    if(status || !o->hash_phase || o->hash_fallback==1) return status==QP_WAIT?-4:status?status:-4;
    if(o->external) {
        struct external_hash *external=o->external;
        /* Publish a regular shared table once all spilled rows have been
         * reloaded. Until then the coordinator owns the spill tokens. */
        if(!external->direct) return -4;
        while(external->partition) {
            struct spool *part=&external->build[external->partition-1];
            while(part->blocks) {
                uint64_t token=part->pages[--part->blocks].token;
                if(token) return csNeedFree(token);
            }
            cs_work_free(s->context,part->pages);
            --external->partition;
        }
        cs_work_free(s->context,external);
        o->external=NULL; o->hash_fallback=0;
    }
    o->finish_wave=finish_hash_wave_input;
    return QP_GRAPH_PHASE_DONE;
}

/* One stream credit owns one build table. E9 calls this Source again
 * only after Probe has drained its partition and released that credit. */
static int64_t partition_reload(struct execution *s,struct femu_csd_args *a,unsigned worker,unsigned id)
{
    (void)a;
    const struct operator_state *owner=graph_state_owner(s,id);
    if(!worker || !owner->external || owner->external->direct) return -1;
    if(worker!=1) return graph_finish_stream();
    struct operator_state *o=&s->nodes[id];
    struct external_hash *external=o->external=owner->external;
    if(external->phase==3) {
        hash_release(s->context,o);
        o->hash_rows=o->hash_capacity=o->hash_phase=0;
        external->probe_at=0;
        external->phase=2;
    }
    int64_t status=external_build(s,id);
    if(status) { wait_io(s,status); return status; }
    if(o->hash_phase) return graph_publish_output();

    /* The last chunk has no reader. All spill tokens can now be released. */
    while(external->cleanup_partition<HASH_PARTITIONS) {
        unsigned part=external->cleanup_partition;
        struct spool *sides[]={&external->build[part],&external->probe[part]};
        for(unsigned i=0;i<2;++i) {
            struct spool *spool=sides[i];
            while(spool->blocks) {
                uint64_t token=spool->pages[--spool->blocks].token;
                if(token) {
                    int64_t request=csNeedFree(token);
                    wait_io(s,request);
                    return request;
                }
            }
            cs_work_free(s->owner->context,spool->pages);
            spool->pages=NULL;
        }
        ++external->cleanup_partition;
    }
    return graph_finish_stream();
}

int64_t pg_sina_graph_partition_reload(struct femu_csd_args *a)
{ return graph_tail_call(a,partition_reload,false); }

int64_t pg_sina_graph_hash_wave(struct femu_csd_args *a)
{
    int64_t status=hash_build_wave(a);
    if(status<0 && graph_operator_args(a) && (a->cparam1&1)) {
        struct qp_control *control=a->mr_addr[1];
        struct execution *s=(void *)(uintptr_t)control->state;
        int id=graph_wave_node(a);
        if(s && id>=0) {
            const struct operator_state *o=&s->nodes[id];
            struct cs_work_context_stats memory={0};
            cs_work_get_context_stats(s->context,&memory);
            fprintf(stderr,"CSD hash wave node=%d input=%d status=%lld rows=%u capacity=%u used=%zu limit=%zu\n",
                id,s->plan->nodes[id].right,(long long)status,atomic_load(&o->hash_rows),o->hash_capacity,
                memory.used_bytes,memory.limit_bytes);
        }
    }
    return status;
}
