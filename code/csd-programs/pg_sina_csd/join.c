/* Bounded inner hash join. Each spilled build batch is loaded once; the PG
 * scheduler explicitly replays the probe scan for it, including duplicate keys. */
#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "cs_csf.h"
#include "query.h"
#include <string.h>
#include <limits.h>
extern int64_t pg_sina_aggregate_sink_impl(struct femu_csd_args *);
extern int64_t pg_sina_filter_impl(struct femu_csd_args *);
struct hash_row { uint32_t next, padding; uint64_t nulls; };
struct build_batch { uint64_t *tokens; unsigned rows, blocks; };
/* Workmem guarantees 16-byte alignment. Space heads two cache lines apart so
 * adjacent slots never share the frequently written cursor fields. */
struct probe_cursor { unsigned position, match, pending; unsigned char padding[116]; };
struct hash_join {
    cs_work_context *context;
    unsigned char *data;
    uint32_t *buckets;
    struct build_batch *batches;
    unsigned ncols, key, stride, capacity, max_capacity, slots, rows;
    unsigned count, batch_capacity, input_position, block, loaded;
    int building, storing, loading, resident;
    struct probe_cursor probes[QS_WORKERS]; /* Private workmem, one per input slot. */
};
static uint64_t hash_key(uint64_t x)
{
    x^=x>>33; x*=UINT64_C(0xff51afd7ed558ccd);
    x^=x>>33; x*=UINT64_C(0xc4ceb9fe1a85ec53); return x^(x>>33);
}
static struct hash_row *row_at(struct hash_join *h,unsigned row)
{ return (void *)(h->data+(size_t)row*h->stride); }
static int64_t *row_values(struct hash_row *r) { return (void *)(r+1); }
static unsigned char *row_scales(struct hash_join *h,struct hash_row *r)
{ return (void *)(row_values(r)+h->ncols); }
static unsigned build_columns(const struct qs_plan *p,unsigned *key)
{
    unsigned n=0;
    for(unsigned col=p->nattrs-p->join.nattrs;col<p->nattrs;++col) if(p->cols[col].slot>=0) {
        if((int)col==p->join.build_key) *key=n;
        ++n;
    }
    return n;
}
static int init_join(const struct qs_plan *p,struct qs_join_control *control,struct hash_join **out)
{
    struct cs_work_stats stats; if(cs_work_get_stats(&stats)) return -4;
    size_t limit=p->join.work_mem_bytes;
    /* A build view selects one descriptor but retains the chain length, so
     * the sum of build shares also fits when the query limit exceeds the pool. */
    size_t physical=stats.capacity_bytes/2/(p->njoins?p->njoins:1);
    if(!limit || limit>physical) limit=physical;
    unsigned key=UINT_MAX,ncols=build_columns(p,&key);
    size_t stride=(sizeof(struct hash_row)+(size_t)ncols*9+7)&~(size_t)7;
    if(!p->join.relid || !ncols || key==UINT_MAX || limit<sizeof(struct hash_join)+2*QS_SORT_IO_BYTES) return -4;
    size_t max_rows=(limit-sizeof(struct hash_join)-QS_SORT_IO_BYTES)/(stride+8);
    unsigned capacity=1;
    while(capacity<=UINT_MAX/4 && (size_t)capacity*2<=max_rows) capacity*=2;
    unsigned max_capacity=capacity;
    uint64_t desired=p->join.rows>256?p->join.rows:256;
    unsigned wanted=1; while(wanted<desired && wanted<capacity) wanted*=2;
    if(wanted<capacity) capacity=wanted;
    if(capacity<2) return -4;
    cs_work_context *context=cs_work_begin_limit(limit); if(!context) return -4;
    struct hash_join *h=cs_work_calloc(context,1,sizeof(*h));
    if(!h) { cs_work_end(context); return -4; }
    h->context=context; h->ncols=ncols; h->key=key; h->stride=stride; h->capacity=capacity; h->max_capacity=max_capacity; h->slots=2*capacity;
    h->data=cs_work_alloc(context,(size_t)capacity*stride);
    h->buckets=cs_work_alloc(context,(size_t)h->slots*sizeof(*h->buckets));
    if(!h->data || !h->buckets) { cs_work_end(context); return -4; }
    h->building=1; h->loaded=UINT_MAX; control->state=(uintptr_t)h; *out=h; return 0;
}
/* Estimates choose the first allocation, never the spill threshold. During
 * build the bucket contents are unused; both arrays grow within this context.
 * The allocator charges old+new while copying, so growth can fail before the
 * final layout fills the limit. Keep the old capacity and spill in that case. */
static int grow_join(struct hash_join *h)
{
    if(h->capacity==h->max_capacity) return 0;
    unsigned next=h->capacity*2;
    void *data=cs_work_realloc(h->context,h->data,(size_t)next*h->stride);
    if(!data) return 0;
    h->data=data;
    void *buckets=cs_work_realloc(h->context,h->buckets,(size_t)next*8);
    if(!buckets) {
        h->data=cs_work_realloc(h->context,h->data,(size_t)h->capacity*h->stride);
        return 0;
    }
    h->buckets=buckets; h->capacity=next; h->slots=next*2; return 1;
}
static int64_t store_block(struct hash_join *h,void *io)
{
    unsigned block_rows=QS_SORT_IO_BYTES/h->stride,first=h->block*block_rows,n=h->rows-first;
    if(n>block_rows) n=block_rows;
    memset(io,0,QS_SORT_IO_BYTES); memcpy(io,h->data+(size_t)first*h->stride,(size_t)n*h->stride);
    return csNeedStore(QS_SORT_IO_BYTES);
}
static int64_t store_batch(struct hash_join *h,struct qs_join_control *control,struct femu_csd_args *a)
{
    void *io=a->mr_addr[a->numr-1];
    if(!h->storing) {
        if(h->count==h->batch_capacity) {
            unsigned capacity=h->batch_capacity?h->batch_capacity*2:8;
            void *batches=cs_work_realloc(h->context,h->batches,(size_t)capacity*sizeof(*h->batches));
            if(!batches) return -4;
            h->batches=batches; h->batch_capacity=capacity;
        }
        unsigned blocks=(h->rows+QS_SORT_IO_BYTES/h->stride-1)/(QS_SORT_IO_BYTES/h->stride);
        uint64_t *tokens=cs_work_calloc(h->context,blocks,sizeof(*tokens)); if(!tokens) return -4;
        h->batches[h->count++]=(struct build_batch){.tokens=tokens,.rows=h->rows,.blocks=blocks};
        h->storing=1; h->block=0;
    } else if(csIsReentry(a)) {
        struct build_batch *b=&h->batches[h->count-1];
        b->tokens[h->block++]=csReentryValue(a); control->spill_write_bytes+=QS_SORT_IO_BYTES;
        if(h->block==b->blocks) { h->rows=0; h->storing=0; return 0; }
    }
    return store_block(h,io);
}
int64_t pg_sina_join_build(struct femu_csd_args *a)
{
    if(!a || a->numr!=4 || a->mr_len[1]<(long long)sizeof(struct qs_plan) ||
        a->mr_len[2]<(long long)sizeof(struct qs_join_control) || a->mr_len[3]<QS_SORT_IO_BYTES) return -1;
    struct qs_chunk *c=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1]; struct qs_join_control *control=a->mr_addr[2];
    if(p->nattrs>QS_COLS || p->join.nattrs>p->nattrs || p->ncols>QS_COLS ||
        a->mr_len[0]<(long long)sizeof(*c) || c->selected>c->rows || c->rows>c->capacity ||
        qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0]) return -1;
    struct hash_join *h=(void *)(uintptr_t)control->state;
    if(!h) { int status=init_join(p,control,&h); if(status) return status; }
    if(!h->building || c->ncols!=h->ncols) return -1;
    if(!csIsReentry(a)) { h->input_position=0; control->rows+=c->rows; }
    if(h->storing) { int64_t status=store_batch(h,control,a); if(status) return status; }
    while(h->input_position<c->selected) {
        if(h->rows==h->capacity && !grow_join(h)) { int64_t status=store_batch(h,control,a); if(status) return status; }
        unsigned row=qs_selection(c)[h->input_position++]; if(row>=c->rows) return -1;
        if(qs_nulls(c)[(size_t)h->key*((c->capacity+7)/8)+row/8]&(1U<<(row%8))) continue;
        struct hash_row *r=row_at(h,h->rows++); r->next=UINT_MAX; r->nulls=0;
        for(unsigned col=0;col<h->ncols;++col) {
            if(qs_nulls(c)[(size_t)col*((c->capacity+7)/8)+row/8]&(1U<<(row%8))) r->nulls|=UINT64_C(1)<<col;
            row_values(r)[col]=qs_values(c)[(size_t)col*c->capacity+row];
            row_scales(h,r)[col]=qs_dscales(c)[(size_t)col*c->capacity+row];
        }
    }
    return 0;
}
static void index_batch(struct hash_join *h)
{
    memset(h->buckets,0xff,(size_t)h->slots*sizeof(*h->buckets));
    for(unsigned i=0;i<h->rows;++i) {
        struct hash_row *r=row_at(h,i); unsigned bucket=hash_key(row_values(r)[h->key])&(h->slots-1);
        r->next=h->buckets[bucket]; h->buckets[bucket]=i;
    }
}
/* control.current is chosen by PG only after all probes of the old batch end. */
int64_t pg_sina_join_load(struct femu_csd_args *a)
{
    if(!a || a->numr!=2 || a->mr_len[0]<(long long)sizeof(struct qs_join_control) || a->mr_len[1]<QS_SORT_IO_BYTES) return -1;
    struct qs_join_control *control=a->mr_addr[0]; struct hash_join *h=(void *)(uintptr_t)control->state;
    if(!h) return -1;
    if(h->building) {
        if(h->count && (h->rows || h->storing)) { int64_t status=store_batch(h,control,a); if(status) return status; }
        h->building=0; h->resident=!h->count; control->batches=h->resident?1:h->count;
        if(h->resident) { index_batch(h); h->loaded=0; }
    }
    if(control->current>=control->batches) return -1;
    if(h->loaded==control->current) return 0;
    struct build_batch *b=&h->batches[control->current]; unsigned block_rows=QS_SORT_IO_BYTES/h->stride;
    if(!h->loading) { h->block=0; h->loading=1; }
    else if(csIsReentry(a)) {
        unsigned first=h->block*block_rows,n=b->rows-first; if(n>block_rows) n=block_rows;
        memcpy(h->data+(size_t)first*h->stride,a->mr_addr[1],(size_t)n*h->stride);
        ++h->block; control->spill_read_bytes+=QS_SORT_IO_BYTES;
    }
    if(h->block<b->blocks) return csNeedLoad(b->tokens[h->block]);
    h->rows=b->rows; index_batch(h); h->loaded=control->current; h->loading=0; return 0;
}
static int64_t aggregate_chunk(struct qs_chunk *out,const struct qs_plan *p,struct qs_task *task,struct femu_csd_args *outer,int reentry)
{
    if(!reentry) {
        if(p->join.residual && out->rows) {
            /* Both physical filters have materialized numeric values already.
             * This filter operates on joined vectors, never on heap offsets. */
            struct qs_plan filter=*p; filter.predicate=p->join.residual-1;
            for(unsigned i=0;i<filter.nattrs;++i) filter.cols[i].deferred=0;
            void *mr[]={out,&filter}; long long len[]={qs_chunk_bytes(out->capacity,out->ncols),sizeof(filter)};
            struct femu_csd_args call={.numr=2,.mr_addr=mr,.mr_len=len};
            int64_t status=pg_sina_filter_impl(&call); if(status) return status;
        }
        task->joined+=out->selected;
    }
    struct qs_task inner=*task; inner.input_bytes=0;
    void *mr[]={out,(void *)p,&inner,p->external_aggregate?outer->mr_addr[5]:NULL};
    long long len[]={qs_chunk_bytes(out->capacity,out->ncols),sizeof(*p),sizeof(inner),p->external_aggregate?outer->mr_len[5]:0};
    struct femu_csd_args call={.numr=p->external_aggregate?4:3,.mr_addr=mr,.mr_len=len,.cparam2=reentry?outer->cparam2:0};
    int64_t status=pg_sina_aggregate_sink_impl(&call); task->state=inner.state; return status;
}
/* All stages keep their cursor in their own build workmem. AFDM contains
 * only the input/output column batches; a child may retain its input while
 * the final aggregate yields NEED_STORE/LOAD through the whole call chain. */
static int64_t probe_stage(unsigned,struct qs_chunk *,const struct qs_plan *,struct qs_task *,struct qs_join_control *,struct femu_csd_args *,int);
static int64_t consume_join(unsigned stage,struct qs_chunk *out,const struct qs_plan *p,struct qs_task *task,
    struct qs_join_control *control,struct femu_csd_args *a,int reentry)
{
    if(stage+1<qs_join_count(p)) return probe_stage(stage+1,out,p,task,control,a,reentry);
    return aggregate_chunk(out,p,task,a,reentry);
}
static int64_t probe_stage(unsigned stage,struct qs_chunk *c,const struct qs_plan *p,struct qs_task *task,
    struct qs_join_control *control,struct femu_csd_args *a,int reentry)
{
    const struct qs_join *j=qs_join_at(p,stage);
    struct hash_join *h=(void *)(uintptr_t)control[stage].state;
    struct qs_chunk *out=(void *)((unsigned char *)a->mr_addr[4]+stage*qs_join_chunk_stride(p));
    unsigned base=qs_probe_attrs(p);
    for(unsigned i=0;i<stage;++i) base+=qs_join_at(p,i)->nattrs;
    unsigned probe_cols[QS_COLS],build_cols[QS_COLS],np=0,nb=0,key=UINT_MAX;
    for(unsigned i=0;i<base+j->nattrs;++i) if(p->cols[i].slot>=0) {
        if(i<base) { if((int)i==j->probe_key) key=stage?(unsigned)p->cols[i].slot:np; probe_cols[np++]=p->cols[i].slot; }
        else build_cols[nb++]=p->cols[i].slot;
    }
    if((!stage && np!=c->ncols) || nb!=h->ncols || key==UINT_MAX) return -1;
    struct probe_cursor *cursor=&h->probes[task->slot];
    if(reentry) {
        if(!cursor->pending) return -1;
        int64_t status=consume_join(stage,out,p,task,control,a,1); if(status) return status;
        cursor->pending=0; out->rows=out->selected=0;
    } else { cursor->position=cursor->match=cursor->pending=0; out->rows=out->selected=0; }
    while(cursor->position<c->selected) {
        unsigned row=qs_selection(c)[cursor->position]; if(row>=c->rows) return -1;
        if(qs_nulls(c)[(size_t)key*((c->capacity+7)/8)+row/8]&(1U<<(row%8))) { ++cursor->position; continue; }
        int64_t value=qs_values(c)[(size_t)key*c->capacity+row];
        unsigned at=cursor->match?cursor->match-1:h->buckets[hash_key(value)&(h->slots-1)];
        if(at==UINT_MAX) { ++cursor->position; continue; }
        struct hash_row *r=row_at(h,at);
        /* Save the next pair before a possibly reentrant aggregate call. */
        cursor->match=r->next==UINT_MAX?0:r->next+1;
        if(!cursor->match) ++cursor->position;
        if(row_values(r)[h->key]!=value) continue;
        if(!out->rows) memset(qs_nulls(out),0,((out->capacity+7)/8)*(size_t)out->ncols);
        unsigned dest=out->rows;
        for(unsigned col=0;col<np+nb;++col) {
            unsigned local=col<np?col:col-np,slot=col<np?probe_cols[local]:build_cols[local];
            int null=col<np?!!(qs_nulls(c)[(size_t)(stage?slot:local)*((c->capacity+7)/8)+row/8]&(1U<<(row%8))):!!(r->nulls&(UINT64_C(1)<<local));
            qs_values(out)[(size_t)slot*out->capacity+dest]=col<np?qs_values(c)[(size_t)(stage?slot:local)*c->capacity+row]:row_values(r)[local];
            qs_dscales(out)[(size_t)slot*out->capacity+dest]=col<np?qs_dscales(c)[(size_t)(stage?slot:local)*c->capacity+row]:row_scales(h,r)[local];
            if(null) qs_nulls(out)[(size_t)slot*((out->capacity+7)/8)+dest/8]|=1U<<(dest%8);
        }
        qs_selection(out)[dest]=dest; ++out->rows; ++out->selected;
        if(out->rows==out->capacity) {
            cursor->pending=1; int64_t status=consume_join(stage,out,p,task,control,a,0); if(status) return status;
            cursor->pending=0; out->rows=out->selected=0;
        }
    }
    if(out->rows) {
        cursor->pending=1; int64_t status=consume_join(stage,out,p,task,control,a,0); if(status) return status;
        cursor->pending=0;
    }
    return 0;
}
/* MR order remains unchanged: probe columns, plan, task, build controls,
 * intermediate joined column batches, optional spill I/O. */
int64_t pg_sina_join_probe(struct femu_csd_args *a)
{
    if(!a || (a->numr!=5 && a->numr!=6) || a->mr_len[1]<(long long)sizeof(struct qs_plan) ||
        a->mr_len[2]<(long long)sizeof(struct qs_task)) return -1;
    struct qs_chunk *c=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1]; struct qs_task *task=a->mr_addr[2];
    unsigned count=qs_join_count(p);
    if(!count || count>QS_JOINS || p->nattrs>QS_COLS || p->ncols>QS_COLS || task->slot>=QS_WORKERS ||
        a->mr_len[3]<(long long)(count*sizeof(struct qs_join_control)) ||
        (p->external_aggregate && (a->numr!=6 || a->mr_len[5]<QS_SORT_IO_BYTES)) ||
        a->mr_len[0]<(long long)sizeof(*c) || c->rows>c->capacity || c->selected>c->rows ||
        qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0] ||
        a->mr_len[4]<(long long)((count-1)*qs_join_chunk_stride(p)+qs_chunk_bytes(256,p->ncols))) return -1;
    struct qs_join_control *control=a->mr_addr[3];
    for(unsigned i=0;i<count;++i) {
        struct hash_join *h=(void *)(uintptr_t)control[i].state;
        struct qs_chunk *out=(void *)((unsigned char *)a->mr_addr[4]+i*qs_join_chunk_stride(p));
        if(!h || h->building || h->loaded!=control[i].current || out->capacity!=256 || out->ncols!=p->ncols) return -1;
    }
    if(!csIsReentry(a)) {
        if(!task->state) {
            struct qs_chunk *out=(void *)((unsigned char *)a->mr_addr[4]+(count-1)*qs_join_chunk_stride(p));
            out->rows=out->selected=0;
            int64_t status=aggregate_chunk(out,p,task,a,0); if(status) return status;
        }
        task->scanned+=c->rows; task->matched+=c->selected;
    }
    return probe_stage(0,c,p,task,control,a,csIsReentry(a));
}
/* Device close owns token reclamation, including already-rolled-back tokens. */
int64_t pg_sina_join_close(struct femu_csd_args *a)
{
    if(!a || a->numr!=1 || a->mr_len[0]<(long long)sizeof(struct qs_join_control)) return -1;
    struct qs_join_control *control=a->mr_addr[0]; struct hash_join *h=(void *)(uintptr_t)control->state;
    if(h) { cs_work_context *context=h->context; control->state=0; cs_work_end(context); }
    return 0;
}
