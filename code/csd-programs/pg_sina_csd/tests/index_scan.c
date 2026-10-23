/* Real PG12 page layouts: internal pivots, composite-prefix duplicates across
 * leaves, NULL/unmatched/negative keys and asynchronous page continuations. */
#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "query.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int64_t (*function)(struct femu_csd_args *);
static unsigned key_bytes=4;
static int empty_index,large_index,retained_window,pair_keys;
static void (*heap_window)(struct qs_join_control *,unsigned,const void *,uint32_t,uint32_t);
static unsigned char index_pages[4][QS_PAGE],heap[256][QS_PAGE];
static unsigned test_slot;
static void w16(void *p,uint16_t v) { memcpy(p,&v,2); }
static void w32(void *p,uint32_t v) { memcpy(p,&v,4); }
static void init_page(unsigned char *p,unsigned special)
{ memset(p,0,QS_PAGE); w16(p+10,4); w16(p+12,24); w16(p+14,special); w16(p+16,special); w16(p+18,QS_PAGE|4); }
static void add(unsigned char *p,const void *tuple,unsigned len)
{
    uint16_t lower,upper; memcpy(&lower,p+12,2); memcpy(&upper,p+14,2); upper-=len;
    memcpy(p+upper,tuple,len); w32(p+lower,upper|(1U<<15)|(len<<17)); w16(p+12,lower+4); w16(p+14,upper);
}
static void index_tuple(unsigned char *p,int key,unsigned block,unsigned item,int pivot)
{
    unsigned size=pair_keys && key_bytes==8?24:16;
    unsigned char t[24]={0}; w16(t,block>>16); w16(t+2,block); w16(t+4,pivot?1:item); w16(t+6,size|(pivot?0x2000:0)); if(key_bytes==2) w16(t+8,key); else if(key_bytes==4) w32(t+8,key); else { int64_t v=key; memcpy(t+8,&v,8); }
    if(key_bytes<8) w32(t+12,item);
    else if(pair_keys) w32(t+16,item);
    add(p,t,size);
}
static void pages(void)
{
    for(unsigned i=0;i<2;++i) init_page(heap[i],QS_PAGE);
    for(unsigned i=0;i<8;++i) {
        unsigned char t[32]={0}; w16(t+18,2); w16(t+20,0xb00); t[22]=24;
        w32(t+24,i<2?-2:i<6?7:9); w32(t+28,10+i); add(heap[i/4],t,32);
    }
    for(unsigned i=1;i<4;++i) init_page(index_pages[i],QS_PAGE-16);
    w16(index_pages[1]+QS_PAGE-4,2); w32(index_pages[1]+QS_PAGE-8,1);
    index_tuple(index_pages[1],0,2,0,1); index_tuple(index_pages[1],7,3,0,1);
    w16(index_pages[2]+QS_PAGE-4,1); w32(index_pages[2]+QS_PAGE-12,3);
    index_tuple(index_pages[2],7,0,0,1); /* high key */
    for(unsigned i=0;i<4;++i) index_tuple(index_pages[2],i<2?-2:7,i/4,i%4+1,0);
    w16(index_pages[3]+QS_PAGE-4,1);
    for(unsigned i=4;i<8;++i) index_tuple(index_pages[3],i<6?7:9,i/4,i%4+1,0);
}
static void large_pages(void)
{
    for(unsigned i=1;i<4;++i) init_page(index_pages[i],QS_PAGE-16);
    w16(index_pages[1]+QS_PAGE-4,2); w32(index_pages[1]+QS_PAGE-8,1);
    index_tuple(index_pages[1],0,2,0,1); index_tuple(index_pages[1],128,3,0,1);
    w16(index_pages[2]+QS_PAGE-4,1); w32(index_pages[2]+QS_PAGE-12,3);
    index_tuple(index_pages[2],128,0,0,1); w16(index_pages[3]+QS_PAGE-4,1);
    for(unsigned i=0;i<256;++i) {
        init_page(heap[i],QS_PAGE);
        unsigned char t[32]={0}; w16(t+18,2); w16(t+20,0xb00); t[22]=24;
        w32(t+24,i); w32(t+28,10+i); add(heap[i],t,32);
        index_tuple(index_pages[i<128?2:3],i,i,1,0);
    }
}
static void check(function build,function probe,function close,function source,int filtered,int cancel)
{
    struct qs_plan p={.having=-1,.nattrs=3,.ncols=3,.nexprs=3,.naggs=2,.nout=2,.limit=-1,.predicate=-1,.work_mem_bytes=1024*1024,
        .join={.relid=1,.nattrs=2,.build_key=1,.probe_key=0,.predicate=-1,.work_mem_bytes=1024*1024,.indexid=2,.indexroot=empty_index==1?0:1,.index_key_bytes=key_bytes}};
    for(unsigned i=0;i<3;++i) p.cols[i]=(struct qs_column){.slot=i,.length=4,.align=4,.type=Q_INTEGER};
    p.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=2};
    p.exprs[1]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=13};
    p.exprs[2]=(struct qs_expr){.op=Q_GE,.type=Q_BOOLEAN,.left=0,.right=1};
    if(filtered) p.join.predicate=2;
    p.aggs[0]=(struct qs_agg){.kind=Q_COUNT_STAR,.expr=-1}; p.aggs[1]=(struct qs_agg){.kind=Q_SUM,.expr=0};
    p.outputs[0]=(struct qs_output){.aggregate=1,.index=0}; p.outputs[1]=(struct qs_output){.aggregate=1,.index=1};
    size_t io_bytes=QS_SORT_IO_BYTES+(size_t)QS_INDEX_CACHE*QS_PAGE;
    void *io=calloc(1,io_bytes); struct qs_join_control control={0};
    struct qs_chunk *c=calloc(1,qs_chunk_bytes(513,1)),*out=calloc(1,qs_chunk_bytes(256,3)); assert(c && out && io);
    c->capacity=513; c->ncols=1; c->rows=c->selected=513;
    int keys[]={-2,7,7,9,8,0};
    unsigned long expected_count=0,expected_sum=0;
    for(unsigned i=0;i<c->rows;++i) {
        int key=large_index?(int)(i%256):keys[i%6]; qs_values(c)[i]=key; qs_selection(c)[i]=i;
        if(i%11==0) qs_nulls(c)[i/8]|=1U<<(i%8);
        else if(!empty_index) for(unsigned j=0;j<(large_index?256:8);++j) if((large_index?(int)j:j<2?-2:j<6?7:9)==key && (!filtered || j+10>=13)) { ++expected_count; expected_sum+=j+10; }
    }
    out->capacity=256; out->ncols=3;
    struct qs_task task={.workers=test_slot+1,.slot=test_slot};
    void *mr[]={c,&p,&control,io}; long long len[]={qs_chunk_bytes(513,1),sizeof(p),sizeof(control),io_bytes};
    struct femu_csd_args a={.numr=4,.mr_addr=mr,.mr_len=len}; assert(!build(&a)); assert(control.indexed);
    if(retained_window) heap_window(&control,test_slot,heap[retained_window==2?1:0],retained_window==2?1:0,retained_window==2?1:large_index?256:2);
    void *args[]={c,&p,&task,&control,out,io}; long long sizes[]={len[0],sizeof(p),sizeof(task),sizeof(control),qs_chunk_bytes(256,3),io_bytes};
    a=(struct femu_csd_args){.numr=6,.mr_addr=args,.mr_len=sizes};
    unsigned resumes=0; int failed=0;
    do {
        int64_t value=probe(&a);
        if(value) { assert(cancel==2 && value==-2); failed=1; break; }
        if(cancel==1) break;
        for(unsigned i=0;i<task.index_nreads;++i) {
            const struct qs_index_read *r=&task.index_reads[i];
            if(retained_window) assert(!r->heap || (retained_window==2 && r->block==0));
            assert(r->slot<QS_INDEX_CACHE && r->block<(r->heap?(large_index?256:2):4));
            unsigned char *dest=(void *)((char *)io+QS_SORT_IO_BYTES+(size_t)r->slot*QS_PAGE);
            memcpy(dest,r->heap?heap[r->block]:index_pages[r->block],QS_PAGE);
            if(cancel==2 && !r->heap) w16(dest+18,0);
        }
        assert(++resumes<5000);
    } while(task.index_pending);
    assert(cancel!=2 || failed);
    struct qs_group group={.count=1,.states={task.state}};
    struct qs_result *result=calloc(1,sizeof(*result));
    void *sm[]={&group,result}; long long sl[]={sizeof(group),sizeof(*result)};
    struct femu_csd_args source_args={.numr=cancel?1:2,.mr_addr=sm,.mr_len=sl}; assert(!source(&source_args));
    if(!cancel) { assert(!result->status && result->rows==1); assert(result->data[0].cells[0].value==expected_count); assert(result->data[0].cells[1].value==expected_sum); assert(task.index_tuples>expected_count || !filtered); }
    void *cm[]={&control}; long long cl[]={sizeof(control)};
    struct femu_csd_args close_args={.numr=1,.mr_addr=cm,.mr_len=cl}; assert(!close(&close_args));
    free(result); free(c); free(out); free(io);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
static void ordered_scan(void *library,unsigned rows,int empty,int flags)
{
    int (*init)(const struct qs_plan *,struct qs_join_control *)=dlsym(library,"qs_index_init");
    int64_t (*scan)(struct qs_chunk *,struct qs_task *,struct qs_join_control *,void *,unsigned,unsigned,int)=dlsym(library,"qs_index_scan");
    void (*close)(struct qs_join_control *)=dlsym(library,"qs_index_close");
    assert(init && scan && close);
    struct qs_plan p={.nattrs=2,.ncols=2,.join={.nattrs=2,.build_key=1,.index_key_bytes=key_bytes,.indexroot=empty?0:1,.work_mem_bytes=QS_INDEX_WORK_BYTES}};
    for(unsigned i=0;i<2;++i) p.cols[i]=(struct qs_column){.slot=i,.length=4,.align=4,.type=Q_INTEGER};
    if((flags&4) && !(flags&8)) { p.cols[1].slot=-1; p.ncols=1; }
    if(flags&8) p.join.build_key2=2;
    struct qs_join_control control={0}; struct qs_task task={0}; assert(!init(&p,&control));
    void (*range)(struct qs_join_control *,unsigned,unsigned)=dlsym(library,"qs_index_scan_range"); assert(range);
    if(flags&16) range(&control,1,3);
    struct qs_chunk *out=calloc(1,qs_chunk_bytes(17,2)); assert(out); out->capacity=17; out->ncols=p.ncols;
    char *io=calloc(1,QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE); assert(io);
    unsigned seen=0;
    for(unsigned iteration=0;;++iteration) {
        assert(iteration<10000);
        int64_t status=scan(out,&task,&control,io,rows==8?2:256,4,flags);
        if(status==QS_INDEX_WAIT) {
            for(unsigned i=0;i<task.index_nreads;++i) {
                struct qs_index_read *r=&task.index_reads[i];
                if(flags&4) assert(!r->heap);
                memcpy(io+QS_SORT_IO_BYTES+(size_t)r->slot*QS_PAGE,r->heap?heap[r->block]:index_pages[r->block],QS_PAGE);
            }
            continue;
        }
        assert(!status);
        if(!out->rows) {
            if((flags&16) && seen==4) { range(&control,3,4); continue; }
            break;
        }
        for(unsigned i=0;i<out->rows;++i) {
            if((flags&4) && ((rows==9 && seen==8) || (rows==10 && seen==9))) assert(qs_nulls(out)[i/8]&(1U<<(i%8)));
            else if(flags&4) assert(qs_values(out)[i]==(rows<=10?(seen<2?-2:seen<6?7:seen<8?9:99):(int)seen));
            else assert(qs_values(out)[out->capacity+i]==10+seen);
            if((flags&8) && rows==10 && seen==8) assert(qs_nulls(out)[(out->capacity+7)/8+i/8]&(1U<<(i%8)));
            else if(flags&8) assert(qs_values(out)[out->capacity+i]==(rows==10 && seen==9?42:seen%4+1));
            ++seen;
        }
        out->rows=out->selected=0;
    }
    assert(seen==(empty?0:rows)); close(&control); free(out); free(io);
}
int main(int argc,char **argv)
{
    assert(argc==3 || argc==4); assert(!cs_work_init((argc==4?256U:4U)*1024*1024));
    void *join=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL),*agg=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL); assert(join && agg);
    function build=(function)dlsym(join,"pg_sina_join_build"),probe=(function)dlsym(join,"pg_sina_join_probe"),close=(function)dlsym(join,"pg_sina_join_close"),source=(function)dlsym(agg,"pg_sina_aggregate_source"); assert(build && probe && close && source);
    heap_window=(void (*)(struct qs_join_control *,unsigned,const void *,uint32_t,uint32_t))dlsym(join,"qs_index_heap_window"); assert(heap_window);
    key_bytes=4; pages(); ordered_scan(join,8,0,0); ordered_scan(join,8,1,2); ordered_scan(join,8,0,4);
    ordered_scan(join,8,0,12);
    ordered_scan(join,8,0,20); ordered_scan(join,8,0,28);
    /* Physical ranges do not follow downlinks through ignored pages. */
    w16(index_pages[1]+QS_PAGE-4,4); ordered_scan(join,8,0,20);
    w16(index_pages[1]+QS_PAGE-4,16); ordered_scan(join,8,0,28); pages();
    unsigned char nullable_pair[24]={0}; w16(nullable_pair+6,0x8000|24);
    nullable_pair[8]=1; w32(nullable_pair+16,99); add(index_pages[3],nullable_pair,24);
    nullable_pair[8]=2; w32(nullable_pair+16,42); add(index_pages[3],nullable_pair,24);
    ordered_scan(join,10,0,12); ordered_scan(join,10,0,28); pages();
    unsigned char null_key[16]={0}; w16(null_key+6,0x8000|16); add(index_pages[3],null_key,16);
    ordered_scan(join,9,0,4);
    large_pages(); ordered_scan(join,256,0,0); ordered_scan(join,256,0,2); ordered_scan(join,256,0,4);
    for(key_bytes=2;key_bytes<=8;key_bytes*=2) {
        pair_keys=1; pages(); ordered_scan(join,8,0,12); ordered_scan(join,8,0,28); pair_keys=0;
        pages(); ordered_scan(join,8,0,4);
        for(retained_window=1;retained_window<=2;++retained_window) {
            check(build,probe,close,source,0,0); check(build,probe,close,source,1,0);
        }
        retained_window=0; check(build,probe,close,source,0,0); check(build,probe,close,source,1,0); check(build,probe,close,source,0,1); check(build,probe,close,source,0,2);
        unsigned char tuple[32]; memcpy(tuple,heap[0]+QS_PAGE-32,32); w16(tuple+18,0x8002);
        add(heap[0],tuple,32); w32(heap[0]+24,5|(2U<<15));
        check(build,probe,close,source,0,0); /* HOT root -> heap-only tuple */
    }
    key_bytes=4; empty_index=1; check(build,probe,close,source,0,0);
    empty_index=2; init_page(index_pages[1],QS_PAGE-16); w16(index_pages[1]+QS_PAGE-4,1);
    check(build,probe,close,source,0,0);
    empty_index=0; large_index=1; large_pages();
    check(build,probe,close,source,0,0); check(build,probe,close,source,1,0);
    if(argc==4) { test_slot=strtoul(argv[3],NULL,10); assert(test_slot<QS_WORKERS);
        check(build,probe,close,source,0,0); check(build,probe,close,source,1,0); }
    puts("PASS PG12 index prefix lookup, duplicate leaf boundaries, NULL, filter and continuation cleanup");
    dlclose(agg); dlclose(join); return 0;
}
