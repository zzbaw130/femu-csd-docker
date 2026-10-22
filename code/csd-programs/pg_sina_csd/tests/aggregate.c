/* Native CSF test: independent sum/count oracle and a simulated NVM token store. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "query_eval.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
typedef int64_t (*function)(struct femu_csd_args *);
static void *blocks[16384];
static unsigned next_token,stored,loaded;
static size_t capacity,budget;
static int64_t run(function f,struct femu_csd_args *a)
{
    a->cparam2=0;
    for(;;) {
        int64_t value=f(a);
        if(value<0) fprintf(stderr,"CSF failed: %lld, stores=%u loads=%u\n",(long long)value,stored,loaded);
        assert(value>=0);
        uint64_t action=(uint64_t)value&~CS_IO_VALUE_MASK,token=(uint64_t)value&CS_IO_VALUE_MASK;
        void *io=a->mr_addr[a->numr-1];
        if(action==CS_NEED_STORE) {
            assert(token==QS_SORT_IO_BYTES && next_token<16384);
            token=next_token++; blocks[token]=malloc(QS_SORT_IO_BYTES); assert(blocks[token]);
            memcpy(blocks[token],io,QS_SORT_IO_BYTES); ++stored;
        } else if(action==CS_NEED_LOAD) {
            assert(token && token<next_token && blocks[token]);
            memcpy(io,blocks[token],QS_SORT_IO_BYTES); ++loaded;
        } else if(action==CS_NEED_FREE) {
            assert(token && token<next_token && blocks[token]); free(blocks[token]); blocks[token]=NULL;
        } else return value;
        a->cparam2=token;
    }
}
struct oracle { int64_t sum; uint64_t values,rows; unsigned dscale; };
static int order_output;
static int compare(const void *left,const void *right)
{
    const struct qs_row *a=left,*b=right; int cmp=0;
    if(order_output) {
        /* SUM DESC NULLS LAST, key ASC NULLS FIRST. */
        if(a->cells[1].isnull!=b->cells[1].isnull) return a->cells[1].isnull?1:-1;
        if(!a->cells[1].isnull) { assert(!qs_compare(a->cells[1],b->cells[1],&cmp)); if(cmp) return -cmp; }
    }
    if(a->cells[0].isnull!=b->cells[0].isnull) return a->cells[0].isnull?-1:1;
    if(a->cells[0].isnull) return 0;
    assert(!qs_compare(a->cells[0],b->cells[0],&cmp)); return cmp;
}
static void check(function sink,function source,unsigned rows,unsigned groups,int ordered,int64_t limit,int64_t offset,int cancel,int profile)
{
    next_token=1; stored=loaded=0;
    struct oracle *oracle=calloc(groups+1,sizeof(*oracle)); assert(oracle);
    struct qs_plan p={.having=-1,.nattrs=2,.ncols=2,.nexprs=2,.naggs=4,.nkeys=1,.nout=5,
        .predicate=-1,.external_aggregate=1,.work_mem_bytes=budget/4,.limit=limit,.offset=offset};
    p.keys[0]=0;
    for(unsigned i=0;i<2;++i) {
        p.cols[i]=(struct qs_column){.slot=i,.type=i?Q_DECIMAL:Q_INTEGER,.scale=i?2:0};
        p.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.column=i,.type=p.cols[i].type,.scale=p.cols[i].scale};
    }
    p.aggs[0]=(struct qs_agg){.kind=Q_SUM,.expr=1};
    p.aggs[1]=(struct qs_agg){.kind=Q_AVG,.expr=1};
    p.aggs[2]=(struct qs_agg){.kind=Q_COUNT,.expr=1};
    p.aggs[3]=(struct qs_agg){.kind=Q_COUNT_STAR,.expr=-1};
    for(unsigned i=0;i<4;++i) p.outputs[i+1]=(struct qs_output){.aggregate=1,.index=i};
    if(ordered) {
        p.norder=2; p.order[0]=(struct qs_order){.output=1,.descending=1};
        p.order[1]=(struct qs_order){.output=0,.nulls_first=1};
    }
    size_t bytes=qs_chunk_bytes(997,2); struct qs_chunk *c=calloc(1,bytes); assert(c);
    c->capacity=997; c->ncols=2;
    void *io=calloc(1,QS_SORT_IO_BYTES); assert(io);
    struct qs_task tasks[4]={{0}}; struct qs_profile counters[4]={0};
    unsigned chunk=0;
    /* Include zero-row input so empty aggregation still has an owned state. */
    for(unsigned first=0;first<rows || !chunk;first+=997) {
        unsigned n=rows-first; if(n>997) n=997;
        memset(qs_nulls(c),0,((c->capacity+7)/8)*2); c->rows=c->selected=n;
        for(unsigned i=0;i<n;++i) {
            unsigned at=first+i,key=(unsigned)((uint64_t)at*2654435761U%groups)+1;
            if(at%257==0) { key=0; qs_nulls(c)[i/8]|=1U<<(i%8); }
            qs_values(c)[i]=(int64_t)key-1;
            int64_t value=(int64_t)(at%201)-100;
            qs_values(c)[c->capacity+i]=value; qs_dscales(c)[c->capacity+i]=at%3;
            struct oracle *o=&oracle[key]; ++o->rows;
            if(at%13==0 || key==7) qs_nulls(c)[(c->capacity+7)/8+i/8]|=1U<<(i%8);
            else { o->sum+=value; ++o->values; if(at%3>o->dscale) o->dscale=at%3; }
            qs_selection(c)[i]=i;
        }
        unsigned worker=chunk++%4; tasks[worker].workers=4; tasks[worker].input_bytes=n*16;
        void *mr[]={c,&p,&tasks[worker],profile?(void *)&counters[worker]:io,io};
        long long len[]={bytes,sizeof(p),sizeof(tasks[0]),profile?sizeof(counters[0]):QS_SORT_IO_BYTES,QS_SORT_IO_BYTES};
        struct femu_csd_args a={.numr=profile?5:4,.mr_addr=mr,.mr_len=len,.cparam1=profile};
        assert(!run(sink,&a));
    }
    struct qs_row *expected=calloc(groups+1,sizeof(*expected)); assert(expected);
    unsigned expected_count=0;
    for(unsigned k=0;k<=groups;++k) if(oracle[k].rows) {
        struct qs_row *r=&expected[expected_count++]; struct oracle *o=&oracle[k];
        r->cells[0]=(struct qs_cell){.value=(int64_t)k-1,.isnull=!k};
        r->cells[1]=(struct qs_cell){.value=o->sum,.scale=2,.dscale=o->dscale,.isnull=!o->values};
        r->cells[2]=r->cells[1]; if(o->values) assert(!qs_average(r->cells[1],o->values,&r->cells[2]));
        r->cells[3].value=o->values; r->cells[4].value=o->rows;
    }
    order_output=ordered; qsort(expected,expected_count,sizeof(*expected),compare);
    struct qs_group group={0};
    for(unsigned i=0;i<4;++i) if(tasks[i].state) group.states[group.count++]=tasks[i].state;
    struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    void *mr[]={&group,out,profile?(void *)&counters[0]:io,io};
    long long len[]={sizeof(group),sizeof(*out),profile?sizeof(counters[0]):QS_SORT_IO_BYTES,QS_SORT_IO_BYTES};
    struct femu_csd_args a={.numr=profile?4:3,.mr_addr=mr,.mr_len=len,.cparam1=profile};
    unsigned seen=0;
    do {
        assert(!run(source,&a));
        for(unsigned i=0;i<out->rows;++i) {
            assert(seen+(uint64_t)offset<expected_count);
            struct qs_row *want=&expected[seen++ + offset];
            for(unsigned j=0;j<5;++j) {
                struct qs_cell x=out->data[out->begin+i].cells[j],y=want->cells[j];
                assert(x.isnull==y.isnull);
                if(!x.isnull) {
                    if(x.value!=y.value || x.scale!=y.scale || x.dscale!=y.dscale)
                        fprintf(stderr,"key=%lld column=%u got=%lld/%u/%u expected=%lld/%u/%u\n",
                            (long long)out->data[out->begin+i].cells[0].value,j,
                            (long long)x.value,x.scale,x.dscale,(long long)y.value,y.scale,y.dscale);
                    assert(x.value==y.value && x.scale==y.scale && x.dscale==y.dscale);
                }
            }
        }
        if(cancel && !out->finished) {
            if(cancel==2) for(unsigned i=1;i<next_token;++i) { free(blocks[i]); blocks[i]=NULL; }
            void *drop[]={&group}; long long size[]={sizeof(group)};
            struct femu_csd_args d={.numr=1,.mr_addr=drop,.mr_len=size}; assert(!source(&d)); break;
        }
    } while(!out->finished);
    if(!cancel) {
        uint64_t count=expected_count>(uint64_t)offset?expected_count-offset:0;
        if(limit>=0 && count>(uint64_t)limit) count=limit;
        assert(seen==count);
    }
    /* One-MR abort delegates surviving NVM tokens to device-close cleanup. */
    for(unsigned i=1;i<next_token;++i) { if(!cancel) assert(!blocks[i]); free(blocks[i]); blocks[i]=NULL; }
    for(unsigned i=0;i<group.count;++i) assert(!group.states[i]);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats));
    assert(!stats.used_bytes && !stats.active_contexts && stats.peak_used_bytes<=budget);
    if(profile) assert(counters[0].operators[QS_AGG_SOURCE].calls);
    printf("rows=%u groups=%u ordered=%d budget=%zu peak=%zu stores=%u loads=%u\n",rows,expected_count,ordered,budget,stats.peak_used_bytes,stored,loaded);
    free(out); free(expected); free(oracle); free(c); free(io);
}
int main(int argc,char **argv)
{
    assert(argc>=2 && argc<=4);
    capacity=argc>=3?(size_t)strtoul(argv[2],NULL,10)*1024*1024:8U*1024*1024;
    budget=argc==4?(size_t)strtoul(argv[3],NULL,10)*1024*1024:capacity;
    if(budget>capacity) budget=capacity;
    assert(!cs_work_init(capacity));
    void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); if(!so) fprintf(stderr,"%s\n",dlerror()); assert(so);
    function sink=(function)dlsym(so,"pg_sina_aggregate_sink"),source=(function)dlsym(so,"pg_sina_aggregate_source");
    assert(sink && source);
    check(sink,source,20003,7,1,3,2,0,1); /* Small hash merge, no runs. */
    check(sink,source,20003,1001,0,-1,0,0,0);
    check(sink,source,100003,32771,1,-1,0,0,1);
    check(sink,source,20003,1001,1,17,5,0,0);
    check(sink,source,20003,1001,0,17,5,0,0);
    check(sink,source,20003,1001,0,0,0,0,0);
    check(sink,source,20003,1001,0,17,INT64_MAX,0,0);
    check(sink,source,20003,1001,0,-1,0,1,0);
    check(sink,source,100003,32771,1,-1,0,2,0);
    check(sink,source,0,1001,0,-1,0,0,0);
    check(sink,source,0,1001,1,-1,0,0,0);
    dlclose(so); puts("PASS bounded external aggregate, weighted AVG, NULL, ordering, streaming and cleanup"); return 0;
}
