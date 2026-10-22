/* Independent row-loop oracle for native-tree joins and multi-stage groups. */
#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "cs_csf.h"
#include "plan.h"
#include "query_eval.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
typedef int64_t (*function)(struct femu_csd_args *);
static void w16(void *p,uint16_t x) { memcpy(p,&x,2); }
static void w32(void *p,uint32_t x) { memcpy(p,&x,4); }
static unsigned char *heap(unsigned rows,unsigned side,size_t *size)
{
    unsigned pages=(rows+99)/100; *size=(size_t)pages*QS_PAGE;
    unsigned char *raw=calloc(1,*size); assert(raw);
    for(unsigned p=0;p<pages;++p) {
        unsigned count=rows-p*100; if(count>100) count=100;
        unsigned char *page=raw+(size_t)p*QS_PAGE;
        w16(page+10,4); w16(page+12,24+count*4); w16(page+14,QS_PAGE-count*40); w16(page+16,QS_PAGE); w16(page+18,QS_PAGE|4);
        for(unsigned i=0;i<count;++i) {
            unsigned row=p*100+i,off=QS_PAGE-(i+1)*40; unsigned char *t=page+off;
            w32(page+24+i*4,off|(1U<<15)|(40U<<17)); w16(t+18,2); w16(t+20,0xb01); t[22]=24; t[23]=side==2 || row%17?3:2;
            if(side==2) { w32(t+24,row); w32(t+28,(uint64_t)row*1009%100003); }
            else if(row%17) { w32(t+24,row%(side?7:11)); w32(t+28,side?row%13+1:row%19+1); }
            else w32(t+24,side?row%13+1:row%19+1);
        }
    }
    return raw;
}
static unsigned char *index_page(unsigned rows)
{
    assert(rows<100);
    unsigned count=0;
    for(unsigned i=0;i<rows;++i) count+=i%17!=0;
    unsigned char *raw=calloc(2,QS_PAGE),*p=raw+QS_PAGE; assert(raw);
    w16(p+12,24+count*4); w16(p+14,QS_PAGE-16-count*16); w16(p+16,QS_PAGE-16); w16(p+18,QS_PAGE|4);
    w16(p+QS_PAGE-4,3); /* leaf | root, no siblings */
    unsigned item=0;
    for(unsigned key=0;key<7;++key) for(unsigned i=0;i<rows;++i) if(i%17 && i%7==key) {
        unsigned off=QS_PAGE-16-(++item)*16; unsigned char *t=p+off;
        w32(p+24+(item-1)*4,off|(1U<<15)|(16U<<17)); w16(t+4,i+1); w16(t+6,16); w32(t+8,key);
    }
    assert(item==count); return raw;
}
static void scan(struct qp_node *n)
{
    n->kind=QP_SCAN; n->left=n->right=-1; n->expr.predicate=-1; n->expr.nattrs=n->expr.ncols=2;
    n->expr.nexprs=n->expr.nout=2;
    for(unsigned i=0;i<2;++i) {
        n->expr.cols[i]=(struct qs_column){.length=4,.align=4,.slot=i,.type=Q_INTEGER};
        n->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=i};
        n->expr.outputs[i]=(struct qs_output){.index=i,.type_oid=23};
    }
}
static void test(function run,unsigned join,unsigned rows,unsigned right_rows,unsigned memory,int hash)
{
    struct qp_plan *plan=calloc(1,sizeof(*plan)); assert(plan); plan->count=4;
    scan(&plan->nodes[2]); scan(&plan->nodes[3]);
    struct qp_node *j=&plan->nodes[1]; j->kind=QP_JOIN; j->left=2; j->right=3; j->join_type=join; j->join_qual=-1;
    j->hash=hash==1;
    if(hash==2) {
        j->kind=QP_INDEX_JOIN;
        plan->nodes[3].expr.join=(struct qs_join){.indexid=1,.indexroot=1,.index_key_bytes=4,.build_key=1};
    }
    j->nkeys=1; j->left_keys[0]=j->right_keys[0]=0; j->expr.predicate=-1; j->expr.nattrs=4; j->expr.nexprs=4; j->expr.nout=3;
    for(unsigned i=0;i<4;++i) j->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=i};
    for(unsigned i=0;i<3;++i) j->expr.outputs[i]=(struct qs_output){.index=i==2?3:(int)i,.type_oid=23};
    if(join==QP_SEMI || join==QP_ANTI) j->expr.outputs[2].index=1;
    struct qp_node *a=&plan->nodes[0]; a->kind=QP_AGGREGATE; a->left=1; a->right=-1; a->nkeys=1; a->left_keys[0]=0;
    a->expr.predicate=-1; a->expr.work_mem_bytes=(uint64_t)memory*1024*1024; a->expr.nattrs=3; a->expr.nexprs=9;
    for(unsigned i=0;i<3;++i) a->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=i};
    a->expr.nkeys=1; a->expr.keys[0]=0; a->expr.naggs=5;
    a->expr.aggs[0]=(struct qs_agg){.expr=2,.kind=Q_SUM};
    a->expr.aggs[1]=(struct qs_agg){.expr=2,.kind=Q_COUNT};
    a->expr.aggs[2]=(struct qs_agg){.expr=2,.kind=Q_MIN};
    a->expr.aggs[3]=(struct qs_agg){.expr=2,.kind=Q_MAX};
    a->expr.aggs[4]=(struct qs_agg){.expr=2,.kind=Q_AVG};
    a->expr.nout=6; a->expr.outputs[0]=(struct qs_output){.index=0,.type_oid=23};
    for(unsigned i=0;i<5;++i) {
        a->expr.exprs[3+i]=(struct qs_expr){.op=Q_AGGREF,.column=i,.type=i==4?Q_DECIMAL:Q_INTEGER};
        a->expr.outputs[i+1]=(struct qs_output){.index=3+i,.type_oid=i==4?1700:20};
    }
    size_t sizes[2]; unsigned char *raw[2]={heap(rows,0,&sizes[0]),heap(right_rows,1,&sizes[1])};
    size_t bytes=(qs_dict_bytes(512,65536)+4095)&~(size_t)4095;
    for(unsigned i=2;i<4;++i) {
        plan->nodes[i].bytes=sizes[i-2]; plan->nodes[i].input_offset=bytes; bytes+=QP_SCAN_BYTES;
        plan->nodes[i].vector_offset=bytes; bytes+=(qs_chunk_bytes(QP_SCAN_BYTES/24,2)+4095)&~(size_t)4095;
    }
    if(hash==2) {
        j->input_offset=bytes; bytes+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE;
        j->vector_offset=bytes; bytes+=4096+((qs_chunk_bytes(256,4)+4095)&~(size_t)4095);
    }
    unsigned char *index=hash==2?index_page(right_rows):NULL;
    unsigned char *arena=calloc(1,bytes),*io=calloc(1,QS_SORT_IO_BYTES); assert(arena && io);
    struct qs_dict *dict=(void *)arena; *dict=(struct qs_dict){.entries=512,.capacity=512,.data=65536};
    for(unsigned i=2;i<4;++i) *(struct qs_chunk *)(arena+plan->nodes[i].vector_offset)=(struct qs_chunk){.capacity=QP_SCAN_BYTES/24,.ncols=2};
    struct qp_control control={0}; struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    void *mr[]={plan,&control,out,arena,io}; long long len[]={sizeof(*plan),sizeof(control),sizeof(*out),bytes,QS_SORT_IO_BYTES};
    struct femu_csd_args args={.numr=5,.mr_addr=mr,.mr_len=len};
    void **tokens=NULL; size_t ntokens=0; unsigned got=0; unsigned char seen[12]={0};
    uint64_t counts[12]={0},sums[12]={0},mins[12]={0},maxs[12]={0};
    for(unsigned l=0;l<rows;++l) {
        unsigned key=l%11; int matched=0;
        for(unsigned r=0;r<right_rows;++r) if(l%17 && r%17 && key==r%7) {
            unsigned v=join==QP_SEMI?l%19+1:r%13+1; matched=1;
            if(join==QP_ANTI) continue;
            ++counts[key]; sums[key]+=v; if(!mins[key] || mins[key]>v) mins[key]=v; if(maxs[key]<v) maxs[key]=v;
            if(join==QP_SEMI) break;
        }
        if(join==QP_INNER || join==QP_SEMI) { if(matched) seen[key]=1; }
        else if(join==QP_LEFT) seen[l%17?key:11]=1;
        else if(!matched) {
            key=l%17?key:11; unsigned v=l%19+1; seen[key]=1;
            ++counts[key]; sums[key]+=v; if(!mins[key] || mins[key]>v) mins[key]=v; if(maxs[key]<v) maxs[key]=v;
        }
    }
    for(unsigned iteration=0;!out->finished;++iteration) {
        assert(iteration<1000000);
        int64_t status=run(&args);
        if(status<0) { fprintf(stderr,"native plan status=%lld join=%u row=%u state=%llu\n",(long long)status,join,rows,(unsigned long long)control.state); abort(); }
        uint64_t op=(uint64_t)status>>60,id=(uint64_t)status&CS_IO_VALUE_MASK;
        if(op) {
            if(op==1) { tokens=realloc(tokens,(ntokens+1)*sizeof(*tokens)); assert(tokens); tokens[ntokens]=malloc(id); assert(tokens[ntokens]); memcpy(tokens[ntokens],io,id); args.cparam2=++ntokens; }
            else if(op==2) { assert(id && id<=ntokens && tokens[id-1]); memcpy(io,tokens[id-1],QS_SORT_IO_BYTES); args.cparam2=id; }
            else { assert(op==3 && id && id<=ntokens && tokens[id-1]); free(tokens[id-1]); tokens[id-1]=NULL; args.cparam2=id; }
            continue;
        }
        args.cparam2=0;
        for(unsigned r=0;r<out->rows;++r) {
            struct qs_cell *v=out->data[r].cells; unsigned key=v[0].isnull?11:(unsigned)v[0].value;
            assert(key<12 && seen[key]); seen[key]=0; ++got;
            if(v[1].isnull!=!counts[key] || v[2].value!=counts[key])
                fprintf(stderr,"join=%u key=%u count=%lld expected=%llu sum=%lld expected=%llu\n",join,key,(long long)v[2].value,(unsigned long long)counts[key],(long long)v[1].value,(unsigned long long)sums[key]);
            assert(v[1].isnull==!counts[key] && v[2].value==counts[key]);
            if(counts[key]) {
                assert(v[1].value==sums[key] && v[3].value==mins[key] && v[4].value==maxs[key]);
                struct qs_cell avg; assert(!qs_average((struct qs_cell){.value=sums[key]},counts[key],&avg));
                assert(v[5].value==avg.value && v[5].scale==avg.scale);
            }
        }
        if(control.index_nreads && !control.ready) {
            assert(hash==2 && control.node==1);
            for(unsigned i=0;i<control.index_nreads;++i) {
                struct qs_index_read *r=&control.index_reads[i];
                const unsigned char *page=r->heap?raw[1]:index;
                assert((r->heap && !r->block) || (!r->heap && r->block==1));
                memcpy(arena+j->input_offset+QS_SORT_IO_BYTES+(size_t)r->slot*QS_PAGE,page+(size_t)r->block*QS_PAGE,QS_PAGE);
            }
            control.ready=1;
        } else if(control.bytes && !control.ready) {
            assert(control.node>=2 && control.node<4);
            struct qp_node *n=&plan->nodes[control.node];
            memcpy(arena+n->input_offset,raw[control.node-2]+control.offset,control.bytes); control.ready=1;
        }
    }
    assert(got); for(unsigned i=0;i<12;++i) assert(!seen[i]);
    for(size_t i=0;i<ntokens;++i) free(tokens[i]);
    free(tokens); free(raw[0]); free(raw[1]); free(index); free(plan); free(arena); free(io); free(out);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
static int descending(const void *a,const void *b)
{ return (*(const unsigned *)a<*(const unsigned *)b)-(*(const unsigned *)a>*(const unsigned *)b); }
static void nested_sort(function run)
{
    unsigned rows=50003; size_t raw_bytes;
    unsigned char *raw=heap(rows,2,&raw_bytes);
    unsigned *expected=malloc(rows*sizeof(*expected)); assert(expected);
    for(unsigned i=0;i<rows;++i) expected[i]=(uint64_t)i*1009%100003;
    qsort(expected,rows,sizeof(*expected),descending);
    struct qp_plan *p=calloc(1,sizeof(*p)); assert(p); p->count=4;
    scan(&p->nodes[3]); p->nodes[3].bytes=raw_bytes;
    struct qp_node *a=&p->nodes[1]; a->kind=QP_AGGREGATE; a->left=2; a->right=-1; a->sorted_input=1;
    a->nkeys=a->expr.nkeys=1; a->expr.predicate=-1; a->expr.nout=a->expr.nexprs=2;
    a->expr.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=0,.type=Q_INTEGER};
    a->expr.exprs[1]=(struct qs_expr){.op=Q_AGGREF,.column=0,.type=Q_INTEGER};
    a->expr.naggs=1; a->expr.aggs[0]=(struct qs_agg){.kind=Q_SUM,.expr=2};
    a->expr.exprs[2]=(struct qs_expr){.op=Q_COLUMN,.column=1,.type=Q_INTEGER}; a->expr.nexprs=3;
    a->expr.outputs[0]=(struct qs_output){.index=0,.type_oid=23}; a->expr.outputs[1]=(struct qs_output){.index=1,.type_oid=20};
    for(unsigned i=0;i<3;i+=2) {
        struct qp_node *n=&p->nodes[i]; n->kind=QP_SORT; n->left=i+1; n->right=-1;
        n->expr=p->nodes[3].expr; n->expr.norder=1;
        n->expr.order[0]=(struct qs_order){.output=i?0:1,.descending=!i};
        n->expr.limit=-1;
    }
    p->nodes[0].expr.work_mem_bytes=4U*1024*1024;
    struct qp_node *leaf=&p->nodes[3]; leaf->input_offset=(qs_dict_bytes(512,65536)+4095)&~(size_t)4095;
    leaf->vector_offset=leaf->input_offset+QP_SCAN_BYTES;
    size_t bytes=leaf->vector_offset+qs_chunk_bytes(QP_SCAN_BYTES/24,2);
    unsigned char *arena=calloc(1,bytes),*io=calloc(1,QS_SORT_IO_BYTES); assert(arena && io);
    *(struct qs_dict *)arena=(struct qs_dict){.entries=512,.capacity=512,.data=65536};
    *(struct qs_chunk *)(arena+leaf->vector_offset)=(struct qs_chunk){.capacity=QP_SCAN_BYTES/24,.ncols=2};
    struct qp_control control={0}; struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    void *mr[]={p,&control,out,arena,io}; long long len[]={sizeof(*p),sizeof(control),sizeof(*out),bytes,QS_SORT_IO_BYTES};
    struct femu_csd_args args={.numr=5,.mr_addr=mr,.mr_len=len};
    void **tokens=NULL; unsigned ntokens=0,seen=0;
    while(!out->finished) {
        int64_t status=run(&args); assert(status>=0);
        uint64_t op=(uint64_t)status>>60,id=(uint64_t)status&CS_IO_VALUE_MASK;
        if(op) {
            if(op==1) { tokens=realloc(tokens,(ntokens+1)*sizeof(*tokens)); assert(tokens); tokens[ntokens]=malloc(id); assert(tokens[ntokens]); memcpy(tokens[ntokens],io,id); args.cparam2=++ntokens; }
            else if(op==2) { assert(id && id<=ntokens && tokens[id-1]); memcpy(io,tokens[id-1],QS_SORT_IO_BYTES); args.cparam2=id; }
            else { assert(op==3 && id && id<=ntokens && tokens[id-1]); free(tokens[id-1]); tokens[id-1]=NULL; args.cparam2=id; }
            continue;
        }
        args.cparam2=0;
        for(unsigned r=0;r<out->rows;++r) {
            if(seen>=rows || out->data[r].cells[1].value!=expected[seen]) {
                fprintf(stderr,"nested sort row=%u got=%lld expected=%u\n",seen,(long long)out->data[r].cells[1].value,seen<rows?expected[seen]:0); abort();
            }
            ++seen;
        }
        if(control.bytes && !control.ready) { memcpy(arena+leaf->input_offset,raw+control.offset,control.bytes); control.ready=1; }
    }
    assert(seen==rows);
    for(unsigned i=0;i<ntokens;++i) { assert(!tokens[i]); }
    free(tokens); free(raw); free(expected); free(p); free(arena); free(io); free(out);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
int main(int argc,char **argv)
{
    assert(argc==2); void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); assert(library);
    function run=(function)dlsym(library,"pg_sina_plan"); assert(run); assert(!cs_work_init(64*1024*1024));
    for(int hash=0;hash<3;++hash)
        for(unsigned join=QP_INNER;join<=QP_ANTI;++join) test(run,join,517,23,64,hash);
    test(run,QP_LEFT,20000,23,4,0);
    test(run,QP_LEFT,73,20000,8,1);
    nested_sort(run);
    puts("PASS native Plan joins, NULL/duplicate keys, MIN/MAX/AVG, spill and cleanup");
}
