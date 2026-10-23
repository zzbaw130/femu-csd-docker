#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "query_eval.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
static int profiling;
static struct qs_profile profile;
typedef int64_t (*function)(struct femu_csd_args *);
static function load(const char *path,const char *name)
{
    void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL); assert(library); function fn=(function)dlsym(library,name); assert(fn); return fn;
}
static void run(function fn,void **addresses,long long *lengths,unsigned n)
{
    void *mr[5]; long long len[5];
    for(unsigned i=0;i<n;++i) { mr[i]=addresses[i]; len[i]=lengths[i]; }
    if(profiling) { mr[n]=&profile; len[n]=sizeof(profile); }
    struct femu_csd_args a={.numr=n+profiling,.mr_addr=mr,.mr_len=len,.cparam1=profiling};
    assert(fn(&a)==0 && a.numr==(int)(n+profiling));
}
int main(int argc,char **argv)
{
    assert(argc==6 || argc==7 || argc==8); profiling=argc==7;
    if(profiling) assert(!strcmp(argv[6],"--profile"));
    unsigned workers=4;
    if(argc==8) { assert(!strcmp(argv[6],"--workers")); workers=strtoul(argv[7],NULL,10); }
    assert(workers>=4 && workers<=QS_WORKERS && !(workers%4));
    function scan=load(argv[1],"pg_sina_scan"),filter=load(argv[2],"pg_sina_filter"),sink=load(argv[3],"pg_sina_aggregate_sink");
    function source=load(argv[3],"pg_sina_aggregate_source"),sort=load(argv[4],"pg_sina_sort"),limit=load(argv[5],"pg_sina_limit");
    struct qs_plan p={.having=-1,.nattrs=2,.ncols=2,.nexprs=4,.nkeys=1,.naggs=4,.nout=5,.norder=1,.predicate=3,.limit=1,.offset=1};
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
    unsigned char page[QS_PAGE]={0};
    uint16_t fields[]={4,24,QS_PAGE,QS_PAGE,QS_PAGE|4}; memcpy(page+10,fields,sizeof(fields));
    size_t empty_bytes=qs_chunk_bytes(4,2); struct qs_chunk *empty=calloc(1,empty_bytes); assert(empty);
    empty->capacity=4; empty->ncols=2; struct qs_task scan_task={.input_bytes=QS_PAGE};
    void *scan_mem[]={page,&p,empty,&scan_task}; long long scan_len[]={sizeof(page),sizeof(p),empty_bytes,sizeof(scan_task)};
    run(scan,scan_mem,scan_len,4); assert(!empty->rows); free(empty);
    struct qs_group group={.count=workers}; struct qs_task tasks[QS_WORKERS]={{0}};
    for(unsigned worker=0;worker<workers;++worker) {
        size_t bytes=qs_chunk_bytes(4,2); struct qs_chunk *chunk=calloc(1,bytes); assert(chunk);
        chunk->capacity=4;chunk->ncols=2;unsigned lane=worker%4; chunk->rows=lane==2?2:1;chunk->selected=chunk->rows;
        int64_t key=worker%2?2:1;
        qs_dscales(chunk)[4]=2;qs_dscales(chunk)[5]=2;
        qs_values(chunk)[0]=key;qs_values(chunk)[4]=lane==0?1000:lane==1?300:lane==2?0:900;
        if(lane==2) {qs_nulls(chunk)[1]|=1;qs_values(chunk)[1]=1;qs_values(chunk)[5]=2000;}
        qs_selection(chunk)[0]=0;qs_selection(chunk)[1]=1;
        void *fmem[]={chunk,&p};long long flen[]={bytes,sizeof(p)};run(filter,fmem,flen,2);
        void *mem[]={chunk,&p,&tasks[worker]};long long len[]={bytes,sizeof(p),sizeof(tasks[0])};run(sink,mem,len,3);
        group.states[worker]=tasks[worker].state; free(chunk);
    }
    struct qs_result *result=calloc(1,sizeof(*result)); assert(result);
    void *out[]={&group,result};long long olen[]={sizeof(group),sizeof(*result)};run(source,out,olen,2);
    assert(result->rows==2 && !group.states[0] && result->metrics.scanned==5*(workers/4));
    void *order[]={result,&p};long long orderlen[]={sizeof(*result),sizeof(p)};run(sort,order,orderlen,2);run(limit,order,orderlen,2);
    assert(result->rows==1 && result->begin==1);
    struct qs_cell *cells=result->data[1].cells;
    assert(cells[0].value==1 && cells[1].value==3000*(workers/4) && cells[1].scale==2);
    assert(cells[2].value==(__int128)15*10000000000000000ULL && cells[2].scale==16);
    assert(cells[3].value==3*(workers/4) && cells[4].value==2*(workers/4));
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
    struct qs_cell average; assert(!qs_average((struct qs_cell){.value=1234,.scale=2,.dscale=2},3,&average));
    assert(average.scale==16 && average.value==41133333333333333LL);
    int cmp;assert(!qs_compare((struct qs_cell){.scale=2},(struct qs_cell){0},&cmp) && !cmp);
    assert(!qs_compare((struct qs_cell){.value=-10,.scale=2},(struct qs_cell){.value=-1},&cmp) && cmp>0);
    __int128 largest=(__int128)(((unsigned __int128)1<<127)-1),large=9;
    for(unsigned i=0;i<37;++i) large*=10;
    assert(!qs_compare((struct qs_cell){.value=largest,.scale=1},(struct qs_cell){.value=large},&cmp) && cmp<0);
    const unsigned expected[QS_OPERATORS]={1,4,4,1,1,1};
    for(unsigned i=0;i<QS_OPERATORS;++i) {
        const struct qs_timing *t=&profile.operators[i];
        assert(t->calls==(profiling?expected[i]:0));
        if(profiling && expected[i]) assert(t->wall_ns>=t->max_wall_ns && t->max_wall_ns && t->cpu_ns);
        else assert(!t->wall_ns && !t->cpu_ns && !t->max_wall_ns);
    }
    /* Instrumentation preserves both the error and the caller's MR count. */
    struct femu_csd_args invalid={0}; int64_t error=scan(&invalid); assert(error<0);
    void *mr[]={&profile}; long long len[]={sizeof(profile)};
    invalid=(struct femu_csd_args){.numr=1,.mr_addr=mr,.mr_len=len,.cparam1=1};
    assert(scan(&invalid)==error && invalid.numr==1 && invalid.cparam1==1);
    uint64_t calls=profile.operators[QS_SCAN].calls; --len[0];
    assert(scan(&invalid)<0 && profile.operators[QS_SCAN].calls==calls);
    free(result); puts("PASS vector filter, NULL aggregates, multi-state merge, exact AVG, sort/limit and workmem cleanup"); return 0;
}
