#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "query_eval.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
typedef int64_t (*function)(struct femu_csd_args *);
static function load(const char *path,const char *name)
{
    void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL); assert(library); function fn=(function)dlsym(library,name); assert(fn); return fn;
}
static void run(function fn,void **addresses,long long *lengths,unsigned n)
{
    struct femu_csd_args a={.numr=n,.mr_addr=addresses,.mr_len=lengths}; assert(fn(&a)==0);
}
int main(int argc,char **argv)
{
    assert(argc==5);
    function filter=load(argv[1],"pg_sina_filter"),sink=load(argv[2],"pg_sina_aggregate_sink");
    function source=load(argv[2],"pg_sina_aggregate_source"),sort=load(argv[3],"pg_sina_sort"),limit=load(argv[4],"pg_sina_limit");
    struct qs_plan p={.nattrs=2,.ncols=2,.nexprs=4,.nkeys=1,.naggs=4,.nout=5,.norder=1,.predicate=3,.limit=1,.offset=1};
    p.cols[0]=(struct qs_column){.slot=0,.type=Q_INTEGER};p.cols[1]=(struct qs_column){.slot=1,.type=Q_DECIMAL,.scale=2};
    p.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=0};
    p.exprs[1]=(struct qs_expr){.op=Q_COLUMN,.type=Q_DECIMAL,.column=1,.scale=2,.dscale=2};
    p.exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER};
    p.exprs[3]=(struct qs_expr){.op=Q_GT,.type=Q_BOOLEAN,.left=0,.right=2};
    p.keys[0]=0;
    p.aggs[0]=(struct qs_agg){.kind=Q_SUM,.expr=1};p.aggs[1]=(struct qs_agg){.kind=Q_AVG,.expr=1};
    p.aggs[2]=(struct qs_agg){.kind=Q_COUNT_STAR,.expr=-1};p.aggs[3]=(struct qs_agg){.kind=Q_COUNT,.expr=1};
    p.outputs[0]=(struct qs_output){.index=0};
    for(unsigned i=1;i<5;++i) p.outputs[i]=(struct qs_output){.index=i-1,.aggregate=1};
    p.order[0]=(struct qs_order){.output=0,.descending=1};
    struct qs_group group={.count=4}; struct qs_task tasks[4]={{0}};
    for(unsigned worker=0;worker<4;++worker) {
        size_t bytes=qs_chunk_bytes(4,2); struct qs_chunk *chunk=calloc(1,bytes); assert(chunk);
        chunk->capacity=4;chunk->ncols=2;chunk->rows=worker==2?2:1;chunk->selected=chunk->rows;
        int64_t key=worker%2?2:1;
        qs_dscales(chunk)[4]=2;qs_dscales(chunk)[5]=2;
        qs_values(chunk)[0]=key;qs_values(chunk)[4]=worker==0?1000:worker==1?300:worker==2?0:900;
        if(worker==2) {qs_nulls(chunk)[1]|=1;qs_values(chunk)[1]=1;qs_values(chunk)[5]=2000;}
        qs_selection(chunk)[0]=0;qs_selection(chunk)[1]=1;
        void *fmem[]={chunk,&p};long long flen[]={bytes,sizeof(p)};run(filter,fmem,flen,2);
        void *mem[]={chunk,&p,&tasks[worker]};long long len[]={bytes,sizeof(p),sizeof(tasks[0])};run(sink,mem,len,3);
        group.states[worker]=tasks[worker].state; free(chunk);
    }
    struct qs_result *result=calloc(1,sizeof(*result)); assert(result);
    void *out[]={&group,result};long long olen[]={sizeof(group),sizeof(*result)};run(source,out,olen,2);
    assert(result->rows==2 && !group.states[0] && result->metrics.scanned==5);
    void *order[]={result,&p};long long orderlen[]={sizeof(*result),sizeof(p)};run(sort,order,orderlen,2);run(limit,order,orderlen,2);
    assert(result->rows==1 && result->begin==1);
    struct qs_cell *cells=result->data[1].cells;
    assert(cells[0].value==1 && cells[1].value==3000 && cells[1].scale==2);
    assert(cells[2].value==(__int128)15*10000000000000000ULL && cells[2].scale==16);
    assert(cells[3].value==3 && cells[4].value==2);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
    struct qs_cell average; assert(!qs_average((struct qs_cell){.value=1234,.scale=2,.dscale=2},3,&average));
    assert(average.scale==16 && average.value==41133333333333333LL);
    int cmp;assert(!qs_compare((struct qs_cell){.scale=2},(struct qs_cell){0},&cmp) && !cmp);
    assert(!qs_compare((struct qs_cell){.value=-10,.scale=2},(struct qs_cell){.value=-1},&cmp) && cmp>0);
    __int128 largest=(__int128)(((unsigned __int128)1<<127)-1),large=9;
    for(unsigned i=0;i<37;++i) large*=10;
    assert(!qs_compare((struct qs_cell){.value=largest,.scale=1},(struct qs_cell){.value=large},&cmp) && cmp<0);
    free(result); puts("PASS vector filter, NULL aggregates, four-state merge, exact AVG, sort/limit and workmem cleanup"); return 0;
}
