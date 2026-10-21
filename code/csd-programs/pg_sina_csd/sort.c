#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "query_eval.h"
static int compare(const struct qs_row *a,const struct qs_row *b,const struct qs_plan *p)
{
    for(unsigned i=0;i<p->norder;++i) {
        const struct qs_order *key=&p->order[i];
        struct qs_cell x=a->cells[key->output],y=b->cells[key->output]; int cmp;
        if(x.isnull || y.isnull) {
            cmp=(x.isnull?1:0)-(y.isnull?1:0); if(key->nulls_first) cmp=-cmp;
        } else {
            if(qs_compare(x,y,&cmp)) return 0;
            if(key->descending) cmp=-cmp;
        }
        if(cmp) return cmp;
    }
    return 0;
}
static void sift(const struct qs_row *rows,uint16_t *order,unsigned n,unsigned root,const struct qs_plan *p)
{
    for(;;) {
        unsigned child=root*2+1; if(child>=n) break;
        if(child+1<n && compare(&rows[order[child]],&rows[order[child+1]],p)<0) ++child;
        if(compare(&rows[order[root]],&rows[order[child]],p)>=0) break;
        uint16_t temp=order[root]; order[root]=order[child]; order[child]=temp; root=child;
    }
}
int64_t pg_sina_sort(struct femu_csd_args *a)
{
    if(!a || a->numr!=2 || a->mr_len[0]<(long long)sizeof(struct qs_result) || a->mr_len[1]<(long long)sizeof(struct qs_plan)) return -1;
    struct qs_result *out=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1];
    if(out->rows>QS_GROUPS || p->norder>QS_OUT) return -1;
    if(out->rows<2) return 0;
    struct cs_work_stats stats; if(cs_work_get_stats(&stats)) return -4;
    size_t limit=p->work_mem_bytes && p->work_mem_bytes<stats.capacity_bytes?p->work_mem_bytes:stats.capacity_bytes;
    cs_work_context *ctx=cs_work_begin_limit(limit); if(!ctx) return -4;
    struct qs_row *rows=cs_work_alloc(ctx,out->rows*(sizeof(*rows)+sizeof(uint16_t)));
    if(!rows) { cs_work_end(ctx); return -4; }
    uint16_t *order=(void *)(rows+out->rows);
    for(unsigned i=0;i<out->rows;++i) order[i]=i;
    for(unsigned i=out->rows/2;i>0;--i) sift(out->data,order,out->rows,i-1,p);
    for(unsigned n=out->rows;n>1;--n) {
        uint16_t temp=order[0]; order[0]=order[n-1]; order[n-1]=temp; sift(out->data,order,n-1,0,p);
    }
    for(unsigned i=0;i<out->rows;++i) rows[i]=out->data[order[i]];
    memcpy(out->data,rows,out->rows*sizeof(*rows)); cs_work_end(ctx); return 0;
}

/* External row sort. AFDMs are input/output and spill I/O only; runs, their
 * heads and expression scratch belong to workmem. The original small result
 * sort above remains the aggregate fast path. */
#include "cs_csf.h"
#include "query_vector.h"
#define RUN_BYTES (1024U*1024)
#define RUN_BLOCKS (RUN_BYTES/QS_SORT_IO_BYTES+1)
struct row_run {
    unsigned char *data;
    cs_work_context *owner;
    uint64_t blocks[RUN_BLOCKS];
    struct qs_cell head[QS_OUT];
    unsigned rows, position, loaded;
    int sorted, spilled;
};
struct row_sort {
    cs_work_context *context;
    struct qs_plan plan;
    struct qs_vector *vectors;
    uint8_t needed[QS_EXPRS];
    struct row_run *runs;
    unsigned count, capacity, row_bytes, run_rows, block_rows, input_position;
    uint64_t topn, skipped, emitted;
    int bounded, combined, root_pending, finishing;
    struct row_sort *owners[QS_WORKERS];
    unsigned owners_count;
    struct row_run **heap, *pending_run;
    unsigned heap_count, pending_block, pending;
    struct qs_metrics metrics;
};
enum { ROW_NONE, ROW_STORE, ROW_LOAD, ROW_FREE };
static int row_compare(const struct qs_cell *a,const struct qs_cell *b,const struct qs_plan *p)
{
    for(unsigned i=0;i<p->norder;++i) {
        const struct qs_order *key=&p->order[i]; struct qs_cell x=a[key->output],y=b[key->output]; int cmp;
        if(x.isnull || y.isnull) {
            cmp=(x.isnull?1:0)-(y.isnull?1:0); if(key->nulls_first) cmp=-cmp;
        } else { qs_compare(x,y,&cmp); if(key->descending) cmp=-cmp; }
        if(cmp) return cmp;
    }
    return 0;
}
static struct qs_cell *run_row(const struct row_sort *s,const struct row_run *r,unsigned i)
{ return (void *)(r->data+(size_t)i*s->row_bytes); }
static void row_swap(struct row_sort *s,struct row_run *r,unsigned a,unsigned b)
{
    struct qs_cell temp[QS_OUT];
    memcpy(temp,run_row(s,r,a),s->row_bytes);
    memcpy(run_row(s,r,a),run_row(s,r,b),s->row_bytes);
    memcpy(run_row(s,r,b),temp,s->row_bytes);
}
static void row_sift(struct row_sort *s,struct row_run *r,unsigned size,unsigned root)
{
    for(;;) {
        unsigned child=root*2+1; if(child>=size) break;
        if(child+1<size && row_compare(run_row(s,r,child),run_row(s,r,child+1),&s->plan)<0) ++child;
        if(row_compare(run_row(s,r,root),run_row(s,r,child),&s->plan)>=0) break;
        row_swap(s,r,root,child); root=child;
    }
}
static void run_sort(struct row_sort *s,struct row_run *r)
{
    if(r->sorted) return;
    for(unsigned i=r->rows/2;i;i--) row_sift(s,r,r->rows,i-1);
    for(unsigned n=r->rows;n>1;--n) { row_swap(s,r,0,n-1); row_sift(s,r,n-1,0); }
    r->sorted=1;
    if(r->rows) memcpy(r->head,r->data,s->row_bytes);
}
static int64_t row_store(struct row_sort *s,struct row_run *r,struct femu_csd_args *a)
{
    s->pending_run=r; s->pending=ROW_STORE;
    unsigned first=s->pending_block*s->block_rows,n=r->rows-first;
    if(n>s->block_rows) n=s->block_rows;
    void *io=a->mr_addr[a->numr-1]; memset(io,0,QS_SORT_IO_BYTES);
    memcpy(io,r->data+(size_t)first*s->row_bytes,(size_t)n*s->row_bytes);
    return csNeedStore(QS_SORT_IO_BYTES);
}
static int64_t row_evict(struct row_sort *s,cs_work_context *owner,struct femu_csd_args *a)
{
    /* A spilled run's read buffer is only a cache. Its head is retained in
     * metadata, so release the buffer before writing another resident run. */
    for(unsigned i=0;i<s->owners_count;++i) {
        struct row_sort *o=s->owners[i];
        for(unsigned j=0;j<o->count;++j) {
            struct row_run *r=&o->runs[j];
            if(r->owner==owner && r->spilled && r->data) {
                cs_work_free(owner,r->data); r->data=NULL; r->loaded=UINT32_MAX; return 0;
            }
        }
    }
    for(unsigned i=0;i<s->owners_count;++i) {
        struct row_sort *o=s->owners[i];
        for(unsigned j=0;j<o->count;++j) {
            struct row_run *r=&o->runs[j];
            if(r->owner==owner && r->data && !r->spilled && r->sorted && r->rows) {
                s->pending_block=0; return row_store(s,r,a);
            }
        }
    }
    return -4;
}
static int64_t row_resume(struct row_sort *s,struct femu_csd_args *a)
{
    if(!csIsReentry(a)) return 0;
    struct row_run *r=s->pending_run;
    if(!r) return -1;
    if(s->pending==ROW_STORE) {
        if(s->pending_block>=RUN_BLOCKS) return -1;
        r->blocks[s->pending_block++]=csReentryValue(a);
        s->metrics.spill_write_bytes+=QS_SORT_IO_BYTES;
        if(s->pending_block*s->block_rows<r->rows) return row_store(s,r,a);
        cs_work_free(r->owner,r->data); r->data=NULL; r->spilled=1; r->loaded=UINT32_MAX;
        ++s->metrics.spilled_runs;
    } else if(s->pending==ROW_LOAD) {
        memcpy(r->data,a->mr_addr[a->numr-1],QS_SORT_IO_BYTES);
        r->loaded=s->pending_block; s->metrics.spill_read_bytes+=QS_SORT_IO_BYTES;
    } else if(s->pending==ROW_FREE) r->blocks[s->pending_block]=0;
    else return -1;
    s->pending=ROW_NONE; return 0;
}
static int row_init(struct qs_task *task,const struct qs_plan *p,struct row_sort **out)
{
    unsigned workers=task->workers?task->workers:1;
    if(workers>QS_WORKERS) return -1;
    size_t limit=qs_sink_memory(p,workers); if(!limit) return -4;
    cs_work_context *context=cs_work_begin_limit(limit); if(!context) return -4;
    struct row_sort *s=cs_work_calloc(context,1,sizeof(*s));
    if(!s) { cs_work_end(context); return -4; }
    s->context=context; s->plan=*p; s->owners[0]=s; s->owners_count=1;
    s->row_bytes=p->nout*sizeof(struct qs_cell); s->block_rows=QS_SORT_IO_BYTES/s->row_bytes;
    unsigned bytes=RUN_BYTES; while(bytes>QS_SORT_IO_BYTES && bytes>limit/4) bytes/=2;
    /* Whole I/O blocks avoid an almost-empty extra block per full run. */
    s->run_rows=(bytes/QS_SORT_IO_BYTES)*s->block_rows;
    s->vectors=cs_work_alloc(context,p->nexprs*sizeof(*s->vectors));
    if(p->nexprs && !s->vectors) { cs_work_end(context); return -4; }
    for(unsigned i=0;i<p->nout;++i)
        if(qs_vector_require(p,p->outputs[i].index,s->needed)) { cs_work_end(context); return -1; }
    if(p->limit>=0 && (uint64_t)p->limit<=UINT64_MAX-(uint64_t)p->offset) {
        s->topn=(uint64_t)p->limit+(uint64_t)p->offset;
        s->bounded=s->topn<=s->run_rows;
    }
    task->state=(uintptr_t)s; *out=s; return 0;
}
/* chunk, plan, task(handle output), spill I/O. Reentry resumes this input chunk. */
int64_t pg_sina_sort_sink(struct femu_csd_args *a)
{
    if(!a || a->numr!=4 || a->mr_len[1]<(long long)sizeof(struct qs_plan) ||
        a->mr_len[2]<(long long)sizeof(struct qs_task) || a->mr_len[3]<QS_SORT_IO_BYTES) return -1;
    struct qs_chunk *c=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1]; struct qs_task *task=a->mr_addr[2];
    if(!p->nout || p->nout>QS_OUT || p->nexprs>QS_EXPRS || p->norder>QS_OUT ||
        a->mr_len[0]<(long long)sizeof(*c) || c->selected>c->rows || c->rows>c->capacity ||
        qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0]) return -1;
    struct row_sort *s=(void *)(uintptr_t)task->state;
    if(!s) { int status=row_init(task,p,&s); if(status) return status; }
    if(!csIsReentry(a)) {
        s->input_position=0; s->metrics.scanned+=c->rows; s->metrics.matched+=c->selected;
        s->metrics.input_bytes+=task->input_bytes; ++s->metrics.chunks;
    }
    int64_t status=row_resume(s,a); if(status) return status;
    while(s->input_position<c->selected) {
        if(s->bounded && !s->topn) { s->input_position=c->selected; break; }
        struct row_run *r=s->count?&s->runs[s->count-1]:NULL;
        if(!r || (!s->bounded && r->sorted)) {
            if(s->count==s->capacity) {
                unsigned capacity=s->capacity?s->capacity*2:8;
                void *runs=cs_work_realloc(s->context,s->runs,capacity*sizeof(*s->runs));
                if(!runs) return row_evict(s,s->context,a);
                s->runs=runs; s->capacity=capacity;
            }
            unsigned n=s->bounded?s->topn:s->run_rows;
            /* Budget for the next run, not a fixed free-memory percentage.
             * Leave one I/O block plus allocation overhead for merge/metadata.
             * Allocation failure still handles fragmentation/global pressure. */
            struct cs_work_context_stats memory;
            if(cs_work_get_context_stats(s->context,&memory)) return -4;
            if(memory.limit_bytes-memory.used_bytes<(size_t)n*s->row_bytes+QS_SORT_IO_BYTES+4096)
                return row_evict(s,s->context,a);
            void *data=cs_work_alloc(s->context,(size_t)n*s->row_bytes);
            if(!data) return row_evict(s,s->context,a);
            r=&s->runs[s->count++]; memset(r,0,sizeof(*r)); r->data=data; r->owner=s->context;
        }
        unsigned count=c->selected-s->input_position;
        if(count>QS_VECTOR_ROWS) count=QS_VECTOR_ROWS;
        if(!s->bounded && count>s->run_rows-r->rows) count=s->run_rows-r->rows;
        if(qs_vector_execute(p,c,qs_selection(c),s->input_position,count,s->needed,s->vectors)) return -5;
        for(unsigned i=0;i<count;++i) {
            struct qs_cell row[QS_OUT];
            for(unsigned col=0;col<p->nout;++col) {
                unsigned expr=p->outputs[col].index;
                if(s->vectors[expr].errors[i]) return -5;
                row[col]=qs_vector_cell(p,s->vectors,expr,i);
            }
            if(s->bounded && r->rows==s->topn) {
                if(row_compare(row,run_row(s,r,0),p)<0) {
                    memcpy(run_row(s,r,0),row,s->row_bytes); row_sift(s,r,r->rows,0);
                }
            } else {
                memcpy(run_row(s,r,r->rows++),row,s->row_bytes);
                if(s->bounded) {
                    unsigned child=r->rows-1;
                    while(child && row_compare(run_row(s,r,(child-1)/2),run_row(s,r,child),p)<0) {
                        row_swap(s,r,child,(child-1)/2); child=(child-1)/2;
                    }
                }
            }
        }
        s->input_position+=count;
        if(!s->bounded && r->rows==s->run_rows) run_sort(s,r);
    }
    return 0;
}
static int heap_compare(const struct row_sort *s,unsigned a,unsigned b)
{ return row_compare(s->heap[a]->head,s->heap[b]->head,&s->plan); }
static void merge_sift(struct row_sort *s,unsigned root)
{
    for(;;) {
        unsigned child=root*2+1; if(child>=s->heap_count) break;
        if(child+1<s->heap_count && heap_compare(s,child+1,child)<0) ++child;
        if(heap_compare(s,root,child)<=0) break;
        struct row_run *r=s->heap[root]; s->heap[root]=s->heap[child]; s->heap[child]=r; root=child;
    }
}
static int row_combine(struct row_sort *s,struct qs_group *group)
{
    if(!group->count || group->count>QS_WORKERS || group->states[0]!=(uintptr_t)s) return -1;
    if(!s->combined) {
        s->owners_count=group->count;
        for(unsigned i=0;i<group->count;++i) {
            struct row_sort *o=(void *)(uintptr_t)group->states[i];
            if(!o) return -1;
            for(unsigned j=0;j<i;++j) if(s->owners[j]==o) return -1;
            s->owners[i]=o;
        }
        for(unsigned i=0;i<s->owners_count;++i) {
            struct row_sort *o=s->owners[i];
            if(o->vectors) { cs_work_free(o->context,o->vectors); o->vectors=NULL; }
            for(unsigned j=0;j<o->count;++j) run_sort(o,&o->runs[j]);
            if(i) {
                s->metrics.scanned+=o->metrics.scanned; s->metrics.matched+=o->metrics.matched;
                s->metrics.input_bytes+=o->metrics.input_bytes; s->metrics.chunks+=o->metrics.chunks;
                s->metrics.spill_write_bytes+=o->metrics.spill_write_bytes; s->metrics.spilled_runs+=o->metrics.spilled_runs;
            }
            s->metrics.runs+=o->count;
        }
        s->metrics.workers=group->count; s->combined=1;
    }
    return 0;
}
static int64_t merge_head(struct row_sort *s,struct row_run *r,struct femu_csd_args *a)
{
    if(r->spilled) {
        while(!r->data) {
            r->data=cs_work_alloc(r->owner,QS_SORT_IO_BYTES);
            if(!r->data) { int64_t status=row_evict(s,r->owner,a); if(status) return status; }
        }
        unsigned block=r->position/s->block_rows;
        if(r->loaded!=block) {
            s->pending_run=r; s->pending_block=block; s->pending=ROW_LOAD;
            return csNeedLoad(r->blocks[block]);
        }
        memcpy(r->head,r->data+(size_t)(r->position%s->block_rows)*s->row_bytes,s->row_bytes);
    } else memcpy(r->head,r->data+(size_t)r->position*s->row_bytes,s->row_bytes);
    return 0;
}
static int64_t row_finish(struct row_sort *s,struct qs_group *group,struct qs_result *out)
{
    for(unsigned i=0;i<s->owners_count;++i) {
        struct row_sort *o=s->owners[i];
        for(unsigned j=0;j<o->count;++j) for(unsigned b=0;b<RUN_BLOCKS;++b) if(o->runs[j].blocks[b]) {
            s->pending_run=&o->runs[j]; s->pending_block=b; s->pending=ROW_FREE;
            return csNeedFree(o->runs[j].blocks[b]);
        }
    }
    if(out) {
        struct cs_work_stats stats; if(cs_work_get_stats(&stats)) return -4;
        s->metrics.work_peak=stats.peak_used_bytes; out->metrics=s->metrics; out->finished=1;
    }
    for(unsigned i=1;i<s->owners_count;++i) cs_work_end(s->owners[i]->context);
    cs_work_context *context=s->context;
    memset(group->states,0,sizeof(group->states)); cs_work_end(context);
    return 0;
}
/* group, output, spill I/O. Cleanup uses group, spill I/O and the same batch
 * reentry protocol, so aborts also release tokens before destroying workmem. */
int64_t pg_sina_sort_source(struct femu_csd_args *a)
{
    if(!a || (a->numr!=1 && a->numr!=2 && a->numr!=3) || a->mr_len[0]<(long long)sizeof(struct qs_group) ||
        (a->numr!=1 && a->mr_len[a->numr-1]<QS_SORT_IO_BYTES)) return -1;
    struct qs_group *group=a->mr_addr[0];
    if(!group->count || group->count>QS_WORKERS) return -1;
    /* PG closes this query's SNIA device immediately afterwards. That releases
     * any surviving tokens, including tokens already rolled back by a failed
     * batch. Do not require a stale token to be freed twice to release RAM. */
    if(a->numr==1) {
        for(unsigned i=0;i<group->count;++i) if(group->states[i]) {
            struct row_sort *owner=(void *)(uintptr_t)group->states[i];
            cs_work_end(owner->context); group->states[i]=0;
        }
        return 0;
    }
    if(a->numr==2 && !csIsReentry(a)) {
        unsigned count=0;
        for(unsigned i=0;i<group->count;++i) if(group->states[i]) group->states[count++]=group->states[i];
        for(unsigned i=count;i<group->count;++i) group->states[i]=0;
        if(!count) return 0;
        group->count=count;
    }
    struct row_sort *s=(void *)(uintptr_t)group->states[0];
    if(!s) return a->numr==2?0:-1;
    if(a->numr==2 && !csIsReentry(a)) { s->pending=ROW_NONE; s->pending_run=NULL; }
    int64_t status=row_resume(s,a); if(status) return status;
    if(row_combine(s,group)) return -1;
    if(a->numr==2) return row_finish(s,group,NULL);
    if(a->mr_len[1]<(long long)sizeof(struct qs_result)) return -1;
    struct qs_result *out=a->mr_addr[1];
    if(!csIsReentry(a)) { out->status=0; out->rows=out->begin=out->finished=0; }
    if(!s->heap && !s->finishing) {
        while(!s->heap) {
            s->heap=cs_work_alloc(s->context,(s->metrics.runs+1)*sizeof(*s->heap));
            if(!s->heap) { int64_t status=row_evict(s,s->context,a); if(status) return status; }
        }
        for(unsigned i=0;i<s->owners_count;++i)
            for(unsigned j=0;j<s->owners[i]->count;++j) if(s->owners[i]->runs[j].rows)
                s->heap[s->heap_count++]=&s->owners[i]->runs[j];
        for(unsigned i=s->heap_count/2;i;i--) merge_sift(s,i-1);
    }
    while(!s->finishing && out->rows<QS_RESULT_ROWS) {
        if(!s->heap_count || (s->plan.limit>=0 && s->emitted>=(uint64_t)s->plan.limit)) { s->finishing=1; break; }
        struct row_run *r=s->heap[0];
        if(s->root_pending) {
            if(r->position==r->rows) {
                if(r->data) { cs_work_free(r->owner,r->data); r->data=NULL; }
                s->heap[0]=s->heap[--s->heap_count];
            } else { status=merge_head(s,r,a); if(status) return status; }
            if(s->heap_count) merge_sift(s,0);
            s->root_pending=0; continue;
        }
        if(s->skipped<(uint64_t)s->plan.offset) ++s->skipped;
        else { memcpy(out->data[out->rows++].cells,r->head,s->row_bytes); ++s->emitted; }
        ++r->position; s->root_pending=1;
    }
    out->metrics=s->metrics;
    if(s->finishing) return row_finish(s,group,out);
    return 0;
}
