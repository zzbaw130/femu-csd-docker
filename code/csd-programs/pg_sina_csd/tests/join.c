#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "query_eval.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
typedef int64_t (*function)(struct femu_csd_args *);
static void *blocks[8192];
static unsigned next_token,stores,loads,estimated_rows;
static size_t capacity,budget;
static int64_t run(function f,struct femu_csd_args *a)
{
    a->cparam2=0;
    for(;;) {
        int64_t value=f(a);
        if(value<0) fprintf(stderr,"join CSF returned %lld\n",(long long)value);
        assert(value>=0);
        uint64_t action=(uint64_t)value&~CS_IO_VALUE_MASK,token=(uint64_t)value&CS_IO_VALUE_MASK;
        void *io=a->mr_addr[a->numr-1];
        if(action==CS_NEED_STORE) {
            assert(token==QS_SORT_IO_BYTES && next_token<8192);
            token=next_token++; blocks[token]=malloc(QS_SORT_IO_BYTES); assert(blocks[token]);
            memcpy(blocks[token],io,QS_SORT_IO_BYTES); ++stores;
        } else if(action==CS_NEED_LOAD) {
            assert(token && token<next_token && blocks[token]); memcpy(io,blocks[token],QS_SORT_IO_BYTES); ++loads;
        } else if(action==CS_NEED_FREE) {
            assert(token && token<next_token && blocks[token]); free(blocks[token]); blocks[token]=NULL;
        } else return value;
        a->cparam2=token;
    }
}
static void check(function build,function load,function probe,function close,function source,
    unsigned build_rows,unsigned probe_rows,int skew,int cancel,int profile,int grouped,int residual)
{
    next_token=1; stores=loads=0;
    struct qs_plan p={.nattrs=4,.ncols=4,.nexprs=9,.naggs=4,.nout=5,.predicate=-1,.limit=-1,
        .work_mem_bytes=budget/8,.join={.relid=1,.nattrs=2,.build_key=2,.probe_key=0,.rows=estimated_rows?estimated_rows:build_rows,.work_mem_bytes=budget/2}};
    for(unsigned i=0;i<4;++i) p.cols[i]=(struct qs_column){.slot=i,.type=i==1?Q_DECIMAL:i==3?Q_BOOLEAN:Q_INTEGER,.scale=i==1?2:0};
    p.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=1,.type=Q_DECIMAL,.scale=2};
    p.exprs[1]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL};
    p.exprs[2]=(struct qs_expr){.op=Q_COLUMN,.column=3,.type=Q_BOOLEAN};
    p.exprs[3]=(struct qs_expr){.op=Q_CASE,.left=2,.right=0,.otherwise=1,.type=Q_DECIMAL,.scale=2};
    p.exprs[4]=(struct qs_expr){.op=Q_AGGREF,.column=0,.type=Q_DECIMAL};
    p.exprs[5]=(struct qs_expr){.op=Q_CONST,.constant=100,.type=Q_DECIMAL};
    p.exprs[6]=(struct qs_expr){.op=Q_MUL,.left=5,.right=4,.type=Q_DECIMAL};
    p.exprs[7]=(struct qs_expr){.op=Q_AGGREF,.column=1,.type=Q_DECIMAL};
    p.exprs[8]=(struct qs_expr){.op=Q_DIV,.left=6,.right=7,.type=Q_DECIMAL};
    p.aggs[0]=(struct qs_agg){.kind=Q_SUM,.expr=3}; p.aggs[1]=(struct qs_agg){.kind=Q_SUM,.expr=0};
    p.aggs[2]=(struct qs_agg){.kind=Q_COUNT_STAR,.expr=-1}; p.aggs[3]=(struct qs_agg){.kind=Q_AVG,.expr=0};
    for(unsigned i=0;i<4;++i) p.outputs[i]=(struct qs_output){.aggregate=1,.index=i};
    p.outputs[4]=(struct qs_output){.aggregate=2,.index=8};
    unsigned workers=grouped?2:4;
    if(grouped) {
        p.external_aggregate=1; p.nkeys=p.norder=1; p.nexprs=10; p.work_mem_bytes=budget/4;
        p.exprs[9]=(struct qs_expr){.op=Q_COLUMN,.column=0,.type=Q_INTEGER}; p.keys[0]=9;
        p.outputs[0]=(struct qs_output){.index=0};
        for(unsigned i=1;i<5;++i) p.outputs[i]=(struct qs_output){.aggregate=1,.index=i-1};
        p.order[0]=(struct qs_order){.output=0};
    }
    if(residual) {
        p.nexprs=16; p.join.residual=16;
        p.exprs[9]=(struct qs_expr){.op=Q_COLUMN,.column=0,.type=Q_INTEGER};
        p.exprs[10]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL,.scale=2,.constant=100};
        p.exprs[11]=(struct qs_expr){.op=Q_GE,.type=Q_BOOLEAN,.left=0,.right=10};
        p.exprs[12]=(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=11,.right=2};
        p.exprs[13]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=-3400};
        p.exprs[14]=(struct qs_expr){.op=Q_LT,.type=Q_BOOLEAN,.left=9,.right=13};
        p.exprs[15]=(struct qs_expr){.op=Q_OR,.type=Q_BOOLEAN,.left=12,.right=14};
        p.cols[1].deferred=1; /* Joined values have already been materialized. */
    }
    unsigned all[7001]={0},promo[7001]={0};
    struct qs_join_control control={0}; struct qs_task tasks[4]={{0}}; struct qs_profile counters[4]={0};
    size_t bytes=qs_chunk_bytes(997,2); struct qs_chunk *c=calloc(1,bytes); assert(c);
    c->capacity=997; c->ncols=2;
    void *io=calloc(1,QS_SORT_IO_BYTES); assert(io);
    for(unsigned first=0;first<build_rows || !control.state;first+=997) {
        unsigned n=build_rows-first; if(n>997) n=997;
        c->rows=c->selected=n; memset(qs_nulls(c),0,2*((c->capacity+7)/8));
        for(unsigned i=0;i<n;++i) {
            unsigned at=first+i,key=skew?3493:at%7001;
            qs_values(c)[i]=(int64_t)key-3500; qs_values(c)[c->capacity+i]=at%3==0;
            if(at%97==0) qs_nulls(c)[i/8]|=1U<<(i%8);
            else { ++all[key]; if(at%3==0 && at%13!=0) ++promo[key]; }
            if(at%13==0) qs_nulls(c)[(c->capacity+7)/8+i/8]|=1U<<(i%8);
            qs_selection(c)[i]=i;
        }
        void *mr[]={c,&p,&control,profile?(void *)&counters[0]:io,io};
        long long len[]={bytes,sizeof(p),sizeof(control),profile?sizeof(counters[0]):QS_SORT_IO_BYTES,QS_SORT_IO_BYTES};
        struct femu_csd_args a={.numr=profile?5:4,.mr_addr=mr,.mr_len=len,.cparam1=profile}; assert(!run(build,&a));
    }
    __int128 expected_promo=0,expected_total=0; uint64_t expected_rows=0,expected_values=0;
    uint64_t pairs_by[7001]={0},values_by[7001]={0},total_by[7001]={0},promo_by[7001]={0},case_by[7001]={0};
    for(unsigned at=0;at<probe_rows;++at) if(at%113!=0) {
        unsigned key=skew?3493:at%7001,pairs=all[key],promos=promo[key];
        if(residual && key>=100) {
            pairs=at%17 && at%201+1>=100?promo[key]:0; promos=pairs;
        }
        expected_rows+=pairs; pairs_by[key]+=pairs;
        case_by[key]+=at%17?pairs:pairs-promos;
        if(at%17!=0) { values_by[key]+=pairs; total_by[key]+=(at%201+1)*pairs; promo_by[key]+=(at%201+1)*promos; }
        if(at%17!=0) { expected_values+=pairs; expected_total+=(__int128)(at%201+1)*pairs; expected_promo+=(__int128)(at%201+1)*promos; }
    }
    struct qs_chunk *joined=calloc(1,qs_chunk_bytes(256,4)); assert(joined); joined->capacity=256; joined->ncols=4;
    unsigned passes=1;
    for(unsigned pass=0;!cancel && pass<passes;++pass) {
        control.current=pass;
        void *mr[]={&control,profile?(void *)&counters[0]:io,io};
        long long len[]={sizeof(control),profile?sizeof(counters[0]):QS_SORT_IO_BYTES,QS_SORT_IO_BYTES};
        struct femu_csd_args a={.numr=profile?3:2,.mr_addr=mr,.mr_len=len,.cparam1=profile}; assert(!run(load,&a)); passes=control.batches;
        unsigned chunk=0;
        for(unsigned first=0;first<probe_rows || !chunk;first+=997) {
            unsigned n=probe_rows-first; if(n>997) n=997;
            c->rows=c->selected=n; memset(qs_nulls(c),0,2*((c->capacity+7)/8));
            for(unsigned i=0;i<n;++i) {
                unsigned at=first+i; qs_values(c)[i]=(int64_t)(skew?3493:at%7001)-3500;
                qs_values(c)[c->capacity+i]=at%201+1; qs_dscales(c)[c->capacity+i]=2;
                if(at%113==0) qs_nulls(c)[i/8]|=1U<<(i%8);
                if(at%17==0) qs_nulls(c)[(c->capacity+7)/8+i/8]|=1U<<(i%8);
                qs_selection(c)[i]=i;
            }
            unsigned worker=chunk++%workers; tasks[worker].workers=workers; tasks[worker].slot=worker;
            void *memory[]={c,&p,&tasks[worker],&control,joined,grouped?(profile?(void *)&counters[worker]:io):&counters[worker],io};
            long long sizes[]={bytes,sizeof(p),sizeof(tasks[0]),sizeof(control),qs_chunk_bytes(256,4),grouped?(profile?sizeof(counters[0]):QS_SORT_IO_BYTES):sizeof(counters[0]),QS_SORT_IO_BYTES};
            struct femu_csd_args request={.numr=grouped?(profile?7:6):(profile?6:5),.mr_addr=memory,.mr_len=sizes,.cparam1=profile}; assert(!run(probe,&request));
        }
    }
    struct qs_group group={0}; uint64_t actual_pairs=0;
    for(unsigned i=0;i<4;++i) if(tasks[i].state) { group.states[group.count++]=tasks[i].state; actual_pairs+=tasks[i].joined; }
    if(grouped) {
        void *mr[]={&control}; long long len[]={sizeof(control)};
        struct femu_csd_args a={.numr=1,.mr_addr=mr,.mr_len=len}; assert(!close(&a) && !control.state);
    }
    if(!cancel) {
        assert(actual_pairs==expected_rows);
        if(estimated_rows && budget>=64*1024*1024 && !grouped) assert(passes==1 && !stores);
        struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
        void *mr[]={&group,out,io}; long long len[]={sizeof(group),sizeof(*out),QS_SORT_IO_BYTES};
        struct femu_csd_args a={.numr=grouped?3:2,.mr_addr=mr,.mr_len=len};
        if(grouped) {
            unsigned next=0;
            do {
                assert(!run(source,&a));
                for(unsigned row=0;row<out->rows;++row) {
                    while(next<7001 && !pairs_by[next]) ++next;
                    assert(next<7001); const struct qs_cell *v=out->data[row].cells;
                    assert(v[0].value==(int)next-3500 && v[3].value==pairs_by[next]);
                    assert(v[1].isnull==!case_by[next] && v[2].isnull==!values_by[next] && v[4].isnull==!values_by[next]);
                    if(case_by[next]) assert(v[1].value==promo_by[next]);
                    if(values_by[next]) {
                        assert(v[2].value==total_by[next]);
                        struct qs_cell avg; assert(!qs_average((struct qs_cell){.value=total_by[next],.scale=2,.dscale=2},values_by[next],&avg));
                        assert(v[4].value==avg.value && v[4].scale==avg.scale);
                    }
                    ++next;
                }
            } while(!out->finished);
            while(next<7001 && !pairs_by[next]) ++next;
            assert(next==7001);
        } else {
        assert(!source(&a));
        assert(out->rows==1 && out->finished && out->data[0].cells[2].value==expected_rows);
        assert(out->data[0].cells[1].isnull==!expected_values);
        if(expected_values) {
            assert(out->data[0].cells[0].value==expected_promo && out->data[0].cells[1].value==expected_total);
            struct qs_cell ratio;
            assert(!qs_divide((struct qs_cell){.value=expected_promo*100,.scale=2,.dscale=2},
                (struct qs_cell){.value=expected_total,.scale=2,.dscale=2},&ratio));
            assert(out->data[0].cells[4].value==ratio.value && out->data[0].cells[4].scale==ratio.scale);
        } else assert(out->data[0].cells[4].isnull);
        }
        assert(control.rows==build_rows && (grouped || !stores || stores==loads)); free(out);
    }
    /* Model device-close token reclamation, including a failed batch scope. */
    for(unsigned i=1;i<next_token;++i) { free(blocks[i]); blocks[i]=NULL; }
    void *mr[]={&control}; long long len[]={sizeof(control)};
    struct femu_csd_args a={.numr=1,.mr_addr=mr,.mr_len=len}; assert(!close(&a) && !control.state);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats));
    assert(!stats.used_bytes && !stats.active_contexts && stats.peak_used_bytes<=budget);
    printf("build=%u probe=%u pairs=%llu passes=%u peak=%zu stores=%u loads=%u\n",build_rows,probe_rows,(unsigned long long)expected_rows,passes,stats.peak_used_bytes,stores,loads);
    free(c); free(io); free(joined);
}
int main(int argc,char **argv)
{
    assert(argc==4 || argc==5); capacity=(size_t)strtoul(argv[3],NULL,10)*1024*1024;
    budget=argc==5?(size_t)strtoul(argv[4],NULL,10)*1024*1024:capacity;
    if(budget>capacity) budget=capacity;
    assert(!cs_work_init(capacity));
    void *join=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL),*agg=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);
    if(!join || !agg) fprintf(stderr,"%s\n",dlerror());
    assert(join && agg);
    function build=(function)dlsym(join,"pg_sina_join_build"),load=(function)dlsym(join,"pg_sina_join_load");
    function probe=(function)dlsym(join,"pg_sina_join_probe"),close=(function)dlsym(join,"pg_sina_join_close");
    function source=(function)dlsym(agg,"pg_sina_aggregate_source"); assert(build && load && probe && close && source);
    check(build,load,probe,close,source,70003,17003,0,0,1,0,0);
    check(build,load,probe,close,source,5001,301,1,0,0,0,0);
    check(build,load,probe,close,source,0,2003,0,0,0,0,0);
    check(build,load,probe,close,source,70003,0,0,0,0,0,0);
    check(build,load,probe,close,source,70003,17003,0,1,0,0,0);
    check(build,load,probe,close,source,70003,150003,0,0,1,1,0);
    check(build,load,probe,close,source,5001,301,1,0,0,1,0);
    check(build,load,probe,close,source,0,2003,0,0,0,1,0);
    check(build,load,probe,close,source,70003,0,0,0,0,1,0);
    check(build,load,probe,close,source,70003,17003,0,0,1,0,1);
    check(build,load,probe,close,source,70003,5003,0,0,1,1,1);
    check(build,load,probe,close,source,0,2003,0,0,0,1,1);
    estimated_rows=256;
    check(build,load,probe,close,source,70003,17003,0,0,1,0,1);
    dlclose(agg); dlclose(join); puts("PASS hash join duplicates, negative/NULL keys, spill/replay, CASE, final projection and cleanup"); return 0;
}
