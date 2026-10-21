#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "query_vector.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
static void expressions(void)
{
    struct qs_plan p={.nattrs=3,.ncols=3,.nexprs=27};
    p.cols[0]=(struct qs_column){.slot=0,.type=Q_INTEGER};
    p.cols[1]=(struct qs_column){.slot=1,.type=Q_DECIMAL,.scale=2};
    p.cols[2]=(struct qs_column){.slot=2,.type=Q_BOOLEAN};
    p.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=0,.type=Q_INTEGER};
    p.exprs[1]=(struct qs_expr){.op=Q_CONST,.constant=17,.type=Q_INTEGER};
    p.exprs[2]=(struct qs_expr){.op=Q_ADD,.left=0,.right=1,.type=Q_INTEGER};
    p.exprs[3]=(struct qs_expr){.op=Q_COLUMN,.column=1,.type=Q_DECIMAL,.scale=2};
    p.exprs[4]=(struct qs_expr){.op=Q_CONST,.constant=1,.type=Q_DECIMAL,.scale=1,.dscale=1};
    p.exprs[5]=(struct qs_expr){.op=Q_SUB,.left=3,.right=4,.type=Q_DECIMAL,.scale=2};
    p.exprs[6]=(struct qs_expr){.op=Q_MUL,.left=2,.right=5,.type=Q_DECIMAL,.scale=2};
    p.exprs[7]=(struct qs_expr){.op=Q_CONST,.constant=INT64_MAX,.type=Q_INTEGER};
    p.exprs[8]=(struct qs_expr){.op=Q_MUL,.left=6,.right=7,.type=Q_DECIMAL,.scale=2};
    p.exprs[9]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL,.scale=2};
    p.exprs[10]=(struct qs_expr){.op=Q_GT,.left=8,.right=9,.type=Q_BOOLEAN};
    p.exprs[11]=(struct qs_expr){.op=Q_CONST,.type=Q_BOOLEAN};
    p.exprs[12]=(struct qs_expr){.op=Q_AND,.left=11,.right=10,.type=Q_BOOLEAN};
    p.exprs[13]=(struct qs_expr){.op=Q_CONST,.type=Q_BOOLEAN,.constant=1};
    p.exprs[14]=(struct qs_expr){.op=Q_OR,.left=13,.right=10,.type=Q_BOOLEAN};
    p.exprs[15]=(struct qs_expr){.op=Q_OR,.left=10,.right=13,.type=Q_BOOLEAN};
    p.exprs[16]=(struct qs_expr){.op=Q_NOT,.left=10,.type=Q_BOOLEAN};
    p.exprs[17]=(struct qs_expr){.op=Q_IS_NULL,.left=3,.type=Q_BOOLEAN};
    p.exprs[18]=(struct qs_expr){.op=Q_AND,.left=17,.right=10,.type=Q_BOOLEAN};
    p.exprs[19]=(struct qs_expr){.op=Q_OR,.left=17,.right=10,.type=Q_BOOLEAN};
    p.exprs[20]=(struct qs_expr){.op=Q_NE,.left=0,.right=7,.type=Q_BOOLEAN};
    p.exprs[21]=(struct qs_expr){.op=Q_LT,.left=3,.right=4,.type=Q_BOOLEAN};
    p.exprs[22]=(struct qs_expr){.op=Q_IS_NOT_NULL,.left=8,.type=Q_BOOLEAN};
    p.exprs[23]=(struct qs_expr){.op=Q_COLUMN,.column=2,.type=Q_BOOLEAN};
    p.exprs[24]=(struct qs_expr){.op=Q_AND,.left=23,.right=10,.type=Q_BOOLEAN};
    p.exprs[25]=(struct qs_expr){.op=Q_OR,.left=23,.right=10,.type=Q_BOOLEAN};
    p.exprs[26]=(struct qs_expr){.op=Q_NOT,.left=23,.type=Q_BOOLEAN};
    size_t bytes=qs_chunk_bytes(3003,3); struct qs_chunk *c=calloc(1,bytes); assert(c);
    c->capacity=c->rows=3003; c->ncols=3; uint64_t random=19;
    for(unsigned col=0;col<3;++col) for(unsigned row=0;row<c->rows;++row) {
        random=random*6364136223846793005ULL+1;
        qs_values(c)[col*c->capacity+row]=col==2?row%2:row%13==0?INT64_MAX:row%13==1?INT64_MIN:(int64_t)(random%200001)-100000;
        qs_dscales(c)[col*c->capacity+row]=col==1?row%3:0;
        if((row+col)%11==0) qs_nulls(c)[col*((c->capacity+7)/8)+row/8]|=1U<<(row%8);
        qs_selection(c)[row]=c->rows-1-row; /* selected/non-contiguous order */
    }
    struct qs_vector *vectors=calloc(p.nexprs,sizeof(*vectors)); assert(vectors);
    for(unsigned selected=0;selected<2;++selected) for(unsigned root=0;root<p.nexprs;++root) {
        uint8_t needed[QS_EXPRS]={0}; assert(!qs_vector_require(&p,root,needed));
        for(unsigned first=0;first<c->rows;first+=QS_VECTOR_ROWS) {
            unsigned n=c->rows-first; if(n>QS_VECTOR_ROWS) n=QS_VECTOR_ROWS;
            assert(!qs_vector_execute(&p,c,selected?qs_selection(c):NULL,first,n,needed,vectors));
            for(unsigned i=0;i<n;++i) {
                struct qs_eval e={.plan=&p,.chunk=c,.row=selected?qs_selection(c)[first+i]:first+i};
                struct qs_cell expected,actual=qs_vector_cell(&p,vectors,root,i);
                int error=qs_evaluate(&e,root,&expected);
                assert((error!=0)==(vectors[root].errors[i]!=0));
                if(error) continue;
                assert(expected.isnull==actual.isnull);
                if(!expected.isnull) assert(expected.value==actual.value && expected.scale==actual.scale && expected.dscale==actual.dscale);
            }
        }
    }
    free(vectors); free(c);
}
typedef int64_t (*function)(struct femu_csd_args *);
static void aggregates(const char *path)
{
    void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL); assert(library);
    function sink=(function)dlsym(library,"pg_sina_aggregate_sink"),source=(function)dlsym(library,"pg_sina_aggregate_source"); assert(sink && source);
    struct qs_plan p={.nattrs=1,.ncols=1,.nexprs=1,.naggs=5,.nout=5,.predicate=-1,.limit=-1};
    p.cols[0]=(struct qs_column){.slot=0,.type=Q_DECIMAL,.scale=2};
    p.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=0,.type=Q_DECIMAL,.scale=2};
    p.aggs[0]=(struct qs_agg){.kind=Q_COUNT,.expr=0};
    p.aggs[1]=(struct qs_agg){.kind=Q_AVG,.expr=0}; p.aggs[2]=(struct qs_agg){.kind=Q_SUM,.expr=0};
    p.aggs[3]=p.aggs[4]=(struct qs_agg){.kind=Q_COUNT_STAR,.expr=-1};
    for(unsigned i=0;i<5;++i) p.outputs[i]=(struct qs_output){.aggregate=1,.index=i};
    size_t bytes=qs_chunk_bytes(513,1); struct qs_chunk *c=calloc(1,bytes); assert(c);
    c->capacity=c->rows=c->selected=513; c->ncols=1;
    for(unsigned row=0;row<c->rows;++row) {
        qs_values(c)[row]=100; qs_dscales(c)[row]=2; qs_selection(c)[row]=row;
        if(row%5==0) qs_nulls(c)[row/8]|=1U<<(row%8);
    }
    struct qs_group group={.count=4}; struct qs_task tasks[4]={{0}};
    for(unsigned worker=0;worker<4;++worker) {
        void *mr[]={c,&p,&tasks[worker]}; long long len[]={bytes,sizeof(p),sizeof(tasks[0])};
        struct femu_csd_args a={.numr=3,.mr_addr=mr,.mr_len=len};
        for(unsigned round=0;round<2;++round) assert(!sink(&a));
        group.states[worker]=tasks[worker].state;
    }
    struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    void *mr[]={&group,out}; long long len[]={sizeof(group),sizeof(*out)};
    struct femu_csd_args a={.numr=2,.mr_addr=mr,.mr_len=len}; assert(!source(&a));
    assert(out->rows==1 && out->data[0].cells[0].value==410*8 && out->data[0].cells[2].value==410*8*100);
    assert(out->data[0].cells[1].value==(__int128)1000000000000000000LL*100 && out->data[0].cells[1].scale==20);
    assert(out->data[0].cells[3].value==513*8 && out->data[0].cells[4].value==513*8);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
    free(out); free(c); dlclose(library);
}
int main(int argc,char **argv)
{
    assert(argc==2); expressions(); aggregates(argv[1]);
    puts("PASS batch expressions vs scalar, NULL/short-circuit/overflow, selection tails and shared aggregate states"); return 0;
}
