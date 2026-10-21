/* Compare eager/late decoding on real heap layouts, including rejected numeric
 * values which cannot be decoded by the device's finite numeric profile. */
#include "femu-csd-kernel.h"
#include "query.h"
#include "heap_numeric.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int64_t (*function)(struct femu_csd_args *);
static void write16(unsigned char *p,uint16_t v) { memcpy(p,&v,2); }
static void write32(unsigned char *p,uint32_t v) { memcpy(p,&v,4); }
static unsigned amount_offsets[62];
static unsigned predicate_offsets[62];
static void make_pages(unsigned char *raw)
{
    for(unsigned page=0;page<2;++page) {
        unsigned char *p=raw+page*QS_PAGE;
        write16(p+10,4); write16(p+12,24+31*4); write16(p+14,QS_PAGE-31*64);
        write16(p+16,QS_PAGE); write16(p+18,QS_PAGE|4);
        for(unsigned i=0;i<31;++i) {
            unsigned row=page*31+i,offset=QS_PAGE-(i+1)*64;
            unsigned char *t=p+offset;
            write32(p+24+i*4,offset|(1U<<15)|(64U<<17));
            write16(t+18,4); write16(t+20,0xb01); t[22]=24;
            t[23]=row%7?15:11; /* nullable amount */
            write32(t+24,row); t[28]=9; memcpy(t+29,"abc",3);
            unsigned at=32;
            if(row%7) {
                unsigned scale=row%3;
                if(row%2) { /* short varlena, short numeric */
                    t[at++]=15;
                    amount_offsets[row]=page*QS_PAGE+offset+at;
                    write16(t+at,0x8000|(scale<<7)|((row%5==0)?0x2000:0)); at+=2;
                } else { /* ordinary varlena, ordinary numeric */
                    write32(t+at,12U<<2); at+=4;
                    amount_offsets[row]=page*QS_PAGE+offset+at;
                    write16(t+at,scale|((row%5==0)?0x4000:0)); at+=2;
                    write16(t+at,0); at+=2;
                }
                write16(t+at,row); at+=2;
                write16(t+at,scale==2?1200:scale==1?1000:0); at+=2;
            }
            t[at++]=11; predicate_offsets[row]=page*QS_PAGE+offset+at;
            write16(t+at,0x8000); at+=2;
            write16(t+at,row%3);
        }
    }
}
static int64_t scan_rows(function scan,struct qs_plan *p,struct qs_chunk *c,size_t bytes,unsigned char *raw)
{
    struct qs_task task={.input_bytes=2*QS_PAGE};
    void *mr[]={raw,p,c,&task}; long long len[]={2*QS_PAGE,sizeof(*p),bytes,sizeof(task)};
    struct femu_csd_args a={.numr=4,.mr_addr=mr,.mr_len=len}; return scan(&a);
}
static int64_t filter_rows(function filter,struct qs_plan *p,struct qs_chunk *c,size_t bytes,unsigned char *raw)
{
    void *mr[]={c,p,raw}; long long len[]={bytes,sizeof(*p),2*QS_PAGE};
    struct femu_csd_args a={.numr=3,.mr_addr=mr,.mr_len=len}; return filter(&a);
}
static void compare(function scan,function filter,struct qs_plan *p,struct qs_chunk *eager,struct qs_chunk *late,size_t bytes,unsigned char *raw)
{
    p->cols[2].deferred=0;
    assert(!scan_rows(scan,p,eager,bytes,raw)); assert(!filter_rows(filter,p,eager,bytes,raw));
    p->cols[2].deferred=1;
    assert(!scan_rows(scan,p,late,bytes,raw)); assert(!filter_rows(filter,p,late,bytes,raw));
    assert(eager->rows==62 && late->rows==62 && eager->selected==late->selected);
    for(unsigned i=0;i<eager->selected;++i) {
        unsigned row=qs_selection(eager)[i]; assert(row==qs_selection(late)[i]);
        for(unsigned col=0;col<3;++col) {
            size_t at=(size_t)col*eager->capacity+row;
            unsigned null=col*((eager->capacity+7)/8)+row/8;
            assert(qs_values(eager)[at]==qs_values(late)[at]);
            assert(qs_dscales(eager)[at]==qs_dscales(late)[at]);
            assert(qs_nulls(eager)[null]==qs_nulls(late)[null]);
        }
    }
}
static void numeric_formats(void)
{
    const uint16_t payloads[][5]={
        {0x8100}, {0x8100,12,3400}, {0xa100,12,3400}, {0x817f,1200},
        {0x8101,12,34,5600}, {0xa101,12,34,5600},
        {2,0,12,3400}, {0x8102,1,2,3,4000}
    };
    const unsigned sizes[]={2,6,6,4,8,8,8,10};
    const int64_t expected[]={0,1234,-1234,12,12003456,-12003456,1234,10002000340LL};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i) {
        int64_t value; unsigned scale;
        assert(!qs_numeric_cents((const unsigned char *)payloads[i],sizes[i],&value,&scale));
        assert(value==expected[i] && scale==2);
    }
    const uint16_t invalid[][4]={{0x8101,12,34,1},{0x8101,12,10000,0},{0x8181,12,34,0},{0x8104,9999}};
    for(unsigned i=0;i<4;++i) {
        int64_t value; unsigned scale;
        assert(qs_numeric_cents((const unsigned char *)invalid[i],i==3?4:8,&value,&scale)<0);
    }
}
int main(int argc,char **argv)
{
    numeric_formats();
    assert(argc==3);
    void *scan_so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL),*filter_so=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);
    assert(scan_so && filter_so);
    function scan=(function)dlsym(scan_so,"pg_sina_scan"),filter=(function)dlsym(filter_so,"pg_sina_filter"); assert(scan && filter);
    unsigned char raw[2*QS_PAGE]={0}; make_pages(raw);
    struct qs_plan p={.nattrs=4,.ncols=3,.nexprs=7,.predicate=6};
    p.cols[0]=(struct qs_column){.length=4,.align=4,.slot=0,.type=Q_INTEGER};
    p.cols[1]=(struct qs_column){.length=-1,.align=4,.slot=-1};
    p.cols[2]=(struct qs_column){.length=-1,.align=4,.slot=2,.type=Q_DECIMAL,.scale=2};
    p.cols[3]=(struct qs_column){.length=-1,.align=4,.slot=1,.type=Q_DECIMAL,.scale=2};
    p.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=0,.type=Q_INTEGER};
    p.exprs[1]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=32};
    p.exprs[2]=(struct qs_expr){.op=Q_GE,.type=Q_BOOLEAN,.left=0,.right=1};
    p.exprs[3]=(struct qs_expr){.op=Q_COLUMN,.column=3,.type=Q_DECIMAL,.scale=2};
    p.exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL,.scale=2,.constant=100};
    p.exprs[5]=(struct qs_expr){.op=Q_LE,.type=Q_BOOLEAN,.left=3,.right=4};
    p.exprs[6]=(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=2,.right=5};
    size_t bytes=qs_chunk_bytes(128,3);
    struct qs_chunk *eager=calloc(1,bytes),*late=calloc(1,bytes); assert(eager && late);
    eager->capacity=late->capacity=128; eager->ncols=late->ncols=3;
    compare(scan,filter,&p,eager,late,bytes,raw); assert(late->selected==20);
    p.exprs[6].op=Q_OR; compare(scan,filter,&p,eager,late,bytes,raw); /* exact fallback */
    p.exprs[6].op=Q_AND;
    p.exprs[1].constant=100; compare(scan,filter,&p,eager,late,bytes,raw); assert(!late->selected);
    p.exprs[1].constant=0; compare(scan,filter,&p,eager,late,bytes,raw); /* full buffer reuse */
    p.exprs[1].constant=32;
    /* Row 1 is rejected: Scan records its location without interpreting NaN. */
    write16(raw+amount_offsets[1],0xc000);
    p.cols[2].deferred=1;
    assert(!scan_rows(scan,&p,late,bytes,raw) && !filter_rows(filter,&p,late,bytes,raw));
    p.cols[2].deferred=0; assert(scan_rows(scan,&p,eager,bytes,raw)==-3);
    /* Selecting the same unsupported value must still report an error. */
    p.cols[2].deferred=1; p.exprs[1].constant=0;
    assert(!scan_rows(scan,&p,late,bytes,raw) && filter_rows(filter,&p,late,bytes,raw)==-3);
    make_pages(raw); p.exprs[1].constant=32;
    assert(!scan_rows(scan,&p,late,bytes,raw));
    struct qs_heap_ref invalid={2*QS_PAGE-1,2};
    memcpy(&qs_values(late)[2*late->capacity+33],&invalid,sizeof(invalid));
    assert(filter_rows(filter,&p,late,bytes,raw)==-2);
    /* Predicate numerics can be lazy too. Rejected rows must not be decoded,
     * and OR must still evaluate its RHS when the left side is false. */
    make_pages(raw); p.cols[2].deferred=0; p.cols[3].deferred=0;
    assert(!scan_rows(scan,&p,eager,bytes,raw) && !filter_rows(filter,&p,eager,bytes,raw));
    p.cols[2].deferred=p.cols[3].deferred=1;
    assert(!scan_rows(scan,&p,late,bytes,raw) && !filter_rows(filter,&p,late,bytes,raw));
    assert(eager->selected==late->selected);
    for(unsigned i=0;i<eager->selected;++i) {
        unsigned row=qs_selection(eager)[i]; assert(row==qs_selection(late)[i]);
        for(unsigned col=0;col<3;++col) {
            size_t at=(size_t)col*eager->capacity+row;
            assert(qs_values(eager)[at]==qs_values(late)[at]);
            assert(qs_dscales(eager)[at]==qs_dscales(late)[at]);
        }
    }
    write16(raw+predicate_offsets[1],0xc000);
    assert(!scan_rows(scan,&p,late,bytes,raw) && !filter_rows(filter,&p,late,bytes,raw));
    p.exprs[6].op=Q_OR;
    assert(!scan_rows(scan,&p,late,bytes,raw) && filter_rows(filter,&p,late,bytes,raw)==-2);
    free(eager); free(late); dlclose(scan_so); dlclose(filter_so);
    puts("PASS eager/late heap decoding, NULL/scales, numeric formats, SIMD/fallback selection, reuse and rejected values");
    return 0;
}
