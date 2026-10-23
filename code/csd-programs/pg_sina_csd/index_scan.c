/* PG12 B-tree leading-integer equality join. PG transfers page addresses only;
 * key search, TID extraction, heap projection and filtering execute in this CSF.
 * Page data stays in the slot's I/O AFDM; cursors/tags use bounded workmem. */
#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "cs_csf.h"
#include "index_scan.h"
#include "query_eval.h"
#include <limits.h>
#include <string.h>
extern int64_t pg_sina_filter_impl(struct femu_csd_args *);
#define INDEX_KEYS 64
struct page_tag { uint32_t block; uint16_t heap, state; }; /* 0 absent, 1 loaded, 2 requested */
struct lookup { unsigned row, block, item, heap_block, heap_item, phase; int64_t key; };
struct index_cursor {
    struct page_tag tags[QS_INDEX_CACHE];
    struct lookup keys[INDEX_KEYS];
    unsigned position, count, hand, waiting, pending, last_slot;
    const unsigned char *heap_window;
    uint32_t heap_first,heap_pages;
    struct lookup scan;
    uint64_t *scan_tids;
    int scan_started,scan_done;
    unsigned scan_end;
};
_Static_assert(sizeof(struct index_cursor)<=4096,"index cursor must fit its per-CU budget");
struct index_join { cs_work_context *context; struct qs_plan scan; struct index_cursor *cursors[QS_WORKERS]; struct qs_dict *dictionary; };
/* Each CU owns one slot. Allocate its cursor on first use, rather than
 * charging all possible CUs to a single-CU scan's workmem context. */
static struct index_cursor *index_cursor(struct index_join *h,unsigned slot)
{
    if(slot>=QS_WORKERS) return NULL;
    if(!h->cursors[slot]) h->cursors[slot]=cs_work_calloc(h->context,1,sizeof(*h->cursors[slot]));
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
void qs_index_reset(struct qs_join_control *control,unsigned slot)
{
    struct index_join *h=(void *)(uintptr_t)control->state;
    if(h->cursors[slot]) memset(h->cursors[slot]->tags,0,sizeof(h->cursors[slot]->tags));
}
void qs_index_skip(struct qs_join_control *control,unsigned slot,uint64_t rows)
{
    struct index_join *h=(void *)(uintptr_t)control->state;
    struct index_cursor *c=index_cursor(h,slot);
    if(!c) return;
    for(unsigned i=0;i<c->count;++i) if(rows&(UINT64_C(1)<<c->keys[i].row)) c->keys[i].phase=4;
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
    if(h) { cs_work_context *ctx=h->context; control->state=0; cs_work_end(ctx); }
}
void qs_index_dictionary(struct qs_join_control *control,struct qs_dict *dictionary)
{ ((struct index_join *)(uintptr_t)control->state)->dictionary=dictionary; }
static const unsigned char *page_at(struct index_cursor *c,struct qs_task *task,void *io,unsigned block,unsigned heap)
{
    if(heap && block>=c->heap_first && block-c->heap_first<c->heap_pages)
        return c->heap_window+(size_t)(block-c->heap_first)*QS_PAGE;
    const struct page_tag *last=&c->tags[c->last_slot];
    if(last->state && last->block==block && last->heap==heap)
        return last->state==1?(unsigned char *)io+QS_SORT_IO_BYTES+(size_t)c->last_slot*QS_PAGE:NULL;
    for(unsigned i=0;i<QS_INDEX_CACHE;++i) if(c->tags[i].state && c->tags[i].block==block && c->tags[i].heap==heap)
        { c->last_slot=i; return c->tags[i].state==1?(unsigned char *)io+QS_SORT_IO_BYTES+(size_t)i*QS_PAGE:NULL; }
    if(task->index_nreads==QS_INDEX_READS) return NULL;
    for(unsigned n=0;n<QS_INDEX_CACHE;++n) {
        unsigned slot=c->hand++%QS_INDEX_CACHE;
        if(c->tags[slot].state==2) continue;
        c->tags[slot]=(struct page_tag){block,heap,2};
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
        for(unsigned i=0;i<QS_INDEX_CACHE;++i) if(c->tags[i].state==2) c->tags[i].state=1;
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
            if(!out->rows) memset(qs_nulls(out),0,((out->capacity+7)/8)*(size_t)out->ncols);
            int status=qs_scan_tuple(&h->scan,page,t-page,len,0,out,out->rows,h->dictionary);
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
    if(a->numr!=6 || a->mr_len[5]<QS_SORT_IO_BYTES+(long long)QS_INDEX_CACHE*QS_PAGE) return -1;
    if(cursor->waiting) {
        for(unsigned i=0;i<QS_INDEX_CACHE;++i) if(cursor->tags[i].state==2) cursor->tags[i].state=1;
        cursor->waiting=0; task->index_pending=task->index_nreads=0;
    }
    if(!resume) {
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
        unsigned done=0;
        for(unsigned k=0;k<cursor->count;++k) {
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
                    if(!dest) memset(qs_nulls(out),0,((out->capacity+7)/8)*(size_t)out->ncols);
                    int status=qs_scan_tuple(&h->scan,page,tuple-page,len,0,out,dest,h->dictionary); if(status) return status;
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
                    if(j->semi<0 || out->rows==out->capacity) { int64_t status=flush(stage,out,p,task,control,a,cursor,0); if(status) return status; }
                }
            }
            done+=key->phase==4;
        }
        if(done==cursor->count) { cursor->count=0; continue; }
        if(!task->index_nreads) return -2;
        cursor->waiting=1; task->index_pending=1; task->index_pages+=task->index_nreads;
        return QS_INDEX_WAIT;
    }
    if(out->rows) return flush(stage,out,p,task,control,a,cursor,0);
    return 0;
}
