/* Exercise real PG12 index/heap pages with a dictionary larger than workmem.
 * malloc-backed pages/tokens below model AFDM and NVM, not operator memory. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "dictionary.h"
#include "index_scan.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ROWS 4096U
#define PER_PAGE 16U
#define LEAF_ROWS 301U
#define LEAVES ((ROWS+LEAF_ROWS-1)/LEAF_ROWS)
#define TEXT_BYTES 192U
#define LIMIT (768U*1024)
static unsigned char heap[ROWS/PER_PAGE][QS_PAGE],index_pages[LEAVES+2][QS_PAGE];
static void **tokens;
static unsigned ntokens,live,stores,loads,seen[ROWS],expected[ROWS];
static struct qs_dictionary *dictionary;
static struct qs_dictionary_access *reader;
static unsigned char read_io[QS_SORT_IO_BYTES];
static void w16(void *p,uint16_t v) { memcpy(p,&v,2); }
static void w32(void *p,uint32_t v) { memcpy(p,&v,4); }
static void init_page(unsigned char *p,unsigned special)
{
    memset(p,0,QS_PAGE); w16(p+10,4); w16(p+12,24); w16(p+14,special);
    w16(p+16,special); w16(p+18,QS_PAGE|4);
}
static void add(unsigned char *p,const void *tuple,unsigned length)
{
    uint16_t lower,upper; memcpy(&lower,p+12,2); memcpy(&upper,p+14,2);
    assert(upper>=lower+length+4); upper-=length; memcpy(p+upper,tuple,length);
    w32(p+lower,upper|(1U<<15)|(length<<17)); w16(p+12,lower+4); w16(p+14,upper);
}
static void text(char *p,unsigned row,unsigned column)
{
    memset(p,'a'+(row+column)%26,TEXT_BYTES); w32(p,row); w32(p+4,column);
}
static void index_tuple(unsigned char *p,unsigned key,unsigned block,unsigned item,int pivot)
{
    unsigned char t[16]={0}; w16(t,block>>16); w16(t+2,block);
    w16(t+4,pivot?1:item); w16(t+6,16|(pivot?0x2000:0)); w32(t+8,key); add(p,t,16);
}
static void pages(void)
{
    for(unsigned i=0;i<ROWS/PER_PAGE;++i) init_page(heap[i],QS_PAGE);
    for(unsigned i=0;i<ROWS;++i) {
        unsigned char t[432]={0}; unsigned off=24;
        w16(t+18,4); w16(t+20,0xb01); t[22]=24; t[23]=i%7?15:13;
        w32(t+off,i/3); off+=4;
        if(i%7) { w32(t+off,i+10); off+=4; }
        for(unsigned col=0;col<2;++col) {
            w32(t+off,(TEXT_BYTES+4)<<2); text((char *)t+off+4,i,col); off+=TEXT_BYTES+4;
        }
        add(heap[i/PER_PAGE],t,sizeof(t));
    }
    init_page(index_pages[1],QS_PAGE-16);
    w16(index_pages[1]+QS_PAGE-4,2); w32(index_pages[1]+QS_PAGE-8,1);
    for(unsigned leaf=0;leaf<LEAVES;++leaf) {
        unsigned first=leaf*LEAF_ROWS,end=first+LEAF_ROWS;
        if(end>ROWS) end=ROWS;
        unsigned char *p=index_pages[leaf+2]; init_page(p,QS_PAGE-16); w16(p+QS_PAGE-4,1);
        index_tuple(index_pages[1],leaf?first/3:0,leaf+2,0,1);
        if(leaf+1<LEAVES) { w32(p+QS_PAGE-12,leaf+3); index_tuple(p,end/3,0,0,1); }
        for(unsigned row=first;row<end;++row) index_tuple(p,row/3,row/PER_PAGE,row%PER_PAGE+1,0);
    }
}
static uint64_t complete(int64_t request,void *io)
{
    uint64_t op=(uint64_t)request&~CS_IO_VALUE_MASK,value=(uint64_t)request&CS_IO_VALUE_MASK;
    if(op==CS_NEED_STORE) {
        assert(value==QS_SORT_IO_BYTES); value=++ntokens;
        tokens=realloc(tokens,(ntokens+1)*sizeof(*tokens)); assert(tokens);
        tokens[value]=malloc(QS_SORT_IO_BYTES); assert(tokens[value]);
        memcpy(tokens[value],io,QS_SORT_IO_BYTES); ++live; ++stores;
    } else {
        assert(value && value<=ntokens && tokens[value]);
        if(op==CS_NEED_LOAD) { memcpy(io,tokens[value],QS_SORT_IO_BYTES); ++loads; }
        else { assert(op==CS_NEED_FREE); free(tokens[value]); tokens[value]=NULL; --live; }
    }
    return value;
}
static unsigned row_from_id(uint32_t id,unsigned column)
{
    uint64_t completion=0; const char *bytes=NULL; unsigned length=0;
    for(;;) {
        int64_t rc=qs_dictionary_pin(dictionary,reader,id,&bytes,&length,read_io,completion);
        if(!rc) break;
        assert(rc>QS_DICTIONARY_BUSY); completion=complete(rc,read_io);
    }
    unsigned row; memcpy(&row,bytes,4); assert(row<ROWS && length==TEXT_BYTES);
    char oracle[TEXT_BYTES]; text(oracle,row,column); assert(!memcmp(oracle,bytes,length));
    qs_dictionary_unpin(reader); return row;
}
/* The consumer only records IDs: inspecting bytes while the index owns an
 * unfinished dictionary operation would itself be an invalid concurrent use. */
struct record { uint32_t a,b; int64_t key,payload; unsigned isnull; };
static struct record records[ROWS*2];
static unsigned nrecords,consumer_phase;
static uint32_t stable_ids[ROWS][2];
static uint64_t consumer_token;
static void capture(struct qs_chunk *c,int probe)
{
    for(unsigned r=0;r<c->selected;++r) {
        unsigned row=qs_selection(c)[r],base=probe?1:0;
        assert(nrecords<ROWS*2);
        records[nrecords++]=(struct record){
            .a=qs_values(c)[(size_t)(base+2)*c->capacity+row],
            .b=qs_values(c)[(size_t)(base+3)*c->capacity+row],
            .key=qs_values(c)[row],
            .payload=qs_values(c)[(size_t)(base+1)*c->capacity+row],
            .isnull=!!(qs_nulls(c)[(size_t)(base+1)*((c->capacity+7)/8)+row/8]&(1U<<(row%8)))};
    }
}
int64_t pg_sina_filter_impl(struct femu_csd_args *a) { (void)a; assert(!"unexpected filter"); return -1; }
int64_t qs_join_consume(unsigned stage,struct qs_chunk *out,const struct qs_plan *p,
    struct qs_task *task,struct qs_join_control *control,struct femu_csd_args *a,int resume)
{
    (void)stage; (void)p; (void)task; (void)control;
    /* The same I/O AFDM serves the consumer and dictionary at different
     * times. The dictionary must not consume this completion token. */
    if(!consumer_phase) {
        memset(a->mr_addr[5],0x5a,QS_SORT_IO_BYTES); consumer_phase=1;
        return csNeedStore(QS_SORT_IO_BYTES);
    }
    assert(resume);
    if(consumer_phase==1) {
        consumer_token=csReentryValue(a); consumer_phase=2;
        return csNeedLoad(consumer_token);
    }
    assert(csReentryValue(a)==consumer_token);
    if(consumer_phase==2) {
        for(unsigned i=0;i<QS_SORT_IO_BYTES;++i) assert(((unsigned char *)a->mr_addr[5])[i]==0x5a);
        consumer_phase=3; return csNeedFree(consumer_token);
    }
    consumer_phase=0; capture(out,1); return 0;
}
static void verify(int ordered)
{
    memset(seen,0,sizeof(seen));
    for(unsigned i=0;i<nrecords;++i) {
        struct record *r=&records[i]; unsigned row=row_from_id(r->a,0);
        assert(row_from_id(r->b,1)==row && r->key==row/3);
        assert(r->isnull==(row%7==0)); if(!r->isnull) assert(r->payload==row+10);
        if(ordered) {
            assert(row==i); stable_ids[row][0]=r->a; stable_ids[row][1]=r->b;
        } else assert(r->a==stable_ids[row][0] && r->b==stable_ids[row][1]);
        ++seen[row];
    }
    for(unsigned i=0;i<ROWS;++i) assert(seen[i]==expected[i]);
}
static void namespace_reads(struct qs_task *task,char *io)
{
    assert(task->index_nreads);
    for(unsigned i=0;i<task->index_nreads;++i) {
        struct qs_index_read *r=&task->index_reads[i];
        assert(r->block<(r->heap?ROWS/PER_PAGE:LEAVES+2));
        memcpy(io+QS_SORT_IO_BYTES+(size_t)r->slot*QS_PAGE,
            r->heap?heap[r->block]:index_pages[r->block],QS_PAGE);
    }
}
static void run(cs_work_context *parent,int probe,uint64_t cancel)
{
    unsigned base=probe?1:0;
    struct qs_plan p={.nattrs=base+4,.ncols=base+4,.work_mem_parent=(uintptr_t)parent,
        .join={.relid=1,.nattrs=4,.build_key=base,.probe_key=0,.predicate=-1,.indexid=2,.indexroot=1,
            .index_key_bytes=4,.work_mem_bytes=256*1024}};
    for(unsigned i=0;i<p.ncols;++i) p.cols[i]=(struct qs_column){.slot=i,.type=i>=base+2?Q_STRING:Q_INTEGER,.align=4,.length=i>=base+2?-1:4};
    struct qs_join_control control={0}; struct qs_task task={.slot=probe?63:0,.workers=probe?64:1};
    assert(!qs_index_init(&p,&control)); assert(!qs_index_paged_dictionary(&control,dictionary));
    size_t io_bytes=QS_SORT_IO_BYTES+(size_t)QS_INDEX_CACHE*QS_PAGE;
    char *io=calloc(1,io_bytes),*spill=calloc(1,QS_SORT_IO_BYTES); assert(io && spill);
    struct qs_chunk *out=calloc(1,qs_chunk_bytes(17,p.ncols)); assert(out);
    out->capacity=17; out->ncols=p.ncols;
    struct qs_chunk *input=calloc(1,qs_chunk_bytes(1500,1)); assert(input);
    input->capacity=1500; input->ncols=1; input->rows=input->selected=1500;
    memset(expected,0,sizeof(expected));
    for(unsigned i=0;i<1500;++i) {
        unsigned key=i<1366?i:(i%2?7:100);
        qs_values(input)[i]=key; qs_selection(input)[i]=i;
        if(i%11==0) qs_nulls(input)[i/8]|=1U<<(i%8);
        else for(unsigned row=key*3;row<key*3+3 && row<ROWS;++row) ++expected[row];
    }
    if(!probe) for(unsigned i=0;i<ROWS;++i) expected[i]=1;
    void *mr[]={input,&p,&task,&control,out,io};
    long long sizes[]={qs_chunk_bytes(1500,1),sizeof(p),sizeof(task),sizeof(control),qs_chunk_bytes(17,p.ncols),io_bytes};
    struct femu_csd_args a={.numr=6,.mr_addr=mr,.mr_len=sizes};
    void *ordered_mr[]={spill}; long long ordered_sizes[]={QS_SORT_IO_BYTES};
    struct femu_csd_args ordered={.numr=1,.mr_addr=ordered_mr,.mr_len=ordered_sizes};
    unsigned waits=0; int resume=0; nrecords=0;
    for(unsigned attempts=0;;++attempts) {
        assert(attempts<1000000);
        int64_t rc=probe?qs_index_probe(0,input,&p,&task,&control,&a,resume):
            qs_index_scan_paged(out,&task,&control,io,ROWS/PER_PAGE,LEAVES+2,0,&ordered);
        a.cparam2=ordered.cparam2=0;
        if(!rc) {
            if(probe || !out->rows) break;
            capture(out,0); out->rows=out->selected=0; continue;
        }
        assert(rc>0); ++waits;
        if(rc==QS_INDEX_WAIT && task.index_pending && !qs_index_dictionary_pending(&control,task.slot)) namespace_reads(&task,io);
        else {
            assert((qs_index_dictionary_pending(&control,task.slot) || consumer_phase) && rc>QS_DICTIONARY_BUSY);
            uint64_t value=complete(rc,probe?(void *)io:(void *)spill);
            a.cparam2=ordered.cparam2=value;
            if(((uint64_t)rc&~CS_IO_VALUE_MASK)==cancel) {
                /* I/O completed, but its token has not reached the CSF. */
                qs_index_close(&control);
                free(input); free(out); free(io); free(spill); return;
            }
        }
        resume=1;
    }
    assert(!cancel && !consumer_phase); qs_index_close(&control); verify(!probe);
    printf("dictionary index %s rows=%u waits=%u stores=%u loads=%u\n",probe?"probe":"ordered",nrecords,waits,stores,loads);
    free(input); free(out); free(io); free(spill);
}
int main(void)
{
    assert(!cs_work_init(LIMIT)); pages();
    cs_work_context *parent=cs_work_begin_limit(LIMIT); assert(parent);
    dictionary=qs_dictionary_create(parent); assert(dictionary);
    reader=qs_dictionary_access_create(dictionary); assert(reader);
    run(parent,0,0); run(parent,1,0);
    assert(stores && loads);
    cs_work_context_stats stats; assert(!cs_work_get_context_stats(parent,&stats));
    assert(stats.peak_used_bytes<=LIMIT);
    struct qs_dictionary_stats ds; qs_dictionary_get_stats(dictionary,&ds);
    assert(ds.stores && ds.loads);
    printf("dictionary index strings=%llu bytes=%llu stores=%llu loads=%llu peak=%zu limit=%u\n",
        (unsigned long long)ds.strings,(unsigned long long)ds.bytes,
        (unsigned long long)ds.stores,(unsigned long long)ds.loads,stats.peak_used_bytes,LIMIT);
    uint64_t completion=0;
    for(;;) {
        int64_t rc=qs_dictionary_close(dictionary,reader,read_io,completion);
        if(!rc) break;
        assert(rc>QS_DICTIONARY_BUSY); completion=complete(rc,read_io);
    }
    assert(!live); free(tokens); tokens=NULL; ntokens=stores=loads=0;
    assert(!cs_work_get_context_stats(parent,&stats) && !stats.used_bytes);
    assert(!cs_work_end(parent));
    struct cs_work_stats total; assert(!cs_work_get_stats(&total) && !total.used_bytes && !total.active_contexts);
    for(unsigned load=0;load<2;++load) {
        parent=cs_work_begin_limit(LIMIT); assert(parent);
        dictionary=qs_dictionary_create(parent); assert(dictionary);
        reader=qs_dictionary_access_create(dictionary); assert(reader);
        if(load) run(parent,0,0);
        run(parent,0,load?CS_NEED_LOAD:CS_NEED_STORE);
        qs_dictionary_abort(dictionary);
        /* The SNIA query scope owns completed but unacknowledged tokens. */
        for(unsigned i=1;i<=ntokens;++i) free(tokens[i]);
        free(tokens); tokens=NULL; ntokens=live=stores=loads=0;
        assert(!cs_work_get_context_stats(parent,&stats) && !stats.used_bytes);
        assert(!cs_work_end(parent));
        assert(!cs_work_get_stats(&total) && !total.used_bytes && !total.active_contexts);
    }
    puts("PASS paged dictionary index decode, NULL retention, duplicate keys, consumer I/O and cancellation cleanup");
    return 0;
}
