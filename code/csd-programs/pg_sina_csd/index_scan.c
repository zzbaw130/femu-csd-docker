/* PG12 B-tree leading-integer equality join. PG transfers page addresses only;
 * key search, TID extraction, heap projection and filtering execute in this CSF.
 * Page data stays in the slot's I/O AFDM; cursors/tags use bounded workmem. */
#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "cs_csf.h"
#include "index_scan.h"
#include "query_eval.h"
#include "scan.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
extern int64_t pg_sina_filter_impl(struct femu_csd_args *);
#define INDEX_KEYS QS_INDEX_KEYS
#define INDEX_BUCKETS (2*QS_INDEX_CACHE)
_Static_assert(!(INDEX_BUCKETS&(INDEX_BUCKETS-1)),"index buckets must be a power of two");
struct page_tag { uint32_t block; uint16_t heap, state; };
#define SHARED_WAYS 8
#define HEAP_HISTORY 64
#define CACHE_SHARD_BYTES (8U*1024*1024)
#define CACHE_SAMPLE_MASK 63U
struct shared_page {
    uint64_t key, stamp;
};
struct shared_set {
    atomic_bool busy;
    uint64_t clock;
    struct shared_page ways[SHARED_WAYS];
    uint64_t seen_heap[HEAP_HISTORY];
};
struct cache_shard {
    unsigned sets;
    unsigned char *data;
    struct shared_set set[];
};
struct qs_index_cache {
    unsigned count;
    bool profiling;
    cs_work_cache shards[];
};
struct lookup { unsigned row, block, item, heap_block, heap_item, phase; int64_t key; };
struct index_cursor {
    struct page_tag tags[QS_INDEX_SLOTS];
    uint16_t heads[INDEX_BUCKETS],next[QS_INDEX_SLOTS]; /* slot+1; zero ends a chain */
    struct lookup keys[INDEX_KEYS];
    unsigned position, count, hand, waiting, pending, last_slot, root;
    const unsigned char *heap_window;
    uint32_t heap_first,heap_pages;
    struct qs_index_cache *shared;
    uint32_t heap_file,index_file;
    uint64_t hits[2],misses[2]; /* B-tree, then Heap. */
    unsigned cache_sequence[2];
    struct qs_timing cache_timing[4];
    struct lookup scan;
    uint64_t *scan_tids;
    int scan_started,scan_done;
    unsigned scan_end;
    struct qs_scan_cursor *decoder;
    void *spill_io;
    uint64_t dictionary_completion;
    unsigned dictionary_pending,dictionary_key,spill_bound;
};
_Static_assert(sizeof(struct index_cursor)<=16384,"index cursor must fit its per-CU budget");

static bool cache_shard_create(cs_work_context *ctx,cs_work_cache *storage,size_t bytes,size_t pool_limit)
{
    unsigned sets=(bytes-sizeof(struct cache_shard))/(sizeof(struct shared_set)+SHARED_WAYS*QS_PAGE);
    if(!sets || cs_work_cache_allocate(ctx,storage,bytes,pool_limit)) return false;
    struct cache_shard *cache=cs_work_cache_pin(storage);
    if(!cache) return false;
    cache->sets=sets;
    cache->data=(void *)(cache->set+sets);
    for(unsigned i=0;i<sets;++i) {
        cache->set[i].clock=0;
        atomic_init(&cache->set[i].busy,false);
        memset(cache->set[i].ways,0,sizeof(cache->set[i].ways));
        memset(cache->set[i].seen_heap,0,sizeof(cache->set[i].seen_heap));
    }
    cs_work_cache_unpin(storage);
    return true;
}
struct qs_index_cache *qs_index_cache_create(cs_work_context *ctx,size_t bytes,size_t pool_limit,bool profiling)
{
    unsigned count=(bytes+CACHE_SHARD_BYTES-1)/CACHE_SHARD_BYTES;
    struct qs_index_cache *cache=cs_work_calloc(ctx,1,sizeof(*cache)+count*sizeof(*cache->shards));
    if(!cache) return NULL;
    cache->profiling=profiling;
    for(unsigned i=0;i<count;++i) {
        size_t shard=bytes<CACHE_SHARD_BYTES?bytes:CACHE_SHARD_BYTES;
        /* Leave room for the allocator's own metadata within the cap. */
        if(!cache_shard_create(ctx,&cache->shards[i],shard-128,pool_limit)) break;
        ++cache->count;
        bytes-=shard;
    }
    if(cache->count) return cache;
    cs_work_free(ctx,cache);
    return NULL;
}

/* Keep the page only for this copy. Private AFDM handles I/O and cursor
 * retention, so workmem can discard unpinned shards between copies. */
static uint64_t cache_clock(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static bool shared_copy(struct index_cursor *cursor,uint64_t key,void *page,bool insert,bool heap)
{
    struct qs_index_cache *shared=cursor->shared;
    bool sample=shared->profiling && !(cursor->cache_sequence[insert]++ & CACHE_SAMPLE_MASK);
    uint64_t start=sample?cache_clock():0, copy_ns=0;
    uint64_t hash=key;
    hash=(hash^(hash>>30))*UINT64_C(0xbf58476d1ce4e5b9);
    hash=(hash^(hash>>27))*UINT64_C(0x94d049bb133111eb);
    hash^=hash>>31;
    cs_work_cache *storage=&shared->shards[(uint32_t)hash%shared->count];
    struct cache_shard *cache=cs_work_cache_pin(storage);
    bool hit=false;
    if(cache) {
        unsigned index=(hash>>32)%cache->sets;
        struct shared_set *set=&cache->set[index];
        if(!atomic_exchange_explicit(&set->busy,true,memory_order_acquire)) {
            unsigned victim=0,slot=0;
            for(;slot<SHARED_WAYS;++slot) {
                if(set->ways[slot].key==key) {
                    hit=true;
                    break;
                }
                if(set->ways[slot].stamp<set->ways[victim].stamp) victim=slot;
            }
            bool admit=insert;
            if(insert && heap && !hit) {
                /* A first load records the key without copying a page. A
                 * matching later load can use its AFDM page to fill the cache. */
                uint64_t *seen=&set->seen_heap[(hash>>16)&(HEAP_HISTORY-1)];
                admit=*seen==key;
                *seen=key;
            }
            if(hit || admit) {
                if(!hit) slot=victim;
                unsigned char *data=cache->data+((size_t)index*SHARED_WAYS+slot)*QS_PAGE;
                if(!insert || !hit) {
                    uint64_t copy_start=sample?cache_clock():0;
                    if(insert) memcpy(data,page,QS_PAGE);
                    else memcpy(page,data,QS_PAGE);
                    if(sample) {
                        copy_ns=cache_clock()-copy_start;
                        struct qs_timing *copy=&cursor->cache_timing[2+insert];
                        copy->wall_ns+=copy_ns;
                        ++copy->calls;
                    }
                }
                if(insert) set->ways[slot].key=key;
                set->ways[slot].stamp=++set->clock;
            }
            atomic_store_explicit(&set->busy,false,memory_order_release);
        }
        cs_work_cache_unpin(storage);
    }
    if(sample) {
        struct qs_timing *op=&cursor->cache_timing[insert];
        op->wall_ns+=cache_clock()-start-copy_ns;
        ++op->calls;
    }
    return hit;
}
static unsigned char *private_page(void *io,unsigned slot)
{ return (unsigned char *)io+QS_SORT_IO_BYTES+(size_t)slot*QS_PAGE; }
static uint64_t page_key(const struct index_cursor *c,unsigned block,unsigned heap)
{
    unsigned file=heap?c->heap_file:c->index_file;
    return file?((uint64_t)file<<32)|block:0;
}
static void loaded_pages(struct index_cursor *c,void *io)
{
    for(unsigned i=0;i<QS_INDEX_SLOTS;++i) if(c->tags[i].state==2) {
        struct page_tag *tag=&c->tags[i];
        tag->state=1;
        uint64_t key=page_key(c,tag->block,tag->heap);
        if(c->shared && key) shared_copy(c,key,private_page(io,i),true,tag->heap);
    }
}
struct index_join {
    cs_work_context *context;
    struct qs_plan scan;
    struct index_cursor *cursors[QS_WORKERS];
    struct qs_dict *dictionary;
    struct qs_dictionary *paged_dictionary;
};
/* Each CU owns one slot. Allocate its cursor on first use, rather than
 * charging all possible CUs to a single-CU scan's workmem context. */
static struct index_cursor *index_cursor(struct index_join *h,unsigned slot)
{
    if(slot>=QS_WORKERS) return NULL;
    if(!h->cursors[slot]) {
        h->cursors[slot]=cs_work_calloc(h->context,1,sizeof(*h->cursors[slot]));
        if(h->cursors[slot]) {
            h->cursors[slot]->root=h->scan.join.indexroot;
            if(h->paged_dictionary) {
                h->cursors[slot]->decoder=qs_scan_cursor_create(h->context,h->paged_dictionary);
                if(!h->cursors[slot]->decoder) {
                    cs_work_free(h->context,h->cursors[slot]); h->cursors[slot]=NULL;
                }
            }
        }
    }
    return h->cursors[slot];
}
static uint16_t u16(const unsigned char *p) { uint16_t x; memcpy(&x,p,2); return x; }
static uint32_t u32(const unsigned char *p) { uint32_t x; memcpy(&x,p,4); return x; }
int qs_index_init(const struct qs_plan *p,struct qs_join_control *control)
{
    struct cs_work_stats stats; if(cs_work_get_stats(&stats)) return -4;
    size_t limit=p->join.work_mem_bytes,physical=stats.capacity_bytes;
    if(!limit || limit>physical) limit=physical;
    cs_work_context *ctx=p->work_mem_parent?cs_work_begin_child((void *)(uintptr_t)p->work_mem_parent,limit):cs_work_begin_limit(limit);
    if(!ctx) return -4;
    struct index_join *h=cs_work_calloc(ctx,1,sizeof(*h));
    if(!h) { cs_work_end(ctx); return -4; }
    h->context=ctx; h->scan=*p;
    if(!index_cursor(h,0)) { cs_work_end(ctx); return -4; }
    unsigned base=p->nattrs-p->join.nattrs; h->scan.nattrs=p->join.nattrs;
    for(unsigned i=0;i<h->scan.nattrs;++i) { h->scan.cols[i]=p->cols[base+i]; h->scan.cols[i].deferred=0; }
    for(unsigned i=0;i<p->nstrings;++i) h->scan.strings[i].column=p->strings[i].column>=base?p->strings[i].column-base:255;
    control->state=(uintptr_t)h; control->indexed=1; control->batches=1;
    return 0;
}
void qs_index_cache_bind(struct qs_join_control *control,struct qs_index_cache *cache)
{
    struct index_join *h=(void *)(uintptr_t)control->state;
    struct index_cursor *c=h->cursors[0];
    c->shared=cache;
    c->heap_file=h->scan.join.relid;
    c->index_file=h->scan.join.indexid;
}
void qs_index_cache_stats(const struct qs_join_control *control,struct qs_metrics *metrics,struct qs_profile *profile)
{
    const struct index_join *h=(void *)(uintptr_t)control->state;
    if(!h || !h->cursors[0]) return;
    struct index_cursor *c=h->cursors[0];
    if(metrics) for(unsigned kind=0;kind<2;++kind) {
        metrics->index_cache_hits[kind]+=c->hits[kind];
        metrics->index_cache_misses[kind]+=c->misses[kind];
    } else {
        memset(c->hits,0,sizeof(c->hits));
        memset(c->misses,0,sizeof(c->misses));
        memset(c->cache_timing,0,sizeof(c->cache_timing));
    }
    if(profile) for(unsigned i=0;i<4;++i) {
        struct qs_timing *out=&profile->operators[QS_INDEX_CACHE_LOOKUP+i];
        out->calls+=c->cache_timing[i].calls;
        out->wall_ns+=c->cache_timing[i].wall_ns;
    }
}
void qs_index_reset(struct qs_join_control *control,unsigned slot)
{
    struct index_join *h=(void *)(uintptr_t)control->state;
    if(h->cursors[slot]) {
        memset(h->cursors[slot]->tags,0,sizeof(h->cursors[slot]->tags));
        memset(h->cursors[slot]->heads,0,sizeof(h->cursors[slot]->heads));
        memset(h->cursors[slot]->next,0,sizeof(h->cursors[slot]->next));
    }
}
void qs_index_skip(struct qs_join_control *control,unsigned slot,const uint64_t *rows)
{
    struct index_join *h=(void *)(uintptr_t)control->state;
    struct index_cursor *c=index_cursor(h,slot);
    if(!c) return;
    for(unsigned i=0;i<c->count;++i) {
        unsigned row=c->keys[i].row;
        if(row<QS_INDEX_KEYS && (rows[row/64]&(UINT64_C(1)<<(row%64)))) c->keys[i].phase=4;
    }
}
void qs_index_heap_window(struct qs_join_control *control,unsigned slot,const void *data,uint32_t first,uint32_t pages)
{
    struct index_join *h=(void *)(uintptr_t)control->state; struct index_cursor *c=index_cursor(h,slot);
    if(!c) return;
    c->heap_window=data; c->heap_first=first; c->heap_pages=pages;
}
void qs_index_close(struct qs_join_control *control)
{
    struct index_join *h=(void *)(uintptr_t)control->state;
    if(h) {
        cs_work_context *ctx=h->context; control->state=0; cs_work_end(ctx);
    }
}
void qs_index_dictionary(struct qs_join_control *control,struct qs_dict *dictionary)
{ ((struct index_join *)(uintptr_t)control->state)->dictionary=dictionary; }
int qs_index_paged_dictionary(struct qs_join_control *control,struct qs_dictionary *dictionary)
{
    struct index_join *h=(void *)(uintptr_t)control->state;
    if(!h || !dictionary || (h->paged_dictionary && h->paged_dictionary!=dictionary)) return -1;
    h->paged_dictionary=dictionary;
    for(unsigned i=0;i<QS_WORKERS;++i) if(h->cursors[i] && !h->cursors[i]->decoder) {
        h->cursors[i]->decoder=qs_scan_cursor_create(h->context,dictionary);
        if(!h->cursors[i]->decoder) return -4;
    }
    return 0;
}
void qs_index_spill_io(struct qs_join_control *control,unsigned slot,void *io)
{
    struct index_join *h=(void *)(uintptr_t)control->state;
    struct index_cursor *c=h?index_cursor(h,slot):NULL;
    if(c) { c->spill_io=io; c->spill_bound=io!=NULL; }
}
int qs_index_dictionary_pending(const struct qs_join_control *control,unsigned slot)
{
    const struct index_join *h=(void *)(uintptr_t)control->state;
    return h && slot<QS_WORKERS && h->cursors[slot] && h->cursors[slot]->dictionary_pending;
}
static int64_t decode_tuple(struct index_join *h,struct index_cursor *c,const unsigned char *page,
    unsigned offset,unsigned length,struct qs_chunk *out,unsigned row)
{
    if(!h->paged_dictionary) return qs_scan_tuple(&h->scan,page,offset,length,0,out,row,h->dictionary);
    if(!c->decoder || !c->spill_io) return -1;
    int64_t status=qs_scan_tuple_paged(c->decoder,&h->scan,page,offset,length,0,out,row,
        c->spill_io,c->dictionary_completion);
    c->dictionary_completion=0; c->dictionary_pending=status>0;
    return status;
}
static unsigned page_bucket(unsigned block,unsigned heap)
{ return (block*UINT32_C(0x9e3779b1)^(heap?UINT32_C(0x85ebca6b):0))&(INDEX_BUCKETS-1); }
static const unsigned char *page_data(const struct index_cursor *c,void *io,unsigned slot)
{
    const struct page_tag *tag=&c->tags[slot];
    if(tag->state!=1) return NULL;
    return private_page(io,slot);
}
static const unsigned char *page_at(struct index_cursor *c,struct qs_task *task,void *io,unsigned block,unsigned heap)
{
    if(heap && block>=c->heap_first && block-c->heap_first<c->heap_pages)
        return c->heap_window+(size_t)(block-c->heap_first)*QS_PAGE;
    const struct page_tag *last=&c->tags[c->last_slot];
    if(last->state && last->block==block && last->heap==heap)
        return page_data(c,io,c->last_slot);
    unsigned bucket=page_bucket(block,heap);
    for(unsigned at=c->heads[bucket];at;at=c->next[at-1]) {
        unsigned i=at-1;
        if(c->tags[i].block==block && c->tags[i].heap==heap) {
            c->last_slot=i;
            return page_data(c,io,i);
        }
    }
    if(task->index_nreads==QS_INDEX_READS) return NULL;
    const struct femu_graph_read_pool *pool=(const void *)((unsigned char *)io+QS_INDEX_POOL_OFFSET);
    for(unsigned n=0;n<QS_INDEX_SLOTS;++n) {
        unsigned slot=n<QS_INDEX_CACHE?c->hand++%QS_INDEX_CACHE:n;
        const struct page_tag *tag=&c->tags[slot];
        /* Every lookup batch revisits the root. Keep its one existing slot;
         * rotate the rest so LOAD destinations remain mostly contiguous. */
        if(tag->state==2 || (tag->state && !tag->heap && tag->block==c->root) ||
           __atomic_load_n(&pool->readers[slot],__ATOMIC_ACQUIRE)) continue;
        if(tag->state) {
            uint16_t *link=&c->heads[page_bucket(tag->block,tag->heap)];
            while(*link && *link!=slot+1) link=&c->next[*link-1];
            if(*link) *link=c->next[slot];
        }
        uint64_t key=page_key(c,block,heap);
        bool ready=c->shared && key && shared_copy(c,key,private_page(io,slot),false,heap);
        c->tags[slot]=(struct page_tag){.block=block,.heap=heap,.state=ready?1:2};
        c->next[slot]=c->heads[bucket]; c->heads[bucket]=slot+1;
        if(ready) {
            ++c->hits[heap];
            c->last_slot=slot; return page_data(c,io,slot);
        }
        ++c->misses[heap];
        task->index_reads[task->index_nreads++]=(struct qs_index_read){block,slot,heap};
        return NULL;
    }
    return NULL;
}
/* Validate an on-disk page/item before following its TID/downlink. */
static int page_items(const unsigned char *p,int index)
{
    unsigned lower=u16(p+12),upper=u16(p+14),special=u16(p+16);
    if(lower<24 || lower>upper || upper>special || special>QS_PAGE || (lower-24)%4 || u16(p+18)!=(QS_PAGE|4)) return -1;
    if(index) { if(special!=QS_PAGE-16 || (u16(p+special+12)&(4|8|16|128))) return -1; }
    else if(special!=QS_PAGE || !(u16(p+10)&4)) return -1;
    return (lower-24)/4;
}
static const unsigned char *item_at(const unsigned char *p,unsigned item,unsigned *length)
{
    if(!item || item>(unsigned)(u16(p+12)-24)/4) return NULL;
    uint32_t lp=u32(p+24+(item-1)*4); unsigned off=lp&32767,flags=(lp>>15)&3;
    *length=lp>>17;
    /* LP_DEAD index tuples retain valid keys/TIDs; the frozen heap is checked. */
    if((flags!=1 && flags!=3) || off<u16(p+14) || off>QS_PAGE || *length>QS_PAGE-off) return NULL;
    return p+off;
}
/* Compare the stored leading key to the lookup value. NULL sorts last.
 * PG12 pivot suffix truncation preserves the first key except the -infinity
 * downlink, which is handled separately. No posting-list format exists in v12. */
static int compare(const unsigned char *t,unsigned len,unsigned bytes,int64_t key,int *cmp)
{
    if(len<8 || (u16(t+6)&8191)!=len) return -1;
    unsigned info=u16(t+6),off=(info&0x8000)?16:8;
    if((info&0x8000) && !(t[8]&1)) { *cmp=1; return 0; }
    if(len<off+bytes) return -1;
    int64_t value=0;
    if(bytes==2) value=(int16_t)u16(t+off);
    else if(bytes==4) value=(int32_t)u32(t+off);
    else if(bytes==8) memcpy(&value,t+off,8);
    else return -1;
    *cmp=(value>key)-(value<key); return 0;
}
static int seek(const unsigned char *page,struct lookup *key,unsigned bytes)
{
    int count=page_items(page,1); if(count<0) return -2;
    const unsigned char *opaque=page+QS_PAGE-16;
    unsigned first=u32(opaque+4)?2:1,leaf=u16(opaque+12)&1;
    unsigned lo=first+(leaf?0:1),hi=count+1;
    if(first>(unsigned)count && (!leaf || first!=(unsigned)count+1)) return -2;
    while(lo<hi) {
        unsigned mid=lo+(hi-lo)/2,len=0; const unsigned char *t=item_at(page,mid,&len); int cmp;
        if(!t || compare(t,len,bytes,key->key,&cmp)) return -2;
        if(cmp<0) lo=mid+1; else hi=mid;
    }
    if(leaf) { key->item=lo; key->phase=2; }
    else {
        unsigned len=0; const unsigned char *t=item_at(page,lo-1,&len);
        if(!t || len<8) return -2;
        unsigned next=((uint32_t)u16(t)<<16)|u16(t+2);
        if(!next || next==key->block) return -2;
        key->block=next;
    }
    return 0;
}
void qs_index_scan_range(struct qs_join_control *control,unsigned first,unsigned end)
{
    struct index_join *h=(void *)(uintptr_t)control->state;
    struct index_cursor *c=h->cursors[0];
    c->scan=(struct lookup){.block=first}; c->scan_end=end;
    c->scan_started=1; c->scan_done=first>=end;
}
int64_t qs_index_scan(struct qs_chunk *out,struct qs_task *task,struct qs_join_control *control,void *io,
    unsigned heap_pages,unsigned index_pages,int flags)
{
    struct index_join *h=(void *)(uintptr_t)control->state;
    struct index_cursor *c=h->cursors[0];
    if(c->waiting) {
        loaded_pages(c,io);
        c->waiting=0; task->index_pending=task->index_nreads=0;
    }
    if(!c->scan_started) {
        if(!(flags&4)) {
            c->scan_tids=cs_work_alloc(h->context,QS_RESULT_ROWS*sizeof(*c->scan_tids));
            if(!c->scan_tids) return -4;
        }
        c->scan_started=1; c->scan.block=h->scan.join.indexroot;
        c->scan.phase=1; c->scan_done=!c->scan.block;
    }
    for(;;) {
        while(!c->scan_done && c->count<QS_RESULT_ROWS) {
            if(!c->scan.block || c->scan.block>=index_pages) return -2;
            const unsigned char *page=page_at(c,task,io,c->scan.block,0);
            if(!page) {
                if(c->scan.phase!=1) for(unsigned i=1;i<16 && i<index_pages-c->scan.block;++i)
                    page_at(c,task,io,c->scan.block+i,0);
                goto wait;
            }
            if(c->scan_end && (u16(page+QS_PAGE-4)&(4|16))) {
                c->scan_done=++c->scan.block>=c->scan_end; c->scan.phase=0; continue;
            }
            int count=page_items(page,1); if(count<0) return -2;
            if(c->scan_end && !(u16(page+QS_PAGE-4)&1)) {
                c->scan_done=++c->scan.block>=c->scan_end; c->scan.phase=0; continue;
            }
            unsigned first=u32(page+QS_PAGE-12)?2:1;
            if(c->scan.phase!=2) {
                if(!(u16(page+QS_PAGE-4)&1)) {
                    unsigned len=0; const unsigned char *t=item_at(page,first,&len);
                    if(!t || len<8) return -2;
                    unsigned next=((uint32_t)u16(t)<<16)|u16(t+2);
                    if(!next || next==c->scan.block) return -2;
                    c->scan.block=next; continue;
                }
                c->scan.item=first; c->scan.phase=2;
            }
            if(c->scan.item>(unsigned)count) {
                c->scan.block=c->scan_end?c->scan.block+1:u32(page+QS_PAGE-12); c->scan.phase=0;
                if(!c->scan.block || (c->scan_end && c->scan.block>=c->scan_end)) c->scan_done=1;
                continue;
            }
            unsigned item=c->scan.item++,len=0;
            if(((u32(page+24+(item-1)*4)>>15)&3)==3) continue;
            const unsigned char *t=item_at(page,item,&len); if(!t || len<8) return -2;
            if(flags&4) {
                unsigned info=u16(t+6),off=info&0x8000?16:8;
                if((info&8191)!=len || ((info&0x8000) && len<16)) return -2;
                if(!out->rows) memset(qs_nulls(out),0,((out->capacity+7)/8)*(size_t)out->ncols);
                for(unsigned k=0;k<(h->scan.join.build_key2?2U:1U);++k) {
                    unsigned attr=(k?h->scan.join.build_key2:h->scan.join.build_key)-1;
                    if(attr>=h->scan.nattrs) return -2;
                    unsigned bytes=k?(unsigned)h->scan.cols[attr].length:h->scan.join.index_key_bytes;
                    if(bytes!=2 && bytes!=4 && bytes!=8) return -2;
                    int null=(info&0x8000) && !(t[8]&(1U<<k)),slot=h->scan.cols[attr].slot;
                    if(!null) off=(off+bytes-1)&~(bytes-1);
                    if(slot>=0) {
                        int64_t value=0;
                        if(!null) {
                            if(len<off+bytes) return -2;
                            if(bytes==2) value=(int16_t)u16(t+off);
                            else if(bytes==4) value=(int32_t)u32(t+off);
                            else memcpy(&value,t+off,8);
                        }
                        qs_values(out)[(size_t)slot*out->capacity+out->rows]=value;
                        qs_dscales(out)[(size_t)slot*out->capacity+out->rows]=0;
                        if(null) qs_nulls(out)[(size_t)slot*((out->capacity+7)/8)+out->rows/8]|=1U<<(out->rows%8);
                    }
                    if(!null) { if(off+bytes>len) return -2; off+=bytes; }
                }
                qs_selection(out)[out->rows]=out->rows; ++out->rows; ++out->selected;
                if(out->rows==out->capacity) return 0;
                continue;
            }
            c->scan_tids[c->count++]=((uint64_t)(((uint32_t)u16(t)<<16)|u16(t+2))<<16)|u16(t+4);
        }
        /* Decode in TID-list order; collect missing pages at the first miss. */
        while(c->position<c->count) {
            uint64_t tid=c->scan_tids[c->position];
            if((tid>>16)>=heap_pages) return -2;
            const unsigned char *page=page_at(c,task,io,tid>>16,1);
            if(!page) {
                unsigned block=tid>>16;
                if(flags&2) for(unsigned i=1;i<32 && i<heap_pages-block;++i) page_at(c,task,io,block+i,1);
                goto wait;
            }
            if(page_items(page,0)<0) return -2;
            unsigned item=tid&65535,steps=0;
            for(;;) {
                if(!item || item>(unsigned)(u16(page+12)-24)/4 || ++steps>QS_PAGE/4) return -2;
                uint32_t lp=u32(page+24+(item-1)*4); unsigned flags=(lp>>15)&3;
                if(flags==2) { item=lp&32767; continue; }
                if(flags!=1) return -2;
                break;
            }
            unsigned len=0; const unsigned char *t=item_at(page,item,&len);
            if(!t || len<23) return -2;
            if(!out->rows && !c->dictionary_pending) memset(qs_nulls(out),0,((out->capacity+7)/8)*(size_t)out->ncols);
            int64_t status=decode_tuple(h,c,page,t-page,len,out,out->rows);
            if(status) return status;
            qs_selection(out)[out->rows]=out->rows; ++out->rows; ++out->selected;
            ++task->index_tuples; ++c->position;
            if(out->rows==out->capacity) return 0;
        }
        c->count=c->position=0;
        if(c->scan_done) return 0;
    }
wait:
    for(unsigned i=c->position;i<c->count && task->index_nreads<QS_INDEX_READS;++i)
        page_at(c,task,io,c->scan_tids[i]>>16,1);
    if(!task->index_nreads) return -2;
    c->waiting=task->index_pending=1; task->index_pages+=task->index_nreads;
    return QS_INDEX_WAIT;
}
int64_t qs_index_scan_paged(struct qs_chunk *out,struct qs_task *task,struct qs_join_control *control,
    void *io,unsigned heap_pages,unsigned index_pages,int flags,struct femu_csd_args *a)
{
    if(!a || !a->numr || a->mr_len[a->numr-1]<QS_SORT_IO_BYTES) return -1;
    struct index_join *h=(void *)(uintptr_t)control->state;
    if(!h || !h->paged_dictionary) return -1;
    struct index_cursor *c=h->cursors[0];
    c->spill_io=a->mr_addr[a->numr-1];
    c->dictionary_completion=c->dictionary_pending?csReentryValue(a):0;
    return qs_index_scan(out,task,control,io,heap_pages,index_pages,flags);
}
static int64_t flush(unsigned stage,struct qs_chunk *out,const struct qs_plan *p,struct qs_task *task,
    struct qs_join_control *control,struct femu_csd_args *a,struct index_cursor *cursor,int resume)
{
    if(!resume && !qs_join_at(p,stage)->semi && qs_join_at(p,stage)->predicate>=0) {
        struct qs_plan filter=*p; filter.predicate=qs_join_at(p,stage)->predicate;
        for(unsigned i=0;i<filter.nattrs;++i) filter.cols[i].deferred=0;
        void *mr[]={out,&filter}; long long len[]={qs_chunk_bytes(out->capacity,out->ncols),sizeof(filter)};
        struct femu_csd_args call={.numr=2,.mr_addr=mr,.mr_len=len};
        int64_t status=pg_sina_filter_impl(&call); if(status) return status;
    }
    cursor->pending=1;
    int64_t status=qs_join_consume(stage,out,p,task,control,a,resume); if(status) return status;
    cursor->pending=0; out->rows=out->selected=0; return 0;
}
int64_t qs_index_probe(unsigned stage,struct qs_chunk *c,const struct qs_plan *p,struct qs_task *task,
    struct qs_join_control *control,struct femu_csd_args *a,int resume)
{
    const struct qs_join *j=qs_join_at(p,stage); struct index_join *h=(void *)(uintptr_t)control[stage].state;
    struct index_cursor *cursor=index_cursor(h,task->slot); if(!cursor) return -4;
    struct qs_chunk *out=(void *)((unsigned char *)a->mr_addr[4]+stage*qs_join_chunk_stride(p));
    if(a->numr!=6 || a->mr_len[5]<QS_INDEX_IO_BYTES) return -1;
    if(h->paged_dictionary) {
        if(!cursor->spill_bound) cursor->spill_io=a->mr_addr[5];
        cursor->dictionary_completion=cursor->dictionary_pending?csReentryValue(a):0;
    }
    if(cursor->waiting) {
        loaded_pages(cursor,a->mr_addr[5]);
        cursor->waiting=0; task->index_pending=task->index_nreads=0;
    }
    if(!resume && !cursor->dictionary_pending) {
        cursor->position=cursor->count=cursor->pending=0; out->rows=out->selected=0;
    } else if(cursor->pending) {
        int64_t status=flush(stage,out,p,task,control,a,cursor,csIsReentry(a)); if(status) return status;
    }
    unsigned base=qs_probe_attrs(p); for(unsigned i=0;i<stage;++i) base+=qs_join_at(p,i)->nattrs;
    unsigned keyslot=stage?(unsigned)p->cols[j->probe_key].slot:0;
    if(!stage) for(int i=0;i<j->probe_key;++i) keyslot+=p->cols[i].slot>=0;
    for(;;) {
        if(!cursor->count) {
            while(cursor->position<c->selected && cursor->count<INDEX_KEYS) {
                unsigned row=qs_selection(c)[cursor->position++]; if(row>=c->rows) return -1;
                if(qs_nulls(c)[(size_t)keyslot*((c->capacity+7)/8)+row/8]&(1U<<(row%8))) continue;
                cursor->keys[cursor->count++]=(struct lookup){.row=row,.block=j->indexroot,.phase=j->indexroot?1:4,.key=qs_values(c)[(size_t)keyslot*c->capacity+row]};
            }
            if(!cursor->count) break;
        }
        unsigned done=0,candidates=0,first=cursor->dictionary_pending?cursor->dictionary_key:0;
        for(unsigned k=first;k<cursor->count;++k) {
            struct lookup *key=&cursor->keys[k];
            while(key->phase!=4) {
                int heap=key->phase==3;
                const unsigned char *page=page_at(cursor,task,a->mr_addr[5],heap?key->heap_block:key->block,heap);
                if(!page) break;
                if(key->phase==1) { int status=seek(page,key,j->index_key_bytes); if(status) return status; continue; }
                if(key->phase==2) {
                    int count=page_items(page,1); if(count<0) return -2;
                    if(key->item>(unsigned)count) {
                        key->block=u32(page+QS_PAGE-12);
                        if(!key->block) key->phase=4;
                        else key->phase=1; /* lower-bound on the next leaf */
                        continue;
                    }
                    unsigned len=0; const unsigned char *tuple=item_at(page,key->item++,&len); int cmp;
                    if(!tuple || compare(tuple,len,j->index_key_bytes,key->key,&cmp)) return -2;
                    if(((u32(page+24+(key->item-2)*4)>>15)&3)==3) continue;
                    if(cmp>0) { key->phase=4; continue; }
                    if(cmp<0) continue;
                    key->heap_block=((uint32_t)u16(tuple)<<16)|u16(tuple+2); key->heap_item=u16(tuple+4); key->phase=3;
                } else {
                    if(page_items(page,0)<0) return -2;
                    unsigned item=key->heap_item,steps=0;
                    /* VACUUM may leave an all-visible HOT root redirected to
                     * its surviving heap-only tuple on this same page. */
                    for(;;) {
                        if(!item || item>(unsigned)(u16(page+12)-24)/4 || ++steps>QS_PAGE/4) return -2;
                        uint32_t lp=u32(page+24+(item-1)*4); unsigned flags=(lp>>15)&3;
                        if(flags==2) { item=lp&32767; continue; }
                        if(flags!=1) return -2;
                        break;
                    }
                    unsigned len=0; const unsigned char *tuple=item_at(page,item,&len);
                    if(!tuple || len<23) return -2;
                    unsigned dest=out->rows;
                    if(!dest && !cursor->dictionary_pending) memset(qs_nulls(out),0,((out->capacity+7)/8)*(size_t)out->ncols);
                    cursor->dictionary_key=k;
                    int64_t status=decode_tuple(h,cursor,page,tuple-page,len,out,dest); if(status) return status;
                    if(j->semi>0) {
                        struct qs_eval eval={.plan=p,.chunk=out,.row=dest}; struct qs_cell match;
                        if(j->predicate<0 || qs_evaluate(&eval,j->predicate,&match) || match.isnull || !match.value) {
                            key->phase=2; continue;
                        }
                    }
                    unsigned local=0;
                    for(unsigned col=0;col<base;++col) if(p->cols[col].slot>=0) {
                        unsigned slot=p->cols[col].slot,src=stage?slot:local++;
                        qs_values(out)[(size_t)slot*out->capacity+dest]=qs_values(c)[(size_t)src*c->capacity+key->row];
                        qs_dscales(out)[(size_t)slot*out->capacity+dest]=qs_dscales(c)[(size_t)src*c->capacity+key->row];
                        if(qs_nulls(c)[(size_t)src*((c->capacity+7)/8)+key->row/8]&(1U<<(key->row%8))) qs_nulls(out)[(size_t)slot*((out->capacity+7)/8)+dest/8]|=1U<<(dest%8);
                    }
                    ++task->index_tuples; key->phase=j->semi>0?4:2;
                    qs_selection(out)[dest]=dest; ++out->rows; ++out->selected;
                    if(j->semi==-1 || out->rows==out->capacity) { int64_t status=flush(stage,out,p,task,control,a,cursor,0); if(status) return status; }
                    /* Let each unresolved key contribute one candidate per
                     * round. The consumer can filter them together, then
                     * qs_index_skip stops only keys with an accepted match. */
                    if(j->semi==-2) { ++candidates; break; }
                }
            }
            done+=key->phase==4;
        }
        if(j->semi==-2 && out->rows) {
            int64_t status=flush(stage,out,p,task,control,a,cursor,0); if(status) return status;
        }
        if(done==cursor->count) { cursor->count=0; continue; }
        if(first) continue; /* Revisit earlier keys only after the retained tuple completes. */
        if(candidates && !task->index_nreads) continue;
        if(!task->index_nreads) return -2;
        cursor->waiting=1; task->index_pending=1; task->index_pages+=task->index_nreads;
        return QS_INDEX_WAIT;
    }
    if(out->rows) return flush(stage,out,p,task,control,a,cursor,0);
    return 0;
}
