#include "femu-csd-kernel.h"
#include "query_eval.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
typedef int64_t (*filter_function)(struct femu_csd_args *);
static void compare(filter_function filter,struct qs_plan *p,struct qs_chunk *c,size_t bytes)
{
    uint32_t expected[3003]; unsigned n=0;
    for(unsigned row=0;row<c->rows;++row) {
        struct qs_eval e; e.plan=p; e.chunk=c; e.row=row; memset(e.valid,0,p->nexprs);
        struct qs_cell value; assert(!qs_evaluate(&e,p->predicate,&value));
        if(!value.isnull && value.value) expected[n++]=row;
    }
    void *mr[]={c,p}; long long len[]={bytes,sizeof(*p)};
    struct femu_csd_args args={.numr=2,.mr_addr=mr,.mr_len=len};
    assert(!filter(&args) && c->selected==n);
    for(unsigned i=0;i<n;++i) assert(qs_selection(c)[i]==expected[i]);
}
int main(int argc,char **argv)
{
    assert(argc==2); void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); assert(so);
    filter_function filter=(filter_function)dlsym(so,"pg_sina_filter"); assert(filter);
    size_t bytes=qs_chunk_bytes(3003,3); struct qs_chunk *c=calloc(1,bytes); assert(c);
    c->capacity=c->rows=3003; c->ncols=3; uint64_t random=17;
    struct qs_plan p={.nattrs=3,.ncols=3,.nexprs=3,.predicate=2};
    for(unsigned col=0;col<3;++col) {
        p.cols[col]=(struct qs_column){.slot=col,.type=col==1?Q_DECIMAL:Q_INTEGER,.scale=col==1?2:0};
        for(unsigned row=0;row<c->rows;++row) {
            random=random*6364136223846793005ULL+1;
            int64_t v=row%17==0?INT64_MIN:row%17==1?INT64_MAX:(int64_t)(random%200001)-100000;
            qs_values(c)[(size_t)col*c->capacity+row]=v;
            qs_dscales(c)[(size_t)col*c->capacity+row]=p.cols[col].scale;
            if((row+col)%11==0) qs_nulls(c)[col*((c->capacity+7)/8)+row/8]|=1U<<(row%8);
        }
    }
    const int64_t constants[]={-100,0,100,INT64_MIN,INT64_MAX};
    for(unsigned col=0;col<3;++col) for(unsigned op=Q_EQ;op<=Q_GE;++op)
    for(unsigned reversed=0;reversed<2;++reversed) for(unsigned v=0;v<5;++v) {
        p.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=col,.type=p.cols[col].type,.scale=p.cols[col].scale};
        p.exprs[1]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL,.scale=v%4,.constant=constants[v]};
        p.exprs[2]=(struct qs_expr){.op=op,.type=Q_BOOLEAN,.left=reversed?1:0,.right=reversed?0:1};
        compare(filter,&p,c,bytes);
        p.nexprs=4; p.predicate=3;
        p.exprs[3]=(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=2,.right=2};
        compare(filter,&p,c,bytes); p.nexprs=3; p.predicate=2;
    }
    p.nexprs=7; p.predicate=6;
    p.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=0,.type=Q_INTEGER};
    p.exprs[1]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=42};
    p.exprs[2]=(struct qs_expr){.op=Q_LT,.type=Q_BOOLEAN,.left=0,.right=1};
    p.exprs[3]=(struct qs_expr){.op=Q_COLUMN,.column=1,.type=Q_DECIMAL,.scale=2};
    p.exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL,.constant=17,.scale=2};
    p.exprs[5]=(struct qs_expr){.op=Q_GE,.type=Q_BOOLEAN,.left=3,.right=4};
    p.exprs[6]=(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=2,.right=5};
    const unsigned rows[]={0,1,3,4,5,31,3003};
    for(unsigned i=0;i<sizeof(rows)/sizeof(*rows);++i) { c->rows=rows[i]; compare(filter,&p,c,bytes); }
    c->rows=3003;
    p.exprs[6].op=Q_OR; compare(filter,&p,c,bytes);
    p.exprs[6].op=Q_NOT; compare(filter,&p,c,bytes);
    p.exprs[6].op=Q_IS_NULL; compare(filter,&p,c,bytes);
    p.exprs[6].op=Q_IS_NOT_NULL; compare(filter,&p,c,bytes);
    p.exprs[6].op=Q_AND; p.exprs[4].isnull=1; compare(filter,&p,c,bytes);
    p.exprs[4].isnull=0; p.exprs[4].constant=51; p.exprs[4].scale=3; compare(filter,&p,c,bytes);
    free(c); dlclose(so); puts("PASS column comparisons, signed limits, NULL, conjunctions, tails and exact evaluator fallback"); return 0;
}
