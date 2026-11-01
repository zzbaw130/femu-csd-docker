#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "query_eval.h"
static int compare(const struct qs_row *a,const struct qs_row *b,const struct qs_plan *p)
{
    for(unsigned i=0;i<p->norder;++i) {
        const struct qs_order *key=&p->order[i];
        struct qs_cell x=a->cells[key->output],y=b->cells[key->output]; int cmp=0;
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
#include "dictionary.h"
#define RUN_BYTES (1024U*1024)
/* A run owns its strings. Cells retain the canonical ID for consumers, plus
 * a private heap reference while sorting. Finalized heaps follow sorted row
 * order, so spilling rows does not turn their payload into dictionary probes. */
struct run_text {
    struct run_text *next;
    unsigned char *data;
    uint64_t *blocks;
    unsigned bytes,capacity,nblocks,loaded,loaded_next,sorted,wide;
    cs_work_context *owner;
};
struct row_run {
    unsigned char *data;
    cs_work_context *owner;
    uint64_t *blocks;
    unsigned nblocks;
    struct qs_cell *head;
    unsigned rows, position, loaded, cache_rows;
    int sorted, spilled;
    unsigned sort_phase,sort_i,sort_n;
    struct run_text *text;
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
    unsigned heap_count, pending_block, pending, cache_evictions;
    int compact_cache;
    struct qs_metrics metrics;
    struct qs_dict *dictionary; /* Optional native-Plan intermediate strings. */
    struct row_run compact_run;
    unsigned char *compact_output;
    unsigned compact_owner,compact_begin,compact_end,compact_free_run,compact_free_block;
    unsigned compact_rows,compact_block,compact_phase;
    int consolidated,heap_ready;
    struct qs_dictionary *paged_dictionary;
    struct qs_dictionary_access *text_left,*text_right,*text_evict;
    const char *compare_left,*compare_right;
    unsigned compare_length,compare_right_length,compare_key,compare_phase,comparing;
    unsigned sift_active,sift_root,sift_child,sift_phase;
    unsigned merge_active,merge_root,merge_child,merge_phase;
    unsigned heap_build,heap_initializing,combine_owner,combine_run,combine_started;
    unsigned top_phase,top_child,sink_active,source_active,dictionary_io;
    uint64_t dictionary_completion;
    void *io;
    unsigned text_active,text_row,text_column,text_columns;
    struct run_text *texts,*pending_text;
    unsigned text_block,output_column;
    char *text_compare;
};
/* Private executor reserve: one row/text run, radix scratch, a 32-way merge
 * window and control buffers. Reserve the minimum working set, not the
 * sort's maximum quota; resident runs remain reclaimable by row_evict(). */
size_t qs_sort_min_workspace(void)
{
    return 3*RUN_BYTES+32*(QS_SORT_IO_BYTES+sizeof(struct row_run)+QS_OUT*sizeof(struct qs_cell))+
        sizeof(struct row_sort)+QS_EXPRS*sizeof(struct qs_vector)+2*sizeof(struct qs_result)+4096;
}
enum { ROW_NONE, ROW_STORE, ROW_LOAD, ROW_FREE, ROW_COMPACT_STORE,
       ROW_TEXT_STORE, ROW_TEXT_LOAD, ROW_TEXT_FREE };
enum { DICTIONARY_NONE, DICTIONARY_COMPARE, DICTIONARY_EVICT, DICTIONARY_COPY };
/* Run cells keep the ID and a query-local heap location in value. These
 * private references are restored to IDs before any consumer sees the row. */
#define TEXT_RUN_TAG 253U
static struct run_text *cell_text(const struct qs_cell *v)
{ return (void *)(uintptr_t)((unsigned __int128)v->value>>64); }
static unsigned cell_text_offset(const struct qs_cell *v)
{ return (uint32_t)((unsigned __int128)v->value>>32); }
static void set_run_text(struct qs_cell *v,struct run_text *h,unsigned offset)
{
    v->value=(unsigned __int128)(uintptr_t)h<<64 | (uint64_t)offset<<32 | (uint32_t)v->value;
    v->scale=0; v->dscale=TEXT_RUN_TAG;
}
static int64_t row_text_get(struct row_sort *,const struct qs_cell *,const char **,unsigned *,struct femu_csd_args *);
static int text_output(const struct qs_plan *p,unsigned col)
{
    unsigned oid=p->outputs[col].type_oid;
    return oid==25 || oid==1042 || oid==1043;
}
static void restore_text_ids(const struct row_sort *s,struct qs_cell *row)
{
    if(!s->text_columns) return;
    for(unsigned col=0;col<s->plan.nout;++col) {
        struct qs_cell *v=&row[col];
        if(text_output(&s->plan,col) && !v->isnull &&
           v->dscale==TEXT_RUN_TAG) {
            v->value=(uint32_t)v->value; v->scale=v->dscale=0;
        }
    }
}
static int64_t row_evict(struct row_sort *,cs_work_context *,struct femu_csd_args *);
static int64_t row_compare(struct row_sort *s,const struct qs_cell *a,const struct qs_cell *b,
    int *result,struct femu_csd_args *args)
{
    const struct qs_plan *p=&s->plan;
    for(unsigned i=s->compare_key;i<p->norder;++i) {
        const struct qs_order *key=&p->order[i]; struct qs_cell x=a[key->output],y=b[key->output]; int cmp=0;
        if(x.isnull || y.isnull) {
            cmp=(x.isnull?1:0)-(y.isnull?1:0); if(key->nulls_first) cmp=-cmp;
        } else {
            unsigned oid=p->outputs[key->output].type_oid;
            if((s->dictionary || s->paged_dictionary) && (oid==25 || oid==1042 || oid==1043)) {
                const char *left,*right; unsigned ln,rn;
                if(x.dscale==TEXT_RUN_TAG && y.dscale==TEXT_RUN_TAG) {
                    if((uint32_t)x.value!=(uint32_t)y.value) {
                        s->compare_key=i;
                        if(!s->compare_phase) {
                            int64_t status=row_text_get(s,&x,&left,&ln,args); if(status) return status;
                            memcpy(s->text_compare,left,ln); s->compare_length=ln; s->compare_phase=1;
                        }
                        int64_t status=row_text_get(s,&y,&right,&rn,args); if(status) return status;
                        left=s->text_compare; ln=s->compare_length;
                        cmp=memcmp(left,right,ln<rn?ln:rn); if(!cmp) cmp=(ln>rn)-(ln<rn);
                        s->compare_phase=0;
                    }
                } else if(s->paged_dictionary && (uint32_t)x.value!=(uint32_t)y.value) {
                    s->compare_key=i; s->comparing=1;
                    while(s->compare_phase<2) {
                        struct qs_dictionary_access *access=s->compare_phase?s->text_right:s->text_left;
                        uint32_t id=(uint32_t)(s->compare_phase?y.value:x.value);
                        int64_t status=qs_dictionary_pin(s->paged_dictionary,access,id,&right,&rn,
                            s->io,s->dictionary_completion);
                        s->dictionary_completion=0;
                        if(status==-4) {
                            /* The dictionary already evicts its own cache.
                             * Also release row caches/runs under real shared
                             * budget pressure, retaining the pending pin. */
                            status=row_evict(s,s->context,args);
                            if(!status) continue;
                        } else if(status>QS_DICTIONARY_BUSY) s->dictionary_io=DICTIONARY_COMPARE;
                        if(status) return status;
                        if(!s->compare_phase) { s->compare_left=right; s->compare_length=rn; }
                        else { s->compare_right=right; s->compare_right_length=rn; }
                        ++s->compare_phase;
                    }
                    left=s->compare_left; ln=s->compare_length;
                    right=s->compare_right; rn=s->compare_right_length;
                    cmp=memcmp(left,right,ln<rn?ln:rn); if(!cmp) cmp=(ln>rn)-(ln<rn);
                    qs_dictionary_unpin(s->text_left); qs_dictionary_unpin(s->text_right);
                    s->compare_phase=s->comparing=0;
                } else if(!s->paged_dictionary) {
                    if(qs_dict_get(s->dictionary,(unsigned)x.value,&left,&ln) ||
                       qs_dict_get(s->dictionary,(unsigned)y.value,&right,&rn)) return -3;
                    cmp=memcmp(left,right,ln<rn?ln:rn); if(!cmp) cmp=(ln>rn)-(ln<rn);
                }
            } else if(qs_compare(x,y,&cmp)) return -3;
            if(key->descending) cmp=-cmp;
        }
        if(cmp) { s->compare_key=0; *result=cmp; return 0; }
        s->compare_key=i+1;
    }
    s->compare_key=0; *result=0; return 0;
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
static int64_t row_sift(struct row_sort *s,struct row_run *r,unsigned size,unsigned root,struct femu_csd_args *a)
{
    if(!s->sift_active) { s->sift_active=1; s->sift_root=root; s->sift_phase=0; }
    for(;;) {
        root=s->sift_root;
        unsigned child=s->sift_phase?s->sift_child:root*2+1;
        if(child>=size) break;
        int cmp; int64_t status;
        if(!s->sift_phase) {
            if(child+1<size) {
                status=row_compare(s,run_row(s,r,child),run_row(s,r,child+1),&cmp,a); if(status) return status;
                if(cmp<0) ++child;
            }
            s->sift_child=child; s->sift_phase=1;
        }
        status=row_compare(s,run_row(s,r,root),run_row(s,r,child),&cmp,a); if(status) return status;
        if(cmp>=0) break;
        row_swap(s,r,root,child); s->sift_root=child; s->sift_phase=0;
    }
    s->sift_active=0; return 0;
}
static uint64_t integer_sort_key(const unsigned char *row,unsigned column,int descending)
{
    uint64_t key=(uint64_t)(int64_t)((const struct qs_cell *)row)[column].value^(UINT64_C(1)<<63);
    return descending?~key:key;
}
/* Normalize a single integer/date key and skip constant bytes, as DuckDB's
 * sort keys do. Scratch is charged to the run owner; use the existing sorter
 * when the key is nullable, wide, composite or scratch does not fit. */
static int run_integer_radix_sort(struct row_sort *s,struct row_run *r)
{
    if(s->plan.norder!=1 || r->rows<2) return 0;
    const struct qs_order *order=&s->plan.order[0]; unsigned column=order->output;
    unsigned oid=s->plan.outputs[column].type_oid;
    if(oid!=20 && oid!=21 && oid!=23 && oid!=1082) return 0;
    uint64_t first=integer_sort_key(r->data,column,order->descending),vary=0;
    for(unsigned i=0;i<r->rows;++i) {
        const struct qs_cell *v=&run_row(s,r,i)[column];
        if(v->isnull || v->scale || v->dscale==UINT8_MAX || v->value<INT64_MIN || v->value>INT64_MAX) return 0;
        vary|=first^integer_sort_key(r->data+(size_t)i*s->row_bytes,column,order->descending);
    }
    if(!vary) return 1;
    size_t bytes=(size_t)r->rows*s->row_bytes;
    unsigned char *scratch=cs_work_alloc(r->owner,bytes); if(!scratch) return 0;
    unsigned char *from=r->data,*to=scratch;
    for(unsigned shift=0;shift<64;shift+=8) if((vary>>shift)&255) {
        unsigned positions[256]={0};
        for(unsigned i=0;i<r->rows;++i) ++positions[(integer_sort_key(from+(size_t)i*s->row_bytes,column,order->descending)>>shift)&255];
        unsigned start=0;
        for(unsigned i=0;i<256;++i) { unsigned count=positions[i]; positions[i]=start; start+=count; }
        for(unsigned i=0;i<r->rows;++i) {
            const unsigned char *row=from+(size_t)i*s->row_bytes;
            unsigned bucket=(integer_sort_key(row,column,order->descending)>>shift)&255;
            memcpy(to+(size_t)positions[bucket]++*s->row_bytes,row,s->row_bytes);
        }
        unsigned char *swap=from; from=to; to=swap;
    }
    if(from!=r->data) memcpy(r->data,from,bytes);
    cs_work_free(r->owner,scratch); return 1;
}
static unsigned __int128 numeric_sort_key(const unsigned char *row,unsigned column,int descending)
{
    unsigned __int128 key=(unsigned __int128)((const struct qs_cell *)row)[column].value^((unsigned __int128)1<<127);
    return descending?~key:key;
}
/* Normalize integer/date and uniform-scale numeric keys, as DuckDB's
 * sort keys do. Scratch is charged to the run owner; use the existing sorter
 * when a key is nullable, has mixed scales or scratch does not fit. */
static int run_radix_sort(struct row_sort *s,struct row_run *r)
{
    /* Preserve the cheaper 64-bit path for the common single integer key. */
    if(s->plan.norder==1 && s->plan.outputs[s->plan.order[0].output].type_oid!=1700)
        return run_integer_radix_sort(s,r);
    if(!s->plan.norder || r->rows<2) return 0;
    unsigned __int128 vary[QS_OUT]={0},changed=0;
    for(unsigned k=0;k<s->plan.norder;++k) {
        const struct qs_order *order=&s->plan.order[k]; unsigned column=order->output;
        unsigned oid=s->plan.outputs[column].type_oid;
        if(oid!=20 && oid!=21 && oid!=23 && oid!=1082 && oid!=1700) return 0;
        unsigned scale=run_row(s,r,0)[column].scale;
        unsigned __int128 first=numeric_sort_key(r->data,column,order->descending);
        for(unsigned i=0;i<r->rows;++i) {
            const struct qs_cell *v=&run_row(s,r,i)[column];
            if(v->isnull || v->scale!=scale || v->dscale==UINT8_MAX) return 0;
            vary[k]|=first^numeric_sort_key(r->data+(size_t)i*s->row_bytes,column,order->descending);
        }
        changed|=vary[k];
    }
    if(!changed) return 1;
    size_t bytes=(size_t)r->rows*s->row_bytes;
    unsigned char *scratch=cs_work_alloc(r->owner,bytes); if(!scratch) return 0;
    unsigned char *from=r->data,*to=scratch;
    /* Stable passes from the last key preserve lexicographic ordering. */
    for(unsigned k=s->plan.norder;k;--k) {
      const struct qs_order *order=&s->plan.order[k-1]; unsigned column=order->output;
      for(unsigned shift=0;shift<128;shift+=8) if((vary[k-1]>>shift)&255) {
        unsigned positions[256]={0};
        for(unsigned i=0;i<r->rows;++i) ++positions[(numeric_sort_key(from+(size_t)i*s->row_bytes,column,order->descending)>>shift)&255];
        unsigned start=0;
        for(unsigned i=0;i<256;++i) { unsigned count=positions[i]; positions[i]=start; start+=count; }
        for(unsigned i=0;i<r->rows;++i) {
            const unsigned char *row=from+(size_t)i*s->row_bytes;
            unsigned bucket=(numeric_sort_key(row,column,order->descending)>>shift)&255;
            memcpy(to+(size_t)positions[bucket]++*s->row_bytes,row,s->row_bytes);
        }
        unsigned char *swap=from; from=to; to=swap;
      }
    }
    if(from!=r->data) memcpy(r->data,from,bytes);
    cs_work_free(r->owner,scratch); return 1;
}
static int64_t run_sort(struct row_sort *s,struct row_run *r,struct femu_csd_args *a)
{
    if(r->sorted) return 0;
    if(!r->sort_phase) {
        if(run_radix_sort(s,r)) r->sort_phase=4;
        else { r->sort_i=r->rows/2; r->sort_phase=1; }
    }
    if(r->sort_phase==1) {
        while(r->sort_i) {
            int64_t status=row_sift(s,r,r->rows,r->sort_i-1,a); if(status) return status;
            --r->sort_i;
        }
        r->sort_n=r->rows; r->sort_phase=2;
    }
    while(r->sort_phase==2 || r->sort_phase==3) {
        if(r->sort_n<=1) break;
        if(r->sort_phase==2) { row_swap(s,r,0,r->sort_n-1); r->sort_phase=3; }
        int64_t status=row_sift(s,r,r->sort_n-1,0,a); if(status) return status;
        --r->sort_n; r->sort_phase=2;
    }
    if(r->text && r->text->bytes) {
        struct run_text *h=r->text;
        /* Keep records within blocks except the single maximum-length value.
         * Include identical padding in the capacity estimate and copy pass. */
        unsigned bytes=0;
        for(unsigned i=0;i<r->rows;++i) for(unsigned c=0;c<s->plan.nout;++c) {
            struct qs_cell *v=&run_row(s,r,i)[c];
            if(v->isnull || v->dscale!=TEXT_RUN_TAG) continue;
            uint16_t n; memcpy(&n,h->data+cell_text_offset(v),sizeof(n));
            if(bytes%QS_SORT_IO_BYTES+sizeof(n)+n>QS_SORT_IO_BYTES) bytes=(bytes+QS_SORT_IO_BYTES-1)/QS_SORT_IO_BYTES*QS_SORT_IO_BYTES;
            bytes+=sizeof(n)+n;
        }
        unsigned char *ordered;
        while(!(ordered=cs_work_alloc(r->owner,bytes))) {
            int64_t status=row_evict(s,r->owner,a); if(status) return status;
        }
        unsigned at=0;
        for(unsigned i=0;i<r->rows;++i) for(unsigned c=0;c<s->plan.nout;++c) {
            struct qs_cell *v=&run_row(s,r,i)[c];
            if(v->isnull || v->dscale!=TEXT_RUN_TAG) continue;
            unsigned off=cell_text_offset(v); uint16_t n; memcpy(&n,h->data+off,sizeof(n));
            if(at%QS_SORT_IO_BYTES+sizeof(n)+n>QS_SORT_IO_BYTES) {
                unsigned next=(at+QS_SORT_IO_BYTES-1)/QS_SORT_IO_BYTES*QS_SORT_IO_BYTES;
                memset(ordered+at,0,next-at); at=next;
            }
            memcpy(ordered+at,h->data+off,sizeof(n)+n); set_run_text(v,h,at); at+=sizeof(n)+n;
        }
        cs_work_free(r->owner,h->data); h->data=ordered; h->bytes=h->capacity=bytes; h->sorted=1;
    }
    r->sorted=1;
    if(r->rows) memcpy(r->head,r->data,s->row_bytes);
    return 0;
}
static int64_t row_store(struct row_sort *s,struct row_run *r,struct femu_csd_args *a)
{
    s->pending_run=r; s->pending=ROW_STORE;
    if(r->text && !r->text->blocks && r->text->bytes) {
        struct run_text *h=r->text;
        h->nblocks=(h->bytes+QS_SORT_IO_BYTES-1)/QS_SORT_IO_BYTES;
        h->blocks=cs_work_calloc(h->owner,h->nblocks,sizeof(*h->blocks));
        if(!h->blocks) return -4;
        s->pending_text=h; s->text_block=0; s->pending=ROW_TEXT_STORE;
    }
    if(s->pending==ROW_TEXT_STORE) {
        struct run_text *h=s->pending_text;
        unsigned at=s->text_block*QS_SORT_IO_BYTES,n=h->bytes-at;
        if(n>QS_SORT_IO_BYTES) n=QS_SORT_IO_BYTES;
        memset(a->mr_addr[a->numr-1],0,QS_SORT_IO_BYTES);
        memcpy(a->mr_addr[a->numr-1],h->data+at,n);
        return csNeedStore(QS_SORT_IO_BYTES);
    }
    unsigned first=s->pending_block*s->block_rows,n=r->rows-first;
    if(n>s->block_rows) n=s->block_rows;
    void *io=a->mr_addr[a->numr-1]; memset(io,0,QS_SORT_IO_BYTES);
    memcpy(io,r->data+(size_t)first*s->row_bytes,(size_t)n*s->row_bytes);
    return csNeedStore(QS_SORT_IO_BYTES);
}
/* Sequential runs benefit from full-block caching. Only shrink windows when
 * repeated premature evictions show that interleaved runs are thrashing. */
static void compact_cache(struct row_sort *o)
{
    unsigned spilled=0;
    for(unsigned j=0;j<o->count;++j) {
        struct row_run *r=&o->runs[j];
        if(!r->spilled) continue;
        ++spilled;
        if(r->data) { cs_work_free(o->context,r->data); r->data=NULL; }
        r->loaded=UINT32_MAX;
    }
    struct cs_work_context_stats memory;
    if(cs_work_get_context_stats(o->context,&memory)) return;
    size_t bytes=(memory.limit_bytes-memory.used_bytes)/(spilled+1);
    bytes=bytes>128?bytes-128:0; /* Allocation metadata/alignment headroom. */
    unsigned rows=bytes/o->row_bytes;
    if(!rows) rows=1;
    if(rows>o->block_rows) rows=o->block_rows;
    for(unsigned j=0;j<o->count;++j) o->runs[j].cache_rows=rows;
    o->compact_cache=1;
}
static int64_t row_evict(struct row_sort *s,cs_work_context *owner,struct femu_csd_args *a)
{
    /* During merge, resident runs can occupy almost the entire quota. Spill
     * one of them before evicting an input cache; otherwise every next row
     * can reload 64 KiB while resident runs keep the available RAM pinned. */
    if(s->combined) for(unsigned i=0;i<s->owners_count;++i) {
        struct row_sort *o=s->owners[i];
        for(unsigned j=0;j<o->count;++j) {
            struct row_run *r=&o->runs[j];
            if(r->owner==owner && r->data && !r->spilled && r->sorted && r->rows) {
                s->pending_block=0; return row_store(s,r,a);
            }
        }
    }
    /* Once all sinks are done, dictionary cache RAM is reusable. Keep the
     * sequential run windows instead of repeatedly evicting their heads. */
    if(s->combined && s->text_columns && s->paged_dictionary && !s->comparing) {
        int64_t status=qs_dictionary_evict(s->paged_dictionary,s->text_evict,a->mr_addr[a->numr-1],0);
        if(status>0) s->dictionary_io=DICTIONARY_EVICT;
        if(status!=-4) return status;
    }
    for(unsigned i=0;i<s->owners_count;++i)
        for(struct run_text *h=s->owners[i]->texts;h;h=h->next)
            if(h->owner==owner && h->blocks && h->data) {
                cs_work_free(owner,h->data); h->data=NULL; h->loaded=h->loaded_next=UINT32_MAX;
                return 0;
            }
    /* A spilled run's read buffer is only a cache. Its head is retained in
     * metadata, so release the buffer before writing another resident run. */
    for(unsigned i=0;i<s->owners_count;++i) {
        struct row_sort *o=s->owners[i];
        for(unsigned j=0;j<o->count;++j) {
            struct row_run *r=&o->runs[j];
            if(r->owner==owner && r->spilled && r->data) {
                int premature=r->loaded!=UINT32_MAX && r->position<r->loaded+r->cache_rows;
                cs_work_free(owner,r->data); r->data=NULL; r->loaded=UINT32_MAX;
                if(premature && !o->compact_cache && ++o->cache_evictions>=2*o->count) compact_cache(o);
                return 0;
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
    for(unsigned i=0;i<s->owners_count;++i)
        for(struct run_text *h=s->owners[i]->texts;h;h=h->next)
            if(h->owner==owner && h->sorted && !h->blocks && h->data && h->bytes) {
                h->nblocks=(h->bytes+QS_SORT_IO_BYTES-1)/QS_SORT_IO_BYTES;
                h->blocks=cs_work_calloc(owner,h->nblocks,sizeof(*h->blocks));
                if(!h->blocks) continue;
                s->pending_run=NULL; s->pending_text=h; s->text_block=0; s->pending=ROW_TEXT_STORE;
                unsigned n=h->bytes<QS_SORT_IO_BYTES?h->bytes:QS_SORT_IO_BYTES;
                memset(a->mr_addr[a->numr-1],0,QS_SORT_IO_BYTES); memcpy(a->mr_addr[a->numr-1],h->data,n);
                return csNeedStore(QS_SORT_IO_BYTES);
            }
    if(s->paged_dictionary && !s->comparing) {
        int64_t status=qs_dictionary_evict(s->paged_dictionary,s->text_evict,
            a->mr_addr[a->numr-1],0);
        if(status>0) s->dictionary_io=DICTIONARY_EVICT;
        return status;
    }
    return -4;
}
static int64_t row_resume(struct row_sort *s,struct femu_csd_args *a)
{
    s->io=a->mr_addr[a->numr-1];
    if(s->dictionary_io==DICTIONARY_EVICT) {
        int64_t status=qs_dictionary_evict(s->paged_dictionary,s->text_evict,s->io,
            csIsReentry(a)?csReentryValue(a):0);
        if(!status) s->dictionary_io=DICTIONARY_NONE;
        return status;
    }
    if(s->dictionary_io==DICTIONARY_COMPARE || s->dictionary_io==DICTIONARY_COPY) {
        if(!csIsReentry(a)) return -1;
        s->dictionary_completion=csReentryValue(a); s->dictionary_io=DICTIONARY_NONE;
        return 0;
    }
    if(!csIsReentry(a)) return 0;
    if(s->pending==ROW_TEXT_LOAD) {
        struct run_text *h=s->pending_text;
        unsigned base=s->text_block==h->loaded?0:QS_SORT_IO_BYTES;
        memcpy(h->data+base,s->io,QS_SORT_IO_BYTES);
        if(base) h->loaded_next=s->text_block;
        s->metrics.spill_read_bytes+=QS_SORT_IO_BYTES;
        s->pending=ROW_NONE; return 0;
    }
    if(s->pending==ROW_TEXT_FREE) {
        s->pending_text->blocks[s->text_block]=0; s->pending=ROW_NONE; return 0;
    }
    struct row_run *r=s->pending_run;
    if(s->pending==ROW_TEXT_STORE) {
        struct run_text *h=s->pending_text;
        h->blocks[s->text_block++]=csReentryValue(a);
        s->metrics.spill_write_bytes+=QS_SORT_IO_BYTES;
        if(s->text_block<h->nblocks) {
            unsigned at=s->text_block*QS_SORT_IO_BYTES,n=h->bytes-at;
            if(n>QS_SORT_IO_BYTES) n=QS_SORT_IO_BYTES;
            memset(s->io,0,QS_SORT_IO_BYTES); memcpy(s->io,h->data+at,n);
            return csNeedStore(QS_SORT_IO_BYTES);
        }
        cs_work_free(h->owner,h->data); h->data=NULL; h->loaded=h->loaded_next=UINT32_MAX;
        s->pending=ROW_NONE;
        s->pending_block=0; return r?row_store(s,r,a):0;
    }
    if(!r) return -1;
    if(s->pending==ROW_STORE) {
        if(s->pending_block>=r->nblocks) return -1;
        r->blocks[s->pending_block++]=csReentryValue(a);
        s->metrics.spill_write_bytes+=QS_SORT_IO_BYTES;
        if(s->pending_block*s->block_rows<r->rows) return row_store(s,r,a);
        cs_work_free(r->owner,r->data); r->data=NULL; r->spilled=1; r->loaded=UINT32_MAX;
        ++s->metrics.spilled_runs;
    } else if(s->pending==ROW_LOAD) {
        unsigned first=(r->position%s->block_rows/r->cache_rows)*r->cache_rows;
        unsigned count=s->block_rows-first; if(count>r->cache_rows) count=r->cache_rows;
        memcpy(r->data,(unsigned char *)a->mr_addr[a->numr-1]+(size_t)first*s->row_bytes,(size_t)count*s->row_bytes);
        r->loaded=s->pending_block*s->block_rows+first; s->metrics.spill_read_bytes+=QS_SORT_IO_BYTES;
    } else if(s->pending==ROW_FREE) r->blocks[s->pending_block]=0;
    else if(s->pending==ROW_COMPACT_STORE) {
        r->blocks[s->pending_block]=csReentryValue(a);
        ++s->compact_block; s->compact_rows=0; s->metrics.spill_write_bytes+=QS_SORT_IO_BYTES;
    }
    else return -1;
    s->pending=ROW_NONE; return 0;
}
static int64_t row_text_get(struct row_sort *s,const struct qs_cell *v,const char **bytes,
    unsigned *length,struct femu_csd_args *a)
{
    struct run_text *h=cell_text(v); unsigned offset=cell_text_offset(v),at=offset;
    if(!h || offset+sizeof(uint16_t)>h->bytes) return -3;
    if(h->blocks) {
        while(!h->data) {
            h->data=cs_work_alloc(h->owner,(h->wide?2U:1U)*QS_SORT_IO_BYTES);
            if(!h->data) { int64_t status=row_evict(s,h->owner,a); if(status) return status; }
        }
        unsigned block=offset/QS_SORT_IO_BYTES; at=offset%QS_SORT_IO_BYTES;
        if(h->loaded!=block) {
            h->loaded=block; h->loaded_next=UINT32_MAX;
            s->pending_text=h; s->text_block=block; s->pending=ROW_TEXT_LOAD;
            return csNeedLoad(h->blocks[block]);
        }
        uint16_t n; memcpy(&n,h->data+at,sizeof(n));
        if(at+sizeof(n)+n>QS_SORT_IO_BYTES && h->loaded_next!=block+1) {
            if(block+1>=h->nblocks) return -3;
            s->pending_text=h; s->text_block=block+1; s->pending=ROW_TEXT_LOAD;
            return csNeedLoad(h->blocks[block+1]);
        }
    }
    uint16_t n; memcpy(&n,h->data+at,sizeof(n));
    if((uint64_t)offset+sizeof(n)+n>h->bytes) return -3;
    *length=n; *bytes=(char *)h->data+at+sizeof(n); return 0;
}
/* Materialize all text columns once while the input batch is still live.
 * A dictionary miss retains the row/column cursor through reentry. */
static int64_t prepare_run_text(struct row_sort *s,struct femu_csd_args *a)
{
    if(!s->text_active) return 0;
    struct row_run *r=&s->runs[s->count-1];
    for(;s->text_row<r->rows;++s->text_row,s->text_column=0)
        for(;s->text_column<s->plan.nout;++s->text_column) {
            unsigned col=s->text_column;
            struct qs_cell *v=&run_row(s,r,s->text_row)[col];
            if(!text_output(&s->plan,col) || v->isnull || v->dscale==TEXT_RUN_TAG) continue;
            const char *bytes; unsigned length;
retry_text:
            s->comparing=1;
            int64_t status=qs_dictionary_pin(s->paged_dictionary,s->text_left,(uint32_t)v->value,
                &bytes,&length,s->io,s->dictionary_completion);
            s->dictionary_completion=0;
            if(status==-4) { status=row_evict(s,s->context,a); if(!status) goto retry_text; }
            else if(status>QS_DICTIONARY_BUSY) s->dictionary_io=DICTIONARY_COPY;
            if(status) return status;
            if(length>UINT16_MAX) return -3;
            if(!r->text) {
                r->text=cs_work_calloc(r->owner,1,sizeof(*r->text));
                if(!r->text) { qs_dictionary_unpin(s->text_left); s->comparing=0; return -4; }
                r->text->owner=r->owner; r->text->next=s->texts; s->texts=r->text;
            }
            struct run_text *h=r->text; unsigned need=h->bytes+sizeof(uint16_t)+length;
            while(need>h->capacity) {
                unsigned capacity=h->capacity?h->capacity*2:QS_SORT_IO_BYTES;
                if(capacity<need) capacity=need;
                void *data=cs_work_realloc(h->owner,h->data,capacity);
                if(!data) {
                    /* Release the pin before yielding. The same ID is pinned
                     * again on reentry; no borrowed pointer survives it. */
                    qs_dictionary_unpin(s->text_left); s->comparing=0;
                    status=row_evict(s,s->context,a); if(status) return status;
                    goto retry_text;
                }
                h->data=data; h->capacity=capacity;
            }
            h->wide|=length+sizeof(uint16_t)>QS_SORT_IO_BYTES;
            uint16_t n=length; memcpy(h->data+h->bytes,&n,sizeof(n));
            memcpy(h->data+h->bytes+sizeof(n),bytes,length);
            set_run_text(v,h,h->bytes); h->bytes=need;
            qs_dictionary_unpin(s->text_left); s->comparing=0;
        }
    s->text_active=s->text_row=s->text_column=0; return 0;
}
static int row_init(struct qs_task *task,const struct qs_plan *p,struct row_sort **out)
{
    unsigned workers=task->workers?task->workers:1;
    if(workers>QS_WORKERS) return -1;
    size_t limit=qs_sink_memory(p,workers); if(!limit) return -4;
    cs_work_context *context=p->work_mem_parent?cs_work_begin_child((void *)(uintptr_t)p->work_mem_parent,limit):cs_work_begin_limit(limit);
    if(!context) return -4;
    struct row_sort *s=cs_work_calloc(context,1,sizeof(*s));
    if(!s) { cs_work_end(context); return -4; }
    s->context=context; s->plan=*p; s->owners[0]=s; s->owners_count=1;
    s->row_bytes=p->nout*sizeof(struct qs_cell); s->block_rows=QS_SORT_IO_BYTES/s->row_bytes;
    unsigned bytes=RUN_BYTES; while(bytes>QS_SORT_IO_BYTES && bytes>limit/4) bytes/=2;
    /* Whole I/O blocks avoid an almost-empty extra block per full run. */
    s->run_rows=(bytes/QS_SORT_IO_BYTES)*s->block_rows;
    s->vectors=cs_work_alloc(context,p->nexprs*sizeof(*s->vectors));
    if(p->nexprs && !s->vectors) { cs_work_end(context); return -4; }
    for(unsigned i=0;i<p->nout && p->ordered_rows<2;++i)
        if(qs_vector_require(p,p->outputs[i].index,s->needed)) { cs_work_end(context); return -1; }
    if(p->limit>=0 && (uint64_t)p->limit<=UINT64_MAX-(uint64_t)p->offset) {
        s->topn=(uint64_t)p->limit+(uint64_t)p->offset;
        s->bounded=s->topn<=s->run_rows;
    }
    task->state=(uintptr_t)s; *out=s; return 0;
}
/* Allocate a run without consuming input. Parallel SQL pipelines use this to
 * establish each local sink before any worker starts producing rows. */
static int row_reserve(struct row_sort *s)
{
    if(s->count==s->capacity) {
        unsigned capacity=s->capacity?s->capacity*2:8;
        void *runs=cs_work_realloc(s->context,s->runs,capacity*sizeof(*s->runs));
        if(!runs) return -4;
        s->runs=runs; s->capacity=capacity;
    }
    unsigned n=s->bounded?s->topn:s->run_rows;
    struct cs_work_context_stats memory;
    if(cs_work_get_context_stats(s->context,&memory)) return -4;
    size_t available=memory.limit_bytes-memory.used_bytes;
    if(s->plan.work_mem_parent) {
        struct cs_work_context_stats parent;
        if(cs_work_get_context_stats((void *)(uintptr_t)s->plan.work_mem_parent,&parent)) return -4;
        size_t free=parent.limit_bytes-parent.used_bytes;
        if(free<available) available=free;
    }
    struct cs_work_stats pool;
    if(cs_work_get_stats(&pool)) return -4;
    if(pool.free_bytes<available) available=pool.free_bytes;
    /* Keep one run of scratch for radix sorting instead of filling the quota
     * with resident runs and silently falling back to comparison sort. */
    size_t bytes=(size_t)n*s->row_bytes,scratch=s->bounded || !s->plan.norder?0:bytes;
    for(unsigned k=0;k<s->plan.norder;++k) {
        unsigned oid=s->plan.outputs[s->plan.order[k].output].type_oid;
        if(oid!=20 && oid!=21 && oid!=23 && oid!=1082 && oid!=1700) scratch=0;
    }
    if(available<bytes+scratch+QS_SORT_IO_BYTES+4096) return -4;
    unsigned blocks=(n+s->block_rows-1)/s->block_rows;
    size_t token_bytes=(blocks*sizeof(uint64_t)+_Alignof(struct qs_cell)-1)&~(size_t)(_Alignof(struct qs_cell)-1);
    uint64_t *tokens=cs_work_calloc(s->context,1,token_bytes+s->row_bytes);
    if(!tokens) return -4;
    void *data=cs_work_alloc(s->context,(size_t)n*s->row_bytes);
    if(!data) { cs_work_free(s->context,tokens); return -4; }
    struct row_run *r=&s->runs[s->count++];
    *r=(struct row_run){.data=data,.owner=s->context,.blocks=tokens,.nblocks=blocks,
        .head=(void *)((char *)tokens+token_bytes)};
    return 0;
}
/* Private SQL composition helpers; neither is a CSF/device command. */
int qs_sort_prepare(uint64_t state)
{ return row_reserve((void *)(uintptr_t)state); }
int64_t qs_sort_finish_sink(uint64_t state,struct femu_csd_args *a)
{
    struct row_sort *s=(void *)(uintptr_t)state;
    int64_t status=row_resume(s,a); if(status) return status;
    status=prepare_run_text(s,a); if(status) return status;
    for(unsigned i=0;i<s->count;++i) {
        struct row_run *r=&s->runs[i];
        if(r->rows) { status=run_sort(s,r,a); if(status) return status; }
        else { cs_work_free(r->owner,r->data); r->data=NULL; r->sorted=1; }
    }
    cs_work_free(s->context,s->vectors); s->vectors=NULL;
    return 0;
}
/* chunk, plan, task(handle output), spill I/O. Reentry resumes this input chunk. */
static int64_t row_sink(struct femu_csd_args *a,const struct qs_plan *columns,
    const struct qs_vector *vectors,unsigned begin,unsigned end)
{
    if(!a || a->numr!=4 || a->mr_len[1]<(long long)sizeof(struct qs_plan) ||
        a->mr_len[2]<(long long)sizeof(struct qs_task) || a->mr_len[3]<QS_SORT_IO_BYTES) return -1;
    struct qs_chunk *c=a->mr_addr[0]; struct qs_result *rows=a->mr_addr[0];
    const struct qs_plan *p=a->mr_addr[1]; struct qs_task *task=a->mr_addr[2];
    /* 3 is the SQL executor's packed CU batch; begin retains a partially
     * consumed producer position. Both layouts contain evaluated cells. */
    int packed=p->ordered_rows==3,row_input=p->ordered_rows==2 || packed;
    if(!p->nout || p->nout>QS_OUT || p->nexprs>QS_EXPRS || p->norder>QS_OUT ||
        (!columns && (row_input?(a->mr_len[0]<(long long)sizeof(*rows) ||
          rows->rows>(packed?QS_RESULT_ROWS*QS_OUT/p->nout:QS_RESULT_ROWS) || (packed && rows->begin>rows->rows)):
         (a->mr_len[0]<(long long)sizeof(*c) || c->selected>c->rows || c->rows>c->capacity ||
          qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0])))) return -1;
    if(columns && (columns->nout!=p->nout || begin>end || end>QS_VECTOR_ROWS)) return -1;
    unsigned selected=columns?end-begin:row_input?rows->rows-(packed?rows->begin:0):c->selected;
    struct row_sort *s=(void *)(uintptr_t)task->state;
    if(!s) { int status=row_init(task,p,&s); if(status) return status; }
    if(!s->sink_active) {
        s->sink_active=1;
        s->input_position=0; s->metrics.scanned+=columns || row_input?selected:c->rows; s->metrics.matched+=selected;
        s->metrics.input_bytes+=task->input_bytes; ++s->metrics.chunks;
    }
    int64_t status=row_resume(s,a); if(status) return status;
    status=prepare_run_text(s,a); if(status) return status;
    if(!s->bounded && s->count) {
        struct row_run *last=&s->runs[s->count-1];
        if(last->rows==s->run_rows || (last->text && last->text->bytes>=RUN_BYTES)) {
            status=run_sort(s,last,a); if(status) return status;
        }
    }
    while(s->input_position<selected) {
        if(s->bounded && !s->topn) { s->input_position=selected; break; }
        struct row_run *r=s->count?&s->runs[s->count-1]:NULL;
        if(!r || (!s->bounded && r->sorted)) {
            /* Budget for the next run, not a fixed free-memory percentage.
             * Leave one I/O block plus allocation overhead for merge/metadata.
             * Allocation failure still handles fragmentation/global pressure. */
            if(row_reserve(s)) {
                status=row_evict(s,s->context,a); if(status) return status;
                continue; /* Reclaimed cache is progress, not consumed input. */
            }
            r=&s->runs[s->count-1];
        }
        unsigned count=selected-s->input_position;
        if(count>QS_VECTOR_ROWS) count=QS_VECTOR_ROWS;
        if(s->text_columns && count>64) count=64;
        if(!s->bounded && count>s->run_rows-r->rows) count=s->run_rows-r->rows;
        if(!columns && !row_input && qs_vector_execute(p,c,qs_selection(c),s->input_position,count,s->needed,s->vectors)) return -5;
        if(columns && !s->bounded) {
            for(unsigned col=0;col<p->nout;++col) {
                unsigned expr=columns->outputs[col].index;
                for(unsigned i=0;i<count;++i) {
                    unsigned at=begin+s->input_position+i;
                    if(vectors[expr].errors[at]) return -5;
                    run_row(s,r,r->rows+i)[col]=qs_vector_cell(columns,vectors,expr,at);
                }
            }
            r->rows+=count;
        } else if(row_input && !s->bounded) {
            if(packed) memcpy(run_row(s,r,r->rows),(struct qs_cell *)rows->data+
                (size_t)(rows->begin+s->input_position)*p->nout,(size_t)count*s->row_bytes);
            else for(unsigned i=0;i<count;++i)
                memcpy(run_row(s,r,r->rows+i),rows->data[s->input_position+i].cells,s->row_bytes);
            r->rows+=count;
        } else {
            unsigned position=s->input_position;
            for(unsigned i=0;i<count;++i) {
                struct qs_cell row[QS_OUT];
                if(columns) {
                    for(unsigned col=0;col<p->nout;++col) {
                        unsigned expr=columns->outputs[col].index,at=begin+position+i;
                        if(vectors[expr].errors[at]) return -5;
                        row[col]=qs_vector_cell(columns,vectors,expr,at);
                    }
                } else if(row_input) memcpy(row,packed?(struct qs_cell *)rows->data+
                    (size_t)(rows->begin+position+i)*p->nout:rows->data[position+i].cells,s->row_bytes);
                else for(unsigned col=0;col<p->nout;++col) {
                    unsigned expr=p->outputs[col].index;
                    if(s->vectors[expr].errors[i]) return -5;
                    row[col]=qs_vector_cell(p,s->vectors,expr,i);
                }
                if(s->bounded) {
                    if(!s->top_phase) {
                        if(r->rows==s->topn) {
                            int cmp;
                            status=row_compare(s,row,run_row(s,r,0),&cmp,a); if(status) return status;
                            if(cmp<0) { memcpy(run_row(s,r,0),row,s->row_bytes); s->top_phase=1; }
                        } else {
                            memcpy(run_row(s,r,r->rows++),row,s->row_bytes);
                            s->top_child=r->rows-1; s->top_phase=2;
                        }
                    }
                    if(s->top_phase==1) { status=row_sift(s,r,r->rows,0,a); if(status) return status; }
                    else if(s->top_phase==2) {
                        while(s->top_child) {
                            unsigned child=s->top_child; int cmp;
                            status=row_compare(s,run_row(s,r,(child-1)/2),run_row(s,r,child),&cmp,a); if(status) return status;
                            if(cmp>=0) break;
                            row_swap(s,r,child,(child-1)/2); s->top_child=(child-1)/2;
                        }
                    }
                    s->top_phase=0; ++s->input_position;
                } else {
                    memcpy(run_row(s,r,r->rows++),row,s->row_bytes);
                }
            }
        }
        if(!s->bounded) {
            s->input_position+=count;
            if(s->text_columns) {
                s->text_active=1; s->text_row=r->rows-count; s->text_column=0;
                status=prepare_run_text(s,a); if(status) return status;
            }
        }
        if(!s->bounded && (r->rows==s->run_rows || (r->text && r->text->bytes>=RUN_BYTES))) {
            status=run_sort(s,r,a); if(status) return status;
        }
    }
    s->sink_active=0; return 0;
}
int64_t pg_sina_sort_sink(struct femu_csd_args *a)
{ return row_sink(a,NULL,NULL,0,0); }
/* Private same-SO composition: already evaluated columns, not a new MR layout
 * or CSF entry. The producer retains these vectors through spill reentry. */
int64_t qs_sort_sink_columns(struct femu_csd_args *a,const struct qs_plan *columns,
    const struct qs_vector *vectors,unsigned begin,unsigned end)
{ return row_sink(a,columns,vectors,begin,end); }
static int64_t heap_compare(struct row_sort *s,unsigned left,unsigned right,int *cmp,struct femu_csd_args *a)
{ return row_compare(s,s->heap[left]->head,s->heap[right]->head,cmp,a); }

/* Internal SQL operator composition; never exported through SNIA or FEMU. */
void qs_sort_dictionary(uint64_t state,struct qs_dict *dictionary)
{ ((struct row_sort *)(uintptr_t)state)->dictionary=dictionary; }
int qs_sort_paged_dictionary(uint64_t state,struct qs_dictionary *dictionary)
{
    struct row_sort *s=(void *)(uintptr_t)state;
    if(!s->text_left) s->text_left=qs_dictionary_access_create(dictionary);
    if(!s->text_right) s->text_right=qs_dictionary_access_create(dictionary);
    if(!s->text_evict) s->text_evict=qs_dictionary_access_create(dictionary);
    if(!s->text_left || !s->text_right || !s->text_evict) return -4;
    s->paged_dictionary=dictionary;
    for(unsigned col=0;!s->bounded && col<s->plan.nout;++col) {
        if(text_output(&s->plan,col)) s->text_columns|=1U<<col;
    }
    if(s->text_columns && !s->text_compare) {
        s->text_compare=cs_work_alloc(s->context,UINT16_MAX);
        if(!s->text_compare) return -4;
    }
    return 0;
}
static int64_t merge_sift(struct row_sort *s,unsigned root,struct femu_csd_args *a)
{
    if(!s->merge_active) { s->merge_active=1; s->merge_root=root; s->merge_phase=0; }
    for(;;) {
        root=s->merge_root;
        unsigned child=s->merge_phase?s->merge_child:root*2+1;
        if(child>=s->heap_count) break;
        int cmp; int64_t status;
        if(!s->merge_phase) {
            if(child+1<s->heap_count) {
                status=heap_compare(s,child+1,child,&cmp,a); if(status) return status;
                if(cmp<0) ++child;
            }
            s->merge_child=child; s->merge_phase=1;
        }
        status=heap_compare(s,root,child,&cmp,a); if(status) return status;
        if(cmp<=0) break;
        struct row_run *r=s->heap[root]; s->heap[root]=s->heap[child]; s->heap[child]=r;
        s->merge_root=child; s->merge_phase=0;
    }
    s->merge_active=0; return 0;
}
static int64_t row_combine(struct row_sort *s,struct qs_group *group,struct femu_csd_args *a,int sort_runs)
{
    if(!group->count || group->count>QS_WORKERS || group->states[0]!=(uintptr_t)s) return -1;
    if(!s->combined) {
        if(!s->combine_started) {
            s->owners_count=group->count;
            for(unsigned i=0;i<group->count;++i) {
                struct row_sort *o=(void *)(uintptr_t)group->states[i];
                if(!o) return -1;
                for(unsigned j=0;j<i;++j) if(s->owners[j]==o) return -1;
                s->owners[i]=o;
            }
            s->combine_started=1;
        }
        for(unsigned i=s->combine_owner;i<s->owners_count;++i) {
            struct row_sort *o=s->owners[i];
            if(o->vectors) { cs_work_free(o->context,o->vectors); o->vectors=NULL; }
            for(unsigned j=s->combine_run;j<o->count;++j) {
                if(sort_runs) { int64_t status=run_sort(s,&o->runs[j],a); if(status) return status; }
                s->combine_run=j+1;
            }
            if(i) {
                s->metrics.scanned+=o->metrics.scanned; s->metrics.matched+=o->metrics.matched;
                s->metrics.input_bytes+=o->metrics.input_bytes; s->metrics.chunks+=o->metrics.chunks;
                s->metrics.spill_write_bytes+=o->metrics.spill_write_bytes; s->metrics.spilled_runs+=o->metrics.spilled_runs;
            }
            s->metrics.runs+=o->count;
            s->combine_owner=i+1; s->combine_run=0;
        }
        for(unsigned i=0;i<s->owners_count;++i)
            for(unsigned j=0;j<s->owners[i]->count;++j) s->owners[i]->runs[j].cache_rows=s->block_rows;
        s->metrics.workers=group->count; s->combined=1;
    }
    return 0;
}
static int64_t merge_head(struct row_sort *s,struct row_run *r,struct femu_csd_args *a)
{
    if(r->spilled) {
        while(!r->data) {
            r->data=cs_work_alloc(r->owner,(size_t)r->cache_rows*s->row_bytes);
            if(!r->data) { int64_t status=row_evict(s,r->owner,a); if(status) return status; }
        }
        unsigned block=r->position/s->block_rows;
        unsigned window=block*s->block_rows+(r->position%s->block_rows/r->cache_rows)*r->cache_rows;
        if(r->loaded!=window) {
            s->pending_run=r; s->pending_block=block; s->pending=ROW_LOAD;
            return csNeedLoad(r->blocks[block]);
        }
        memcpy(r->head,r->data+(size_t)(r->position-r->loaded)*s->row_bytes,s->row_bytes);
    } else memcpy(r->head,r->data+(size_t)r->position*s->row_bytes,s->row_bytes);
    return 0;
}
/* Bound merge fan-in by the cache budget. Every input keeps a complete I/O
 * block; intermediate merged runs are written sequentially. This avoids
 * repeatedly loading 64 KiB for a handful of rows from hundreds of runs. */
static unsigned merge_fan_in(struct row_sort *o)
{
    struct cs_work_context_stats memory;
    if(cs_work_get_context_stats(o->context,&memory)) return 2;
    size_t limit=memory.limit_bytes,data=0,tokens=0;
    if(o->plan.work_mem_parent) {
        struct cs_work_context_stats parent;
        if(!cs_work_get_context_stats((void *)(uintptr_t)o->plan.work_mem_parent,&parent)) {
            size_t available=parent.limit_bytes-parent.used_bytes+memory.used_bytes;
            if(available<limit) limit=available;
        }
    }
    for(unsigned i=0;i<o->count;++i) {
        struct row_run *r=&o->runs[i];
        if(r->data) data+=(size_t)(r->spilled?r->cache_rows:o->bounded?o->topn:o->run_rows)*o->row_bytes;
        tokens+=((uint64_t)r->rows+o->block_rows-1)/o->block_rows*sizeof(uint64_t);
    }
    size_t metadata=memory.used_bytes>data?memory.used_bytes-data:0;
    size_t reserve=metadata+tokens+2*QS_SORT_IO_BYTES+4096;
    unsigned fan=limit>reserve?(limit-reserve)/QS_SORT_IO_BYTES:2;
    if(fan<2) fan=2;
    /* The available parent/child budget already bounds every input window.
     * A fixed fan-in adds full read/write passes even when more runs fit. */
    return fan;
}
static int64_t consolidate(struct row_sort *s,struct femu_csd_args *a)
{
    /* The normal output AFDM is unused until consolidation finishes. Use
     * its data area as the sequential STORE staging buffer; LOAD has its
     * separate I/O AFDM. No extra runtime-memory buffer is needed. */
    s->compact_output=(unsigned char *)((struct qs_result *)a->mr_addr[1])->data;
    while(s->compact_owner<s->owners_count) {
        struct row_sort *o=s->owners[s->compact_owner];
        if(!s->compact_phase) {
            unsigned spilled=0; for(unsigned i=0;i<o->count;++i) spilled+=o->runs[i].spilled;
            unsigned fan=merge_fan_in(o);
            if(!spilled || o->count<=fan) {
                ++s->compact_owner; s->compact_begin=0; continue;
            }
            if(s->compact_begin>=o->count) {
                unsigned count=0;
                for(unsigned i=0;i<o->count;++i) if(o->runs[i].rows) {
                    if(i!=count) { o->runs[count]=o->runs[i]; memset(&o->runs[i],0,sizeof(o->runs[i])); }
                    ++count;
                }
                o->count=count; o->compact_cache=0; o->cache_evictions=0;
                s->compact_begin=0; continue;
            }
            s->compact_end=s->compact_begin+fan;
            if(s->compact_end>o->count) s->compact_end=o->count;
            if(s->compact_end-s->compact_begin==1) { ++s->compact_begin; continue; }
            uint64_t rows=0;
            for(unsigned i=s->compact_begin;i<s->compact_end;++i) rows+=o->runs[i].rows;
            if(rows>UINT32_MAX) return -4;
            struct row_run *r=&s->compact_run;
            r->rows=rows; r->nblocks=(rows+s->block_rows-1)/s->block_rows; r->owner=o->context;
            size_t token_bytes=(r->nblocks*sizeof(uint64_t)+_Alignof(struct qs_cell)-1)&~(size_t)(_Alignof(struct qs_cell)-1);
            r->blocks=cs_work_calloc(o->context,1,token_bytes+s->row_bytes);
            if(!r->blocks) {
                int64_t status=row_evict(s,o->context,a); if(status) return status;
                continue;
            }
            r->spilled=r->sorted=1; r->loaded=UINT32_MAX; r->cache_rows=s->block_rows;
            r->head=(void *)((char *)r->blocks+token_bytes);
            s->heap_count=0; s->root_pending=0; s->compact_block=s->compact_rows=0;
            for(unsigned i=s->compact_begin;i<s->compact_end;++i) {
                struct row_run *input=&o->runs[i];
                if(input->spilled && input->data) { cs_work_free(o->context,input->data); input->data=NULL; input->loaded=UINT32_MAX; }
                input->cache_rows=s->block_rows;
                if(input->rows) s->heap[s->heap_count++]=input;
            }
            s->heap_build=s->heap_count/2; s->compact_phase=3;
        }
        if(s->compact_phase==3) {
            while(s->heap_build) {
                int64_t status=merge_sift(s,s->heap_build-1,a); if(status) return status;
                --s->heap_build;
            }
            s->compact_phase=1;
        }
        if(s->compact_phase==1) {
            while(s->heap_count || s->compact_rows) {
                if(s->root_pending==1) {
                    struct row_run *r=s->heap[0];
                    if(r->position==r->rows) {
                        if(r->data) { cs_work_free(r->owner,r->data); r->data=NULL; }
                        s->heap[0]=s->heap[--s->heap_count];
                    } else {
                        int64_t status=merge_head(s,r,a); if(status) return status;
                    }
                    s->root_pending=2;
                }
                if(s->root_pending==2) {
                    if(s->heap_count) { int64_t status=merge_sift(s,0,a); if(status) return status; }
                    s->root_pending=0;
                }
                if(s->compact_rows==s->block_rows || (!s->heap_count && s->compact_rows)) {
                    memset(a->mr_addr[a->numr-1],0,QS_SORT_IO_BYTES);
                    memcpy(a->mr_addr[a->numr-1],s->compact_output,(size_t)s->compact_rows*s->row_bytes);
                    s->pending_run=&s->compact_run; s->pending_block=s->compact_block; s->pending=ROW_COMPACT_STORE;
                    return csNeedStore(QS_SORT_IO_BYTES);
                }
                if(!s->heap_count) break;
                struct row_run *r=s->heap[0];
                if(!s->compact_block && !s->compact_rows) memcpy(s->compact_run.head,r->head,s->row_bytes);
                memcpy(s->compact_output+(size_t)s->compact_rows*s->row_bytes,r->head,s->row_bytes);
                ++s->compact_rows; ++r->position; s->root_pending=1;
            }
            s->compact_phase=2; s->compact_free_run=s->compact_begin; s->compact_free_block=0;
        }
        while(s->compact_free_run<s->compact_end) {
            struct row_run *r=&o->runs[s->compact_free_run];
            while(s->compact_free_block<r->nblocks) {
                unsigned block=s->compact_free_block;
                if(r->blocks[block]) {
                    s->pending_run=r; s->pending_block=block; s->pending=ROW_FREE;
                    return csNeedFree(r->blocks[block]);
                }
                ++s->compact_free_block;
            }
            if(r->data) cs_work_free(o->context,r->data);
            cs_work_free(o->context,r->blocks); memset(r,0,sizeof(*r));
            ++s->compact_free_run; s->compact_free_block=0;
        }
        o->runs[s->compact_begin]=s->compact_run; memset(&s->compact_run,0,sizeof(s->compact_run));
        ++s->metrics.spilled_runs; ++s->metrics.runs;
        s->compact_begin=s->compact_end; s->compact_phase=0;
    }
    s->consolidated=1; s->heap_count=0; s->root_pending=0;
    return 0;
}
static int64_t row_finish(struct row_sort *s,struct qs_group *group,struct qs_result *out)
{
    for(unsigned i=0;i<s->owners_count;++i)
        for(struct run_text *h=s->owners[i]->texts;h;h=h->next)
            for(unsigned b=0;h->blocks && b<h->nblocks;++b) if(h->blocks[b]) {
                s->pending_text=h; s->text_block=b; s->pending=ROW_TEXT_FREE;
                return csNeedFree(h->blocks[b]);
            }
    for(unsigned i=0;i<s->owners_count;++i) {
        struct row_sort *o=s->owners[i];
        for(unsigned j=0;j<o->count;++j) for(unsigned b=0;b<o->runs[j].nblocks;++b) if(o->runs[j].blocks[b]) {
            s->pending_run=&o->runs[j]; s->pending_block=b; s->pending=ROW_FREE;
            return csNeedFree(o->runs[j].blocks[b]);
        }
    }
    for(unsigned b=0;s->compact_run.blocks && b<s->compact_run.nblocks;++b) if(s->compact_run.blocks[b]) {
        s->pending_run=&s->compact_run; s->pending_block=b; s->pending=ROW_FREE;
        return csNeedFree(s->compact_run.blocks[b]);
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
static void unpin_text(struct row_sort *s)
{
    if(s->text_left) qs_dictionary_unpin(s->text_left);
    if(s->text_right) qs_dictionary_unpin(s->text_right);
}
/* Private Sort Source payload. Public cells still contain canonical IDs.
 * A small offset hash in the already existing string area lets the SQL
 * consumer reuse these bytes without pinning the global dictionary again. */
#define SOURCE_TEXT_SLOTS (QS_RESULT_ROWS*QS_OUT*2U)
#define SOURCE_TEXT_HEADER (SOURCE_TEXT_SLOTS*sizeof(uint32_t))
const char *qs_sort_output_text(const struct qs_result *out,uint32_t id,unsigned *length)
{
    if(out->str_bytes<SOURCE_TEXT_HEADER) return NULL;
    const uint32_t *slots=(const void *)out->str_data;
    unsigned slot=(id*2654435761U)&(SOURCE_TEXT_SLOTS-1);
    while(slots[slot]) {
        unsigned at=slots[slot]; uint32_t key; uint16_t n;
        memcpy(&key,out->str_data+at,sizeof(key));
        if(key==id) {
            memcpy(&n,out->str_data+at+sizeof(key),sizeof(n));
            *length=n; return out->str_data+at+sizeof(key)+sizeof(n);
        }
        slot=(slot+1)&(SOURCE_TEXT_SLOTS-1);
    }
    return NULL;
}
static int64_t source_text(struct row_sort *s,const struct qs_cell *row,struct qs_result *out,
    struct femu_csd_args *a)
{
    for(;s->output_column<s->plan.nout;++s->output_column) {
        const struct qs_cell *v=&row[s->output_column];
        if(v->isnull || v->dscale!=TEXT_RUN_TAG) continue;
        unsigned n; uint32_t id=(uint32_t)v->value;
        if(qs_sort_output_text(out,id,&n)) continue;
        const char *bytes; int64_t status=row_text_get(s,v,&bytes,&n,a); if(status) return status;
        if((uint64_t)out->str_bytes+sizeof(id)+sizeof(uint16_t)+n>QS_RESULT_STRING_BYTES) return out->rows?3:-4;
        unsigned slot=(id*2654435761U)&(SOURCE_TEXT_SLOTS-1); uint32_t *slots=(void *)out->str_data;
        while(slots[slot]) slot=(slot+1)&(SOURCE_TEXT_SLOTS-1);
        unsigned at=out->str_bytes; slots[slot]=at; uint16_t length=n;
        memcpy(out->str_data+at,&id,sizeof(id)); at+=sizeof(id);
        memcpy(out->str_data+at,&length,sizeof(length)); at+=sizeof(length);
        memcpy(out->str_data+at,bytes,n); out->str_bytes=at+n;
    }
    s->output_column=0; return 0;
}
/* group, output, spill I/O. Cleanup uses group, spill I/O and the same batch
 * reentry protocol, so aborts also release tokens before destroying workmem. */
int64_t pg_sina_sort_source(struct femu_csd_args *a)
{
    if(!a || (a->numr!=1 && a->numr!=2 && a->numr!=3) || a->mr_len[0]<(long long)sizeof(struct qs_group) ||
        (a->numr!=1 && a->mr_len[a->numr-1]<QS_SORT_IO_BYTES) ||
        (a->numr==3 && a->mr_len[1]<(long long)sizeof(struct qs_result))) return -1;
    struct qs_group *group=a->mr_addr[0];
    if(!group->count || group->count>QS_WORKERS) return -1;
    /* PG closes this query's SNIA device immediately afterwards. That releases
     * any surviving tokens, including tokens already rolled back by a failed
     * batch. Do not require a stale token to be freed twice to release RAM. */
    if(a->numr==1) {
        for(unsigned i=0;i<group->count;++i) if(group->states[i]) {
            struct row_sort *owner=(void *)(uintptr_t)group->states[i];
            unpin_text(owner);
            cs_work_end(owner->context); group->states[i]=0;
        }
        return 0;
    }
    struct row_sort *first=NULL;
    for(unsigned i=0;i<group->count && !first;++i) first=(void *)(uintptr_t)group->states[i];
    if(!csIsReentry(a) && (!first || !first->source_active)) {
        /* Dynamic scan assignment can leave CU slots without a run. Merge
         * only initialized owners, also on the ordinary output path. */
        unsigned count=0;
        for(unsigned i=0;i<group->count;++i) if(group->states[i]) group->states[count++]=group->states[i];
        for(unsigned i=count;i<group->count;++i) group->states[i]=0;
        if(!count) {
            if(a->numr==3) { struct qs_result *out=a->mr_addr[1]; out->rows=out->begin=0; out->finished=1; }
            return 0;
        }
        group->count=count;
    }
    struct row_sort *s=(void *)(uintptr_t)group->states[0];
    if(!s) return a->numr==2?0:-1;
    if(a->numr==2 && !csIsReentry(a)) { s->pending=ROW_NONE; s->pending_run=NULL; }
    int64_t status=row_resume(s,a); if(status) return status;
    status=row_combine(s,group,a,a->numr!=2); if(status) return status;
    if(a->numr==2) return row_finish(s,group,NULL);
    if(a->mr_len[1]<(long long)sizeof(struct qs_result)) return -1;
    struct qs_result *out=a->mr_addr[1];
    if(!s->source_active) {
        s->source_active=1; out->status=0; out->rows=out->begin=out->finished=0;
        out->str_bytes=0; s->output_column=0;
        if(s->text_columns) {
            memset(out->str_data,0,SOURCE_TEXT_HEADER); out->str_bytes=SOURCE_TEXT_HEADER;
        }
    }
    if(!s->heap && !s->finishing) {
        while(!s->heap) {
            s->heap=cs_work_alloc(s->context,(s->metrics.runs+1)*sizeof(*s->heap));
            if(!s->heap) { int64_t status=row_evict(s,s->context,a); if(status) return status; }
        }
    }
    if(!s->consolidated && !s->finishing) {
        int64_t status=consolidate(s,a); if(status) return status;
    }
    if(!s->heap_ready && !s->finishing) {
        if(!s->heap_initializing) {
            for(unsigned i=0;i<s->owners_count;++i)
                for(unsigned j=0;j<s->owners[i]->count;++j) if(s->owners[i]->runs[j].rows)
                    s->heap[s->heap_count++]=&s->owners[i]->runs[j];
            s->heap_build=s->heap_count/2; s->heap_initializing=1;
        }
        while(s->heap_build) { status=merge_sift(s,s->heap_build-1,a); if(status) return status; --s->heap_build; }
        s->heap_ready=1;
    }
    while(!s->finishing && out->rows<QS_RESULT_ROWS) {
        if(!s->heap_count || (s->plan.limit>=0 && s->emitted>=(uint64_t)s->plan.limit)) { s->finishing=1; break; }
        struct row_run *r=s->heap[0];
        if(s->root_pending==1) {
            if(r->position==r->rows) {
                if(r->data) { cs_work_free(r->owner,r->data); r->data=NULL; }
                s->heap[0]=s->heap[--s->heap_count];
            } else { status=merge_head(s,r,a); if(status) return status; }
            s->root_pending=2;
        }
        if(s->root_pending==2) {
            if(s->heap_count) { status=merge_sift(s,0,a); if(status) return status; }
            s->root_pending=0; continue;
        }
        if(s->skipped<(uint64_t)s->plan.offset) ++s->skipped;
        else {
            status=source_text(s,r->head,out,a);
            if(status==3) { s->source_active=0; return 0; }
            if(status) return status;
            struct qs_cell *row=out->data[out->rows++].cells;
            memcpy(row,r->head,s->row_bytes); restore_text_ids(s,row); ++s->emitted;
        }
        ++r->position; s->root_pending=1;
    }
    out->metrics=s->metrics;
    if(s->finishing) return row_finish(s,group,out);
    s->source_active=0; return 0;
}
