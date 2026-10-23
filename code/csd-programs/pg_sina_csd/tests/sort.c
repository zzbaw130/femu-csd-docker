/* Device external sort with an in-memory NVM emulator. Only the test owns
 * host allocations; all operator runs/scratch must fit the shared workmem. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "query.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int64_t (*function)(struct femu_csd_args *);
static void *blocks[8192];
static unsigned next_token=1,stored,loaded,workers=4;
static size_t capacity=8U*1024*1024,budget=8U*1024*1024;
static int64_t run(function f,struct femu_csd_args *args)
{
    args->cparam2=0;
    for(;;) {
        int64_t value=f(args);
        if(value<0) { struct cs_work_stats stats; assert(!cs_work_get_stats(&stats));
            fprintf(stderr,"sort status=%lld args=%d workers=%u used=%zu peak=%zu tokens=%u\n",(long long)value,args->numr,workers,stats.used_bytes,stats.peak_used_bytes,next_token); }
        assert(value>=0);
        uint64_t action=(uint64_t)value&~CS_IO_VALUE_MASK,token=(uint64_t)value&CS_IO_VALUE_MASK;
        void *io=args->mr_addr[args->numr-1];
        if(action==CS_NEED_STORE) {
            assert(token==QS_SORT_IO_BYTES && next_token<8192);
            token=next_token++; blocks[token]=malloc(QS_SORT_IO_BYTES); assert(blocks[token]);
            memcpy(blocks[token],io,QS_SORT_IO_BYTES); ++stored;
        } else if(action==CS_NEED_LOAD) {
            assert(token && token<next_token && blocks[token]);
            memcpy(io,blocks[token],QS_SORT_IO_BYTES); ++loaded;
        } else if(action==CS_NEED_FREE) {
            assert(token && token<next_token && blocks[token]); free(blocks[token]); blocks[token]=NULL;
        } else return value;
        args->cparam2=token;
    }
}
static int host_order(const void *left,const void *right)
{
    unsigned a=*(const unsigned *)left,b=*(const unsigned *)right;
    if((a%11==0)!=(b%11==0)) return a%11==0?-1:1;
    return a%11==0?(a<b)-(a>b):(a>b)-(a<b);
}
static void check(function sink,function source,unsigned rows,int64_t limit,int64_t offset,int cancel,int profile)
{
    next_token=1; stored=loaded=0;
    unsigned *expected=malloc((rows?rows:1)*sizeof(*expected)); assert(expected);
    for(unsigned i=0;i<rows;++i) expected[i]=i;
    qsort(expected,rows,sizeof(*expected),host_order);
    struct qs_plan p={.having=-1,.nattrs=3,.ncols=3,.nexprs=3,.nout=3,.norder=2,.ordered_rows=1,.work_mem_bytes=budget/workers,.predicate=-1,.limit=limit,.offset=offset};
    for(unsigned i=0;i<3;++i) {
        p.cols[i]=(struct qs_column){.slot=i,.type=i==1?Q_DECIMAL:Q_INTEGER,.scale=i==1?2:0};
        p.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.column=i,.type=p.cols[i].type,.scale=p.cols[i].scale};
        p.outputs[i].index=i;
    }
    p.order[0]=(struct qs_order){.output=0,.nulls_first=1}; p.order[1]=(struct qs_order){.output=2};
    size_t bytes=qs_chunk_bytes(1001,3); struct qs_chunk *c=calloc(1,bytes); assert(c);
    c->capacity=1001; c->ncols=3;
    void *io=calloc(1,QS_SORT_IO_BYTES); assert(io);
    struct qs_task tasks[QS_WORKERS]={{0}}; struct qs_profile counters[QS_WORKERS]={0};
    unsigned chunk=0;
    for(unsigned first=0;first<rows;first+=1001) {
        unsigned n=rows-first; if(n>1001) n=1001;
        memset(qs_nulls(c),0,((c->capacity+7)/8)*3); c->rows=c->selected=n;
        for(unsigned i=0;i<n;++i) {
            unsigned key=(uint64_t)(rows-1-first-i)*1009%rows; qs_values(c)[i]=key;
            qs_values(c)[c->capacity+i]=(int64_t)key*123; qs_values(c)[2*c->capacity+i]=-(int64_t)key;
            qs_dscales(c)[c->capacity+i]=key%3;
            qs_dscales(c)[2*c->capacity+i]=key%5;
            if(key%11==0) qs_nulls(c)[i/8]|=1U<<(i%8);
            qs_selection(c)[i]=i;
        }
        unsigned worker=chunk++%workers; tasks[worker].workers=workers; tasks[worker].input_bytes=n*24;
        void *mr[]={c,&p,&tasks[worker],profile?(void *)&counters[worker]:io,io};
        long long len[]={bytes,sizeof(p),sizeof(tasks[0]),profile?sizeof(counters[0]):QS_SORT_IO_BYTES,QS_SORT_IO_BYTES};
        struct femu_csd_args a={.numr=profile?5:4,.mr_addr=mr,.mr_len=len,.cparam1=profile};
        assert(!run(sink,&a));
    }
    struct qs_group group={.count=workers};
    for(unsigned i=0;i<workers;++i) group.states[i]=tasks[i].state;
    struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    void *mr[]={&group,out,profile?(void *)&counters[0]:io,io};
    long long len[]={sizeof(group),sizeof(*out),profile?sizeof(counters[0]):QS_SORT_IO_BYTES,QS_SORT_IO_BYTES};
    struct femu_csd_args a={.numr=profile?4:3,.mr_addr=mr,.mr_len=len,.cparam1=profile};
    unsigned seen=0;
    do {
        assert(!run(source,&a));
        for(unsigned i=0;i<out->rows;++i) {
            assert(seen+(uint64_t)offset<rows);
            unsigned key=expected[seen++ + offset]; const struct qs_cell *cells=out->data[i].cells;
            assert(cells[0].isnull==(key%11==0));
            if(!cells[0].isnull) assert(cells[0].value==key);
            assert(cells[1].value==(int64_t)key*123 && cells[1].scale==2 && cells[1].dscale==key%3);
            assert(cells[2].value==-(int64_t)key);
            assert(cells[2].dscale==key%5);
        }
        if(cancel && !out->finished) {
            void *drop[]={&group,io}; long long size[]={sizeof(group),QS_SORT_IO_BYTES};
            if(cancel==2) {
                /* Failed batch scope already reclaimed its NVM extents. RAM
                 * cleanup must work without asking to free stale IDs again. */
                for(unsigned i=1;i<next_token;++i) { free(blocks[i]); blocks[i]=NULL; }
            }
            struct femu_csd_args d={.numr=cancel==2?1:2,.mr_addr=drop,.mr_len=size};
            assert(!run(source,&d)); break;
        }
    } while(!out->finished);
    if(!cancel) {
        uint64_t count=rows>(uint64_t)offset?rows-offset:0;
        if(limit>=0 && count>(uint64_t)limit) count=limit;
        assert(seen==count);
    }
    if(limit<0 && (uint64_t)rows*3*sizeof(struct qs_cell)>budget) assert(stored && loaded);
    if(limit<0 && (uint64_t)rows*3*sizeof(struct qs_cell)<budget/2) assert(!stored && !loaded);
    if(limit>=0) assert(!stored && !loaded);
    /* These monotonic runs should retain full I/O blocks, even at 4 MiB. */
    if(limit<0 && rows>1000000 && !cancel && stored) assert(loaded<=3*stored);
    if(profile) assert(counters[0].operators[QS_SORT_SOURCE].calls);
    for(unsigned i=1;i<next_token;++i) assert(!blocks[i]);
    for(unsigned i=0;i<workers;++i) assert(!group.states[i]);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats));
    assert(!stats.used_bytes && !stats.active_contexts && stats.peak_used_bytes<=budget && stats.peak_used_bytes<=capacity);
    printf("rows=%u pool=%zu budget=%zu peak=%zu store=%u load=%u\n",rows,capacity,budget,stats.peak_used_bytes,stored,loaded);
    free(out); free(io); free(c); free(expected);
}
int main(int argc,char **argv)
{
    assert(argc>=2 && argc<=5);
    if(argc>=3) capacity=(size_t)strtoul(argv[2],NULL,10)*1024*1024;
    budget=argc>=4?(size_t)strtoul(argv[3],NULL,10)*1024*1024:capacity;
    if(argc==5) workers=strtoul(argv[4],NULL,10);
    assert(workers && workers<=QS_WORKERS);
    if(budget>capacity) budget=capacity;
    assert(capacity && budget && !cs_work_init(capacity));
    void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); assert(so);
    function sink=(function)dlsym(so,"pg_sina_sort_sink"),source=(function)dlsym(so,"pg_sina_sort_source"); assert(sink && source);
    check(sink,source,0,-1,0,0,0);
    check(sink,source,5003,-1,0,0,0);
    check(sink,source,262147,-1,0,0,1);
    check(sink,source,262147,17,5,0,1);
    check(sink,source,262147,-1,0,1,0);
    check(sink,source,262147,-1,0,2,0);
    check(sink,source,1048583,-1,0,0,0); /* Merge read buffers exceed workmem. */
    check(sink,source,5003,INT64_MAX,INT64_MAX,0,0);
    dlclose(so);
    puts("PASS external row sort, multiple sinks, memory pressure/reentry, NULL/precision, chunked output, TopN/OFFSET and abort cleanup");
    return 0;
}
