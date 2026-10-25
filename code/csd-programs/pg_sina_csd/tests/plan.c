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
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <endian.h>
typedef int64_t (*function)(struct femu_csd_args *);
enum { TEST_SERIAL,TEST_PARALLEL,TEST_BUILD_PHASE,TEST_COUNT_ROWS,TEST_BUILD_FALLBACK,TEST_LARGE_SCAN,TEST_WIDE_HASH,TEST_SIGNED_HASH,TEST_BIGINT_HASH,TEST_HASH_GROUP,TEST_INDEX_PIPELINE,TEST_INDEX_HASH,TEST_INDEX_CHAIN,TEST_INDEX_EXPR,TEST_DEVICE_IO,TEST_PARTITION,TEST_PARTITION_SKEW,TEST_PARTITION_SERIAL, TEST_VECTOR_AGG=-1,TEST_HASH_QUAL=-2 };
static unsigned test_workers=4;
static bool test_scheduled;
/* Reservations must cover physical input even when filters/NDV estimates
 * are tiny, while unused string columns must not consume AFDM. */
static void dictionary_size(void)
{
    struct qp_plan *p=calloc(1,sizeof(*p)); assert(p); p->count=1;
    struct qp_node *n=&p->nodes[0]; n->kind=QP_SCAN; n->bytes=1024*1024;
    n->expr.nattrs=1; n->expr.cols[0].type=Q_STRING; n->expr.cols[0].slot=-1;
    unsigned entries,data; qp_dictionary_size(p,&entries,&data);
    assert(entries==QS_DICT_SMALL_ENTRIES && data==QS_DICT_SMALL_DATA);
    n->expr.cols[0].slot=0; n->estimated_rows=1;
    qp_dictionary_size(p,&entries,&data);
    assert(entries>=512+n->bytes/28+1 && !(entries&(entries-1)));
    assert(data>=65536+n->bytes && qs_dict_bytes(entries,data)<2*1024*1024);
    n->expr.nattrs=2; n->expr.cols[1]=n->expr.cols[0]; n->expr.cols[1].slot=1;
    qp_dictionary_size(p,&entries,&data);
    assert(entries>=512+2*(n->bytes/28+1) && data==65536+n->bytes);
    n->expr.nstrings=1; n->expr.strings[0].length=127;
    qp_dictionary_size(p,&entries,&data); assert(data>=65536+n->bytes+127);
    n->expr.nexprs=1; n->expr.exprs[0].type=Q_STRING; n->expr.exprs[0].op=Q_SUBSTRING;
    qp_dictionary_size(p,&entries,&data);
    assert(entries==QS_DICT_BIG_ENTRIES && data==QS_DICT_BIG_DATA);
    n->expr.nexprs=0; n->bytes=UINT64_MAX;
    qp_dictionary_size(p,&entries,&data);
    assert(entries==QS_DICT_BIG_ENTRIES && data==QS_DICT_BIG_DATA);
    free(p);
}
static void *dictionary_alloc(void *context,size_t bytes)
{ return cs_work_alloc(context,bytes); }
static void *dictionary_no_memory(void *context,size_t bytes)
{ (void)context; (void)bytes; return NULL; }
static void *dictionary_insert_worker(void *arg)
{
    struct qs_dict *dict=arg;
    for(unsigned i=0;i<4096;++i) {
        char text[81]; unsigned prefix=snprintf(text,sizeof(text),"key-%u-",i);
        memset(text+prefix,'x',80-prefix); text[80]=0;
        int id=qs_dict_insert(dict,text,80); assert(id>0);
        const char *bytes; unsigned length;
        assert(!qs_dict_get(dict,id,&bytes,&length) && length==80 && !memcmp(bytes,text,80));
        assert(qs_dict_find(dict,text,80)==id);
    }
    return NULL;
}
static void dictionary_growth(void)
{
    cs_work_context *context=cs_work_begin_limit(4U*1024*1024); assert(context);
    struct qs_dict dict={.entries=8192,.capacity=8192,.data=1024*1024,
        .context=context,.allocate=dictionary_alloc};
    int first=qs_dict_insert(&dict,"held",4); assert(first>0);
    const char *held; unsigned length; assert(!qs_dict_get(&dict,first,&held,&length) && length==4);
    struct qs_dict *original=qs_dict_storage(&dict);
    pthread_t threads[8];
    for(unsigned i=0;i<8;++i) assert(!pthread_create(&threads[i],NULL,dictionary_insert_worker,&dict));
    for(unsigned i=0;i<8;++i) assert(!pthread_join(threads[i],NULL));
    assert(dict.count==4097 && dict.bytes==4+4096*80 && qs_dict_storage(&dict)!=original);
    assert(!memcmp(held,"held",4)); /* A byte view borrowed before all growth. */
    struct cs_work_context_stats memory; assert(!cs_work_get_context_stats(context,&memory));
    assert(memory.used_bytes>qs_dict_bytes(qs_dict_storage(&dict)->entries,qs_dict_storage(&dict)->data));
    dict.allocate=dictionary_no_memory;
    unsigned before=dict.count; int status=0;
    for(unsigned i=0;i<8192 && status>=0;++i) {
        char text[100]; memset(text,'y',sizeof(text)); snprintf(text,32,"extra-%u",i);
        status=qs_dict_insert(&dict,text,sizeof(text));
    }
    assert(status<0 && dict.count>=before && !memcmp(held,"held",4));
    const char *bytes; assert(!qs_dict_get(&dict,first,&bytes,&length) && length==4 && !memcmp(bytes,"held",4));
    assert(!cs_work_end(context));
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
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
            if(side==2) { w32(t+24,row); w32(t+28,(int32_t)((uint64_t)row*1009%997)-498); }
            else if(row%17) { w32(t+24,row%(side?7:11)); w32(t+28,side?row%13+1:row%19+1); }
            else w32(t+24,side?row%13+1:row%19+1);
        }
    }
    return raw;
}
static unsigned char *index_page(unsigned rows)
{
    assert(40+rows*20<QS_PAGE);
    unsigned count=0;
    for(unsigned i=0;i<rows;++i) count+=i%17!=0;
    unsigned char *raw=calloc(2,QS_PAGE),*p=raw+QS_PAGE; assert(raw);
    w16(p+12,24+count*4); w16(p+14,QS_PAGE-16-count*16); w16(p+16,QS_PAGE-16); w16(p+18,QS_PAGE|4);
    w16(p+QS_PAGE-4,3); /* leaf | root, no siblings */
    unsigned item=0;
    for(unsigned key=0;key<7;++key) for(unsigned i=0;i<rows;++i) if(i%17 && i%7==key) {
        unsigned off=QS_PAGE-16-(++item)*16; unsigned char *t=p+off;
        w32(p+24+(item-1)*4,off|(1U<<15)|(16U<<17));
        w16(t+2,i/100); w16(t+4,i%100+1); w16(t+6,16); w32(t+8,key);
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
struct probe_test {
    function run;
    struct femu_csd_args args;
    struct qp_plan *plan;
    struct qp_control *control;
    struct qs_result *out;
    unsigned char *arena,*raw,*heap,*index;
    unsigned worker;
    void **tokens;
    size_t ntokens;
    atomic_uint_fast64_t *next_offset;
    bool *need_input;
    unsigned scan_node;
};
/* Independent linear physical-range oracle; source extents deliberately use
 * separated LBAs above 32 bits and split the first Heap page in half. */
static void namespace_read(struct femu_csd_args *a,unsigned count,const unsigned char *left,
    const unsigned char *right,const unsigned char *index)
{
    struct qp_plan *p=a->mr_addr[0]; struct femu_graph_read reads[FEMU_CSD_GRAPH_MAX_NODES];
    assert(count && count<=FEMU_CSD_GRAPH_MAX_NODES);
    memcpy(reads,a->mr_addr[4],count*sizeof(*reads));
    for(unsigned r=0;r<count;++r) {
        struct femu_graph_read *read=&reads[r]; unsigned found=0;
        uint64_t lba=le64toh(read->lba),bytes=(uint64_t)le32toh(read->blocks)*512,offset=le64toh(read->offset);
        assert(le32toh(read->nsid)==1 && le32toh(read->afdm_index)==3 &&
            offset<=(uint64_t)a->mr_len[3] && bytes<=(uint64_t)a->mr_len[3]-offset);
        for(unsigned n=2;n<4;++n) for(unsigned k=0;k<2;++k) {
            const struct qp_file_mapping *map=k?&p->indexes[n]:&p->files[n];
            const struct qs_extent *extents=(void *)((unsigned char *)p+map->offset);
            for(unsigned e=0;e<map->count;++e) if(lba>=extents[e].lba &&
                lba-extents[e].lba<=extents[e].bytes/512 && bytes<=extents[e].bytes-(lba-extents[e].lba)*512) {
                const unsigned char *source=k?index:n==2?left:right;
                memcpy((unsigned char *)a->mr_addr[3]+offset,source+extents[e].logical+(lba-extents[e].lba)*512,bytes);
                ++found;
            }
        }
        assert(found==1);
    }
    a->cparam2=count;
}
static void namespace_reentry(function run)
{
    size_t plan_bytes=sizeof(struct qp_plan)+sizeof(struct qs_extent),raw_bytes;
    struct qp_plan *p=calloc(1,plan_bytes); assert(p);
    p->count=1; p->scan_bytes=QP_SCAN_BYTES; scan(&p->nodes[0]);
    struct qp_node *n=&p->nodes[0]; n->bytes=QS_PAGE; n->expr.work_mem_bytes=4U*1024*1024;
    n->input_offset=(qs_dict_bytes(512,65536)+4095)&~(size_t)4095;
    n->vector_offset=n->input_offset+QP_SCAN_BYTES;
    p->files[0]=(struct qp_file_mapping){.offset=sizeof(*p),.nsid=1,.lba_bytes=512,.count=1};
    *(struct qs_extent *)(p+1)=(struct qs_extent){.lba=4096,.bytes=QS_PAGE};
    size_t arena_bytes=n->vector_offset+qs_chunk_bytes(QS_PAGE/24,2);
    unsigned char *raw=heap(1,2,&raw_bytes),*arena=calloc(1,arena_bytes),*io=calloc(1,QS_SORT_IO_BYTES);
    struct qs_result *out=calloc(1,sizeof(*out)); assert(arena && io && out);
    struct qp_control c={0};
    void *mr[]={p,&c,out,arena,io};
    long long len[]={plan_bytes,sizeof(c),sizeof(*out),arena_bytes,QS_SORT_IO_BYTES};
    for(unsigned trial=0;trial<3;++trial) {
        c=(struct qp_control){0}; memset(out,0,sizeof(*out));
        *(struct qs_dict *)arena=(struct qs_dict){.entries=512,.capacity=512,.data=65536};
        *(struct qs_chunk *)(arena+n->vector_offset)=(struct qs_chunk){.capacity=QS_PAGE/24,.ncols=2};
        struct femu_csd_args a={.numr=5,.mr_addr=mr,.mr_len=len};
        assert((uint64_t)run(&a)==(CS_NEED_NVM_READ|1));
        const struct femu_graph_read *read=(void *)io;
        assert(le64toh(read->lba)==4096 && le32toh(read->blocks)==QS_PAGE/512 &&
            le32toh(read->afdm_index)==3 && le64toh(read->offset)==n->input_offset);
        if(trial==1) {
            a.cparam2=2; assert(run(&a)==-1); /* mismatched completion */
        } else if(trial==2) {
            memcpy(arena+n->input_offset,raw,raw_bytes); a.cparam2=1;
            assert(!run(&a) && out->finished && out->rows==1 &&
                out->data[0].cells[0].value==0 && out->data[0].cells[1].value==-498);
        }
        c.close=1; a.cparam2=0; assert(!run(&a));
        struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
    }
    free(raw); free(arena); free(io); free(out); free(p);
}
static void *probe_worker(void *arg)
{
    struct probe_test *t=arg;
    void *ranges[5]; long long lengths[5];
    memcpy(ranges,t->args.mr_addr,sizeof(ranges)); memcpy(lengths,t->args.mr_len,sizeof(lengths));
    ranges[4]=(unsigned char *)ranges[4]+t->worker*QS_SORT_IO_BYTES; lengths[4]=QS_SORT_IO_BYTES;
    t->args.mr_addr=ranges; t->args.mr_len=lengths;
    for(unsigned iteration=0;;++iteration) {
        assert(iteration<1000000);
        struct qp_control *c=t->control;
        if(!csIsReentry(&t->args)) t->args.cparam1=(t->worker<<1)|(t->args.cparam1&1);
        if(t->next_offset && *t->need_input && !csIsReentry(&t->args)) {
            const struct qp_node *n=&t->plan->nodes[t->scan_node-1];
            uint64_t offset=atomic_fetch_add(t->next_offset,t->plan->scan_bytes);
            if(offset>n->bytes) offset=n->bytes;
            size_t bytes=n->bytes-offset<t->plan->scan_bytes?n->bytes-offset:t->plan->scan_bytes;
            size_t input=t->worker>1?n->parallel_input+(t->worker-2)*t->plan->parallel_stride:n->input_offset;
            memcpy(t->arena+input,((t->scan_node-1==3 || t->scan_node-1==6)?t->heap:t->raw)+offset,bytes);
            t->args.cparam1|=QP_SCAN_INPUT | ((offset/QS_PAGE)<<QP_SCAN_PAGE_SHIFT);
            *t->need_input=false;
        } else if(!t->next_offset && !csIsReentry(&t->args) && (c->index_nreads || c->bytes) && !c->ready) {
            assert(!t->plan->files[2].count); /* mapped workers must continue without host LOADs */
            const struct qp_node *n=&t->plan->nodes[c->node];
            size_t offset=t->worker>1?n->parallel_input+(t->worker-2)*t->plan->parallel_stride:n->input_offset;
            if(c->index_nreads) {
                for(unsigned i=0;i<c->index_nreads;++i) {
                    const struct qs_index_read *r=&c->index_reads[i];
                    memcpy(t->arena+offset+QS_SORT_IO_BYTES+(size_t)r->slot*QS_PAGE,
                        (r->heap?t->heap:t->index)+(size_t)r->block*QS_PAGE,QS_PAGE);
                }
                t->args.cparam1|=QP_PAGES_INPUT;
            } else {
                memcpy(t->arena+offset,((c->node==3 || c->node==6)?t->heap:t->raw)+c->offset,c->bytes);
                c->ready=1;
            }
        }
        int64_t status=t->run(&t->args);
        if(status<0) fprintf(stderr,"worker %u status=%lld node=%u kind=%d ready=%u bytes=%u offset=%llu\n",
            t->worker,(long long)status,c->error_node,t->plan->nodes[c->error_node].kind,
            c->ready,c->bytes,(unsigned long long)c->offset);
        assert(status>=0);
        if((uint64_t)status>>60) {
            uint64_t op=(uint64_t)status>>60,id=(uint64_t)status&CS_IO_VALUE_MASK;
            if(op==4) { namespace_read(&t->args,id,t->raw,t->heap,t->index); continue; }
            assert(op==2 && id && id<=t->ntokens && t->tokens[id-1]);
            memcpy(t->args.mr_addr[4],t->tokens[id-1],QS_SORT_IO_BYTES);
            t->args.cparam2=id; continue;
        }
        t->args.cparam2=0;
        if(t->next_offset) {
            assert((unsigned)status<=7);
            assert(((status&QP_OUTPUT_READY)!=0)==(t->out->rows!=0));
            assert(((status&QP_WORKER_FINISHED)!=0)==(t->out->finished!=0));
            *t->need_input=(status&QP_INPUT_DRAINED)!=0;
        }
        if(t->out->rows || t->out->finished) return NULL;
        /* A direct sink can consume a batch without emitting rows or I/O. */
    }
}
static void test(function run,unsigned join,unsigned rows,unsigned right_rows,unsigned memory,int hash,int parallel)
{
    struct qp_plan *plan=calloc(1,sizeof(*plan)); assert(plan); plan->count=4; plan->scan_bytes=parallel==TEST_LARGE_SCAN?QS_CHUNK:test_workers>4?QS_PAGE:QP_SCAN_BYTES;
    scan(&plan->nodes[2]); scan(&plan->nodes[3]);
    __int128 factor=parallel==TEST_WIDE_HASH?((__int128)1<<64):parallel==TEST_SIGNED_HASH?-1:parallel==TEST_BIGINT_HASH?INT64_C(1)<<32:1;
    if(parallel==TEST_INDEX_EXPR && join!=QP_SEMI && join!=QP_ANTI) factor=-2;
    if(parallel==TEST_WIDE_HASH) {
        struct qs_plan *p=&plan->nodes[3].expr; p->nexprs=6;
        /* Constants are int64; two products create a genuine int128 payload. */
        p->exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL,.constant=INT64_C(1)<<32,.scale=2,.dscale=3};
        p->exprs[3]=(struct qs_expr){.op=Q_MUL,.type=Q_DECIMAL,.left=1,.right=2,.scale=2,.dscale=3};
        p->exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=INT64_C(1)<<32};
        p->exprs[5]=(struct qs_expr){.op=Q_MUL,.type=Q_DECIMAL,.left=3,.right=4,.scale=2,.dscale=3};
        p->outputs[1]=(struct qs_output){.index=5,.type_oid=1700};
    }
    if(parallel==TEST_SIGNED_HASH || parallel==TEST_BIGINT_HASH) {
        struct qs_plan *p=&plan->nodes[3].expr; p->nexprs=4;
        p->exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=(int64_t)factor};
        p->exprs[3]=(struct qs_expr){.op=Q_MUL,.type=Q_INTEGER,.left=1,.right=2};
        p->outputs[1]=(struct qs_output){.index=3,.type_oid=parallel==TEST_BIGINT_HASH?20:23};
    }
    struct qp_node *j=&plan->nodes[1]; j->kind=QP_JOIN; j->left=2; j->right=3; j->join_type=join; j->join_qual=-1;
    j->hash=hash==1;
    if(parallel==TEST_WIDE_HASH && hash!=2) {
        /* Fuse a second projection over a genuine int128 scan expression.
         * It must keep the wide payload and numeric display scale. */
        plan->count=5; j->right=4;
        struct qp_node *projection=&plan->nodes[4];
        projection->kind=QP_PROJECT; projection->left=3; projection->right=-1;
        struct qs_plan *p=&projection->expr; p->predicate=-1; p->nattrs=p->nout=2; p->nexprs=4;
        p->exprs[0]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=0};
        p->exprs[1]=(struct qs_expr){.op=Q_COLUMN,.type=Q_DECIMAL,.column=1,.scale=2};
        p->exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL,.scale=2};
        p->exprs[3]=(struct qs_expr){.op=Q_ADD,.type=Q_DECIMAL,.left=1,.right=2,.scale=2};
        p->outputs[0]=(struct qs_output){.index=0,.type_oid=23};
        p->outputs[1]=(struct qs_output){.index=3,.type_oid=1700};
    }
    if(hash==2) {
        j->kind=QP_INDEX_JOIN;
        plan->nodes[3].expr.join=(struct qs_join){.indexid=1,.indexroot=1,.index_key_bytes=4,.build_key=1};
        if(parallel==TEST_INDEX_EXPR) {
            struct qs_plan *p=&plan->nodes[3].expr; p->nexprs=6;
            p->exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=-2};
            p->exprs[3]=(struct qs_expr){.op=Q_MUL,.type=Q_INTEGER,.left=1,.right=2};
            p->exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=5};
            p->exprs[5]=(struct qs_expr){.op=Q_GE,.type=Q_BOOLEAN,.left=1,.right=4};
            p->predicate=5; p->outputs[1].index=3;
        }
    }
    j->nkeys=1; j->left_keys[0]=j->right_keys[0]=0; j->expr.predicate=-1; j->expr.nattrs=4; j->expr.nexprs=4; j->expr.nout=3;
    for(unsigned i=0;i<4;++i) j->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=i};
    for(unsigned i=0;i<3;++i) j->expr.outputs[i]=(struct qs_output){.index=i==2?3:(int)i,.type_oid=23};
    if(join==QP_SEMI || join==QP_ANTI) j->expr.outputs[2].index=1;
    if(parallel==TEST_INDEX_EXPR) {
        /* Reject initial candidates by both the right filter and a residual
         * Join condition. A match stops only its own key, not its neighbors;
         * NULL keys and never-matching keys must survive ANTI correctly. */
        j->expr.nexprs=8;
        j->expr.exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=-16};
        j->expr.exprs[5]=(struct qs_expr){.op=Q_LE,.type=Q_BOOLEAN,.left=3,.right=4};
        j->expr.exprs[6]=(struct qs_expr){.op=Q_CONST,.type=Q_BOOLEAN,.isnull=1};
        j->expr.exprs[7]=(struct qs_expr){.op=Q_CASE,.type=Q_BOOLEAN,.left=5,.right=5,.otherwise=6};
        j->join_qual=7;
    }
    if(parallel==TEST_HASH_QUAL) {
        j->expr.nexprs=8;
        j->expr.exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=8};
        j->expr.exprs[5]=(struct qs_expr){.op=Q_GE,.type=Q_BOOLEAN,.left=3,.right=4};
        j->expr.exprs[6]=(struct qs_expr){.op=Q_CONST,.type=Q_BOOLEAN,.isnull=1};
        j->expr.exprs[7]=(struct qs_expr){.op=Q_CASE,.type=Q_BOOLEAN,.left=5,.right=5,.otherwise=6};
        j->join_qual=7;
    }
    if(parallel==TEST_INDEX_PIPELINE) {
        /* A membership Hash Join below an INNER Index Join preserves its
         * results while exercising retained scan windows and both I/O paths. */
        assert(hash==2 && join==QP_INNER); plan->count=5;
        struct qp_node *probe=&plan->nodes[4]; *probe=*j;
        probe->kind=QP_JOIN; probe->hash=1; probe->join_type=QP_SEMI; probe->expr.nout=2;
        j->left=4;
    }
    if(parallel==TEST_INDEX_HASH || parallel==TEST_INDEX_CHAIN) {
        /* An Index SEMI output is consumed by a Hash Join as columns, with
         * duplicate matches spanning many index chunks and Heap windows. */
        assert(hash==2 && join==QP_INNER); plan->count=5;
        struct qp_node *probe=&plan->nodes[4]; *probe=*j;
        probe->join_type=QP_SEMI; probe->expr.nout=2;
        j->kind=QP_JOIN; j->hash=1; j->left=4;
        /* Retain residual candidates while the surrounding Index cursors
         * cross lookup batches and the borrowed scan window is exhausted. */
        j->expr.nexprs=6;
        j->expr.exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER};
        j->expr.exprs[5]=(struct qs_expr){.op=Q_GE,.type=Q_BOOLEAN,.left=3,.right=4};
        j->join_qual=5;
    }
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
    if(parallel==TEST_INDEX_CHAIN) {
        /* The upper Index consumer must not flush Hash's partial columns
         * while the lower Index cursor still owns a pending lookup batch. */
        plan->count=7; plan->nodes[6]=plan->nodes[3];
        struct qp_node *outer=&plan->nodes[5]; *outer=plan->nodes[4];
        outer->left=1; outer->right=6; outer->expr.nattrs=5;
        outer->expr.nexprs=outer->expr.nout=3;
        for(unsigned i=0;i<3;++i) {
            outer->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=i};
            outer->expr.outputs[i]=(struct qs_output){.index=i,.type_oid=23};
        }
        a->left=5;
    }
    if(parallel==TEST_VECTOR_AGG) {
        /* A nullable CASE projection feeds all five local aggregate kinds. */
        j->kind=QP_PROJECT; j->right=-1; j->expr.nattrs=2; j->expr.nexprs=7;
        j->expr.exprs[4]=(struct qs_expr){.op=Q_IS_NULL,.type=Q_BOOLEAN,.left=0};
        j->expr.exprs[5]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.isnull=1};
        j->expr.exprs[6]=(struct qs_expr){.op=Q_CASE,.type=Q_INTEGER,.left=4,.right=5,.otherwise=1};
        j->expr.outputs[2].index=6;
        a->nkeys=a->expr.nkeys=0;
        a->expr.exprs[0]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER};
    }
    if(parallel==TEST_COUNT_ROWS || parallel>=TEST_PARTITION) {
        /* COUNT(*) needs row cardinality but no payload from the probe pipeline. */
        j->expr.nout=0; a->nkeys=a->expr.nkeys=a->expr.nattrs=0;
        a->expr.naggs=a->expr.nout=1; a->expr.aggs[0]=(struct qs_agg){.kind=Q_COUNT_STAR};
        a->expr.outputs[0]=(struct qs_output){.index=3,.type_oid=20};
    }
    if(parallel==TEST_HASH_GROUP) {
        a->hash=1; a->estimated_rows=12; a->expr.naggs=1; a->expr.nout=2;
        a->expr.aggs[0]=(struct qs_agg){.expr=2,.kind=Q_COUNT};
        a->expr.outputs[1]=(struct qs_output){.index=3,.type_oid=20};
    }
    if(parallel==TEST_BUILD_FALLBACK) {
        /* The small inner build needs a parallel grouped input; the large
         * outer build falls back while its own scan still has buffered rows. */
        plan->count=7; j->left=4;
        plan->nodes[4]=*j; plan->nodes[4].left=2; plan->nodes[4].right=5;
        plan->nodes[4].expr.nout=2;
        plan->nodes[5]=*a; plan->nodes[5].left=6; plan->nodes[5].nkeys=plan->nodes[5].expr.nkeys=1;
        plan->nodes[5].expr.nattrs=2; plan->nodes[5].expr.naggs=1; plan->nodes[5].expr.nout=2;
        plan->nodes[5].expr.aggs[0].expr=1;
        plan->nodes[5].expr.outputs[1]=(struct qs_output){.index=3,.type_oid=20};
        plan->nodes[6]=plan->nodes[3];
    }
    size_t sizes[2]; unsigned char *raw[2]={heap(rows,0,&sizes[0]),heap(right_rows,1,&sizes[1])};
    if(parallel==TEST_PARTITION_SKEW)
        for(unsigned side=0;side<2;++side) for(unsigned i=0;i<(side?right_rows:rows);++i)
            if(i%17) w32(raw[side]+(i/100)*QS_PAGE+QS_PAGE-(i%100+1)*40+24,1);
    size_t bytes=(qs_dict_bytes(512,65536)+4095)&~(size_t)4095;
    for(unsigned i=2;i<4;++i) {
        plan->nodes[i].bytes=sizes[i-2]; plan->nodes[i].input_offset=bytes; bytes+=plan->scan_bytes;
        plan->nodes[i].vector_offset=bytes; bytes+=(qs_chunk_bytes(plan->scan_bytes/24,2)+4095)&~(size_t)4095;
    }
    if(parallel==TEST_BUILD_FALLBACK || parallel==TEST_INDEX_CHAIN) {
        plan->nodes[6].bytes=plan->nodes[3].bytes;
        plan->nodes[6].input_offset=plan->nodes[3].input_offset;
        plan->nodes[6].vector_offset=plan->nodes[3].vector_offset;
    }
    for(unsigned i=0;i<plan->count;++i) if(plan->nodes[i].kind==QP_INDEX_JOIN) {
        struct qp_node *indexed=&plan->nodes[i];
        indexed->input_offset=bytes; bytes+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE;
        indexed->vector_offset=bytes; bytes+=4096+((qs_chunk_bytes(256,4)+4095)&~(size_t)4095);
    }
    size_t parallel_bytes=bytes;
    if(parallel && parallel!=TEST_PARTITION_SERIAL) {
        plan->workers=test_workers; plan->nodes[parallel==TEST_INDEX_CHAIN?5:1].parallel=1;
        plan->nodes[2].parallel_input=parallel_bytes; parallel_bytes+=plan->scan_bytes;
        plan->nodes[2].parallel_vector=parallel_bytes; parallel_bytes+=(qs_chunk_bytes(plan->scan_bytes/24,2)+4095)&~(size_t)4095;
        for(unsigned i=0;i<plan->count;++i) if(plan->nodes[i].kind==QP_INDEX_JOIN) {
            struct qp_node *indexed=&plan->nodes[i];
            indexed->parallel_input=parallel_bytes; parallel_bytes+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE;
            indexed->parallel_vector=parallel_bytes; parallel_bytes+=4096+((qs_chunk_bytes(256,4)+4095)&~(size_t)4095);
        }
        if(parallel==TEST_BUILD_PHASE || parallel==TEST_BUILD_FALLBACK) {
            plan->nodes[3].parallel=1;
            plan->nodes[3].parallel_input=plan->nodes[2].parallel_input;
            plan->nodes[3].parallel_vector=plan->nodes[2].parallel_vector;
        }
        if(parallel==TEST_BUILD_FALLBACK) {
            plan->nodes[6].parallel=1;
            plan->nodes[6].parallel_input=plan->nodes[2].parallel_input;
            plan->nodes[6].parallel_vector=plan->nodes[2].parallel_vector;
        }
        plan->parallel_stride=parallel_bytes-bytes; bytes+=test_workers*plan->parallel_stride;
    }
    unsigned char *index=hash==2?index_page(right_rows):NULL;
    unsigned char *arena=calloc(1,bytes),*io=calloc(plan->workers+1,QS_SORT_IO_BYTES); assert(arena && io);
    struct qs_dict *dict=(void *)arena; *dict=(struct qs_dict){.entries=512,.capacity=512,.data=65536};
    for(unsigned i=2;i<4;++i) *(struct qs_chunk *)(arena+plan->nodes[i].vector_offset)=(struct qs_chunk){.capacity=qp_scan_capacity(plan,&plan->nodes[i]),.ncols=2};
    struct qp_control *controls=calloc(plan->workers+1,sizeof(*controls)); assert(controls);
#define control controls[0]
    struct qs_result *out=calloc(plan->workers+1,sizeof(*out)); assert(out);
    for(unsigned w=0;w<plan->workers;++w)
        *(struct qs_chunk *)(arena+plan->nodes[2].parallel_vector+w*plan->parallel_stride)=(struct qs_chunk){.capacity=plan->scan_bytes/24,.ncols=2};
    size_t plan_bytes=sizeof(*plan);
    if(parallel==TEST_DEVICE_IO) {
        plan=realloc(plan,plan_bytes+5*sizeof(struct qs_extent)); assert(plan);
        for(unsigned i=2;i<4;++i) {
            struct qp_file_mapping *map=&plan->files[i];
            *map=(struct qp_file_mapping){.offset=plan_bytes,.nsid=1,.lba_bytes=512,.count=2};
            struct qs_extent *e=(void *)((unsigned char *)plan+plan_bytes); plan_bytes+=2*sizeof(*e);
            e[0]=(struct qs_extent){.logical=0,.lba=i*100000,.bytes=QS_PAGE/2};
            e[1]=(struct qs_extent){.logical=QS_PAGE/2,.lba=UINT64_C(0x100000000)+i*100000,.bytes=plan->nodes[i].bytes-QS_PAGE/2};
        }
        plan->indexes[3]=(struct qp_file_mapping){.offset=plan_bytes,.nsid=1,.lba_bytes=512,.count=1};
        *(struct qs_extent *)((unsigned char *)plan+plan_bytes)=(struct qs_extent){.lba=UINT64_C(0x200000000),.bytes=2*QS_PAGE};
        plan_bytes+=sizeof(struct qs_extent);
    }
    void *mr[]={plan,&control,out,arena,io}; long long len[]={plan_bytes,(plan->workers+1)*sizeof(*controls),(plan->workers+1)*sizeof(*out),bytes,(plan->workers+1)*QS_SORT_IO_BYTES};
    struct femu_csd_args args={.numr=5,.mr_addr=mr,.mr_len=len};
    args.cparam1=parallel==TEST_PARALLEL || parallel==TEST_BUILD_PHASE || parallel==TEST_HASH_GROUP || parallel==TEST_PARTITION || parallel==TEST_PARTITION_SKEW;
    void **tokens=NULL; size_t ntokens=0; unsigned got=0; unsigned char seen[12]={0};
    uint64_t counts[12]={0},sums[12]={0},mins[12]={0},maxs[12]={0};
    for(unsigned l=0;l<rows;++l) {
        unsigned key=parallel==TEST_PARTITION_SKEW?1:l%11; int matched=0;
        for(unsigned r=0;r<right_rows;++r) if(l%17 && r%17 && key==(parallel==TEST_PARTITION_SKEW?1:r%7) &&
            (parallel!=TEST_INDEX_EXPR || r%13+1>=8) &&
            (parallel!=TEST_HASH_QUAL || r%13+1>=8)) {
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
    if(parallel==TEST_VECTOR_AGG) {
        memset(seen,0,sizeof(seen)); memset(counts,0,sizeof(counts)); memset(sums,0,sizeof(sums));
        memset(mins,0,sizeof(mins)); memset(maxs,0,sizeof(maxs)); seen[0]=1;
        for(unsigned i=0;i<rows;++i) if(i%17) {
            unsigned v=i%19+1; ++counts[0]; sums[0]+=v;
            if(!mins[0] || v<mins[0]) mins[0]=v;
            if(v>maxs[0]) maxs[0]=v;
        }
    }
    uint64_t available=0,done=0; unsigned epoch=0;
    atomic_uint_fast64_t host_offset=0;
    bool need_input[QS_WORKERS]={0};
    for(unsigned iteration=0;!out->finished;++iteration) {
        if(control.parallel && !csIsReentry(&args)) {
            pthread_t threads[QS_WORKERS]; struct probe_test tasks[QS_WORKERS]; uint64_t started=0;
            for(unsigned w=0;w<plan->workers;++w) if(!((available|done)&(UINT64_C(1)<<w))) {
                tasks[w]=(struct probe_test){run,args,plan,&controls[w+1],&out[w+1],arena,raw[0],raw[1],index,w+1,tokens,ntokens,NULL,NULL,0};
                tasks[w].args.cparam1=((w+1)<<1)|(args.cparam1&1);
                if(test_scheduled && control.scan_node) {
                    tasks[w].next_offset=&host_offset; tasks[w].need_input=&need_input[w];
                    tasks[w].scan_node=control.scan_node;
                }
                assert(!pthread_create(&threads[w],NULL,probe_worker,&tasks[w])); started|=UINT64_C(1)<<w;
            }
            for(unsigned w=0;w<plan->workers;++w) if(started&(UINT64_C(1)<<w)) {
                assert(!pthread_join(threads[w],NULL)); available|=UINT64_C(1)<<w;
                if(out[w+1].finished) done|=UINT64_C(1)<<w;
            }
            control.available=available;
        }
        if(iteration==1000000) { fprintf(stderr,"STUCK join=%u hash=%d parallel=%d epoch=%u avail=%llu done=%llu active=%u rows=%u node=%u reads=%u bytes=%u\n",join,hash,parallel,control.epoch,(unsigned long long)available,(unsigned long long)done,control.parallel,out->rows,control.node,control.index_nreads,control.bytes); abort(); }
        uint64_t consumed=csIsReentry(&args)?control.drained:0; unsigned previous_epoch=control.epoch;
        int64_t status=run(&args);
        if(control.epoch==previous_epoch) assert((control.drained&consumed)==consumed);
        if(status<0) { fprintf(stderr,"native plan status=%lld join=%u row=%u state=%llu\n",(long long)status,join,rows,(unsigned long long)control.state); abort(); }
        uint64_t op=(uint64_t)status>>60,id=(uint64_t)status&CS_IO_VALUE_MASK;
        if(op) {
            if(op==4) { namespace_read(&args,id,raw[0],raw[1],index); continue; }
            if(op==1) { tokens=realloc(tokens,(ntokens+1)*sizeof(*tokens)); assert(tokens); tokens[ntokens]=malloc(id); assert(tokens[ntokens]); memcpy(tokens[ntokens],io,id); args.cparam2=++ntokens; }
            else if(op==2) { assert(id && id<=ntokens && tokens[id-1]); memcpy(io,tokens[id-1],QS_SORT_IO_BYTES); args.cparam2=id; }
            else { assert(op==3 && id && id<=ntokens && tokens[id-1]); free(tokens[id-1]); tokens[id-1]=NULL; args.cparam2=id; }
            continue;
        }
        args.cparam2=0; args.cparam1&=~QP_PAGES_INPUT; available&=~control.drained;
        if(control.epoch!=epoch) {
            epoch=control.epoch; available=done=0;
            atomic_store(&host_offset,0);
            for(unsigned w=0;w<plan->workers;++w) need_input[w]=true;
        }
        for(unsigned r=0;r<out->rows;++r) {
            if(parallel==TEST_COUNT_ROWS || parallel>=TEST_PARTITION) {
                uint64_t expected=0; for(unsigned i=0;i<12;++i) expected+=counts[i];
                assert(out->data[r].cells[0].value==expected); ++got; continue;
            }
            struct qs_cell *v=out->data[r].cells; unsigned key=v[0].isnull?11:(unsigned)v[0].value;
            assert(key<12 && seen[key]); seen[key]=0; ++got;
            if(parallel==TEST_HASH_GROUP) { assert(!v[1].isnull && v[1].value==counts[key]); continue; }
            if(v[1].isnull!=!counts[key] || v[2].value!=counts[key])
                fprintf(stderr,"join=%u key=%u count=%lld expected=%llu sum=%lld expected=%llu\n",join,key,(long long)v[2].value,(unsigned long long)counts[key],(long long)v[1].value,(unsigned long long)sums[key]);
            assert(v[1].isnull==!counts[key] && v[2].value==counts[key]);
            if(counts[key]) {
                unsigned min=factor<0?maxs[key]:mins[key],max=factor<0?mins[key]:maxs[key];
                assert(v[1].value==(__int128)sums[key]*factor && v[3].value==(__int128)min*factor && v[4].value==(__int128)max*factor);
                if(parallel==TEST_WIDE_HASH) assert(v[1].scale==2 && v[1].dscale==3);
                struct qs_cell avg; assert(!qs_average((struct qs_cell){.value=(__int128)sums[key]*factor,.scale=parallel==TEST_WIDE_HASH?2:0,.dscale=parallel==TEST_WIDE_HASH?3:0},counts[key],&avg));
                assert(v[5].value==avg.value && v[5].scale==avg.scale);
            }
        }
        if(control.index_nreads && !control.ready) {
            assert(parallel!=TEST_DEVICE_IO);
            assert(hash==2 && control.node==1);
            for(unsigned i=0;i<control.index_nreads;++i) {
                struct qs_index_read *r=&control.index_reads[i];
                const unsigned char *page=r->heap?raw[1]:index;
                assert((r->heap && !r->block) || (!r->heap && r->block==1));
                memcpy(arena+j->input_offset+QS_SORT_IO_BYTES+(size_t)r->slot*QS_PAGE,page+(size_t)r->block*QS_PAGE,QS_PAGE);
            }
            args.cparam1|=QP_PAGES_INPUT;
        } else if(control.bytes && !control.ready) {
            assert(parallel!=TEST_DEVICE_IO);
            assert((control.node>=2 && control.node<4) || control.node==6);
            struct qp_node *n=&plan->nodes[control.node];
            memcpy(arena+n->input_offset,raw[control.node==2?0:1]+control.offset,control.bytes); control.ready=1;
        }
    }
    if(hash==2 && memory==4 && parallel==TEST_SERIAL) assert(ntokens); /* page-ready flag survives spill reentry */
    if(parallel==TEST_DEVICE_IO && memory==4) assert(ntokens); /* namespace and spill reentry share one CU I/O AFDM */
    if(parallel==TEST_HASH_QUAL && right_rows==150000) assert(ntokens);
    if(test_workers>4 && parallel) assert(out->metrics.workers==test_workers && control.epoch);
    assert(got); if(parallel!=TEST_COUNT_ROWS && parallel<TEST_PARTITION) for(unsigned i=0;i<12;++i) assert(!seen[i]);
    if(args.cparam1&1) {
        uint64_t rounds=0;
        for(unsigned i=0;i<plan->count;++i) {
            const struct qp_pipeline_profile *p=&plan->pipelines[i]; rounds+=p->rounds;
            if(p->rounds) assert(p->wall_ns && p->worker_ns && p->coordinator_ns);
        }
        assert(rounds==control.epoch);
        if(parallel==TEST_BUILD_PHASE) assert(plan->pipelines[3].sink_rows);
        if(parallel==TEST_HASH_GROUP) assert(plan->pipelines[1].sink_rows);
        uint64_t children=0;
        for(unsigned op=0;op<QS_OPERATORS;++op) if(op!=QS_JOIN_PROBE) children+=out->profile.operators[op].cpu_ns;
        assert(out->profile.operators[QS_JOIN_PROBE].cpu_ns>=children);
    } else for(unsigned i=0;i<plan->count;++i) assert(!plan->pipelines[i].rounds);
    if(parallel>=TEST_PARTITION) {
        assert(ntokens && !out->metrics.runs && out->metrics.workers==(parallel==TEST_PARTITION_SERIAL?1:4));
        if(parallel==TEST_PARTITION_SERIAL) assert(!control.epoch);
        else assert(memory==8?control.epoch==1:control.epoch>1);
    }
    for(size_t i=0;i<ntokens;++i) free(tokens[i]);
    free(controls); free(tokens); free(raw[0]); free(raw[1]); free(index); free(plan); free(arena); free(io); free(out);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
#undef control
static int descending(const void *a,const void *b)
{ return (*(const int32_t *)a<*(const int32_t *)b)-(*(const int32_t *)a>*(const int32_t *)b); }
struct expected_group { struct qs_cell value; unsigned key; };
static int group_order(const void *a,const void *b)
{
    const struct expected_group *x=a,*y=b; int cmp;
    if(x->value.isnull || y->value.isnull) return x->value.isnull!=y->value.isnull?(x->value.isnull?1:-1):(x->key>y->key)-(x->key<y->key);
    assert(!qs_compare(x->value,y->value,&cmp));
    return cmp?-cmp:(x->key>y->key)-(x->key<y->key);
}
static void nested_sort(function run,unsigned type,unsigned hash,unsigned kind)
{
    unsigned rows=hash==3?0:50003; size_t raw_bytes;
    unsigned char *raw=heap(rows,2,&raw_bytes);
    int32_t *expected=malloc((rows?rows:1)*sizeof(*expected)); assert(expected);
    for(unsigned i=0;i<rows;++i) expected[i]=(int32_t)((uint64_t)i*1009%997)-498;
    qsort(expected,rows,sizeof(*expected),descending);
    unsigned groups=hash?(rows?20004:0):rows;
    struct expected_group *want=calloc(groups?groups:1,sizeof(*want)); unsigned *counts=calloc(groups?groups:1,sizeof(*counts)); assert(want && counts);
    if(hash) {
        for(unsigned i=0;i<rows;++i) {
            unsigned key=i%20003,at=i%17?key:20003;
            unsigned char *t=raw+(i/100)*QS_PAGE+QS_PAGE-(i%100+1)*40;
            int key_null=i%17==0,value_null=i%19==0 || key==7;
            int value=(int32_t)((uint64_t)i*1009%997)-498;
            t[23]=(key_null?0:1)|(value_null?0:2);
            unsigned off=24;
            if(!key_null) { w32(t+off,key); off+=4; }
            if(!value_null) w32(t+off,value);
            want[at].key=key_null?UINT32_MAX:key;
            if(!value_null || kind==Q_COUNT_STAR) { want[at].value.value+=value; ++counts[at]; }
        }
        for(unsigned i=0;i<groups;++i) {
            if(kind==Q_AVG && counts[i]) {
                assert(!qs_average(want[i].value,counts[i],&want[i].value));
                want[i].value.value*=2; ++want[i].value.scale; ++want[i].value.dscale;
            }
            if(kind==Q_COUNT || kind==Q_COUNT_STAR) want[i].value=(struct qs_cell){.value=counts[i]};
            else if(!counts[i]) want[i].value.isnull=1;
        }
        if(hash==4 || hash==5) {
            unsigned kept=0;
            for(unsigned i=0;i<groups;++i) if(!want[i].value.isnull) want[kept++]=want[i];
            groups=kept;
        }
        qsort(want,groups,sizeof(*want),group_order);
    }
    struct qp_plan *p=calloc(1,sizeof(*p)); assert(p); p->count=4; p->scan_bytes=QP_SCAN_BYTES;
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
    if(hash) { a->hash=1; a->sorted_input=0; a->left=3; a->estimated_rows=groups; a->expr.aggs[0].kind=kind; }
    if(hash && kind==Q_AVG) {
        a->expr.nexprs=5;
        a->expr.exprs[3]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL,.constant=2,.scale=1,.dscale=1};
        a->expr.exprs[4]=(struct qs_expr){.op=Q_MUL,.type=Q_DECIMAL,.left=1,.right=3};
        a->expr.outputs[1].index=4;
    }
    if(hash==4 || hash==5) {
        /* Underestimated integer groups must grow with the same hash function;
         * HAVING must skip rejected groups in both resident and spilled output. */
        a->estimated_rows=a->estimated_groups=128;
        unsigned predicate=a->expr.nexprs++;
        a->expr.exprs[predicate]=(struct qs_expr){.op=Q_IS_NOT_NULL,.left=1,.type=Q_BOOLEAN};
        a->expr.predicate=predicate;
    }
    p->nodes[0].expr.work_mem_bytes=((hash==2 || hash==4)?64U:4U)*1024*1024;
    p->nodes[0].expr.norder=2;
    p->nodes[0].expr.order[1]=(struct qs_order){.output=0};
    p->nodes[0].expr.outputs[1].type_oid=type;
    struct qp_node *leaf=&p->nodes[3]; leaf->input_offset=(qs_dict_bytes(512,65536)+4095)&~(size_t)4095;
    leaf->vector_offset=leaf->input_offset+QP_SCAN_BYTES;
    size_t bytes=leaf->vector_offset+qs_chunk_bytes(QP_SCAN_BYTES/24,2);
    unsigned char *arena=calloc(1,bytes),*io=calloc(1,QS_SORT_IO_BYTES); assert(arena && io);
    *(struct qs_dict *)arena=(struct qs_dict){.entries=512,.capacity=512,.data=65536};
    *(struct qs_chunk *)(arena+leaf->vector_offset)=(struct qs_chunk){.capacity=QP_SCAN_BYTES/24,.ncols=2};
    struct qp_control control={0}; struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    void *mr[]={p,&control,out,arena,io}; long long len[]={sizeof(*p),sizeof(control),sizeof(*out),bytes,QS_SORT_IO_BYTES};
    struct femu_csd_args args={.numr=5,.mr_addr=mr,.mr_len=len};
    void **tokens=NULL; unsigned ntokens=0,seen=0,last_id=0;
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
            if(hash) {
                assert(seen<groups); int cmp;
                assert(out->data[r].cells[1].isnull==want[seen].value.isnull);
                if(!want[seen].value.isnull) assert(!qs_compare(out->data[r].cells[1],want[seen].value,&cmp) && !cmp);
                if(want[seen].key==UINT32_MAX) assert(out->data[r].cells[0].isnull);
                else assert(!out->data[r].cells[0].isnull && out->data[r].cells[0].value==want[seen].key);
                ++seen; continue;
            }
            if(seen>=rows || out->data[r].cells[1].value!=expected[seen]) {
                fprintf(stderr,"nested sort row=%u got=%lld expected=%d\n",seen,(long long)out->data[r].cells[1].value,seen<rows?expected[seen]:0); abort();
            }
            unsigned id=(unsigned)out->data[r].cells[0].value;
            if(seen && expected[seen]==expected[seen-1]) assert(id>last_id);
            last_id=id; ++seen;
        }
        if(control.bytes && !control.ready) { memcpy(arena+leaf->input_offset,raw+control.offset,control.bytes); control.ready=1; }
    }
    assert(seen==groups);
    if(hash==1 || hash==5) assert(ntokens);
    if(hash==2 || hash==4) assert(!ntokens);
    for(unsigned i=0;i<ntokens;++i) { assert(!tokens[i]); }
    free(tokens); free(raw); free(expected); free(want); free(counts); free(p); free(arena); free(io); free(out);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
/* Two keys (text + nullable integer), expression SUM/AVG, adaptive growth
 * and partial-state spill. The oracle groups independently by array indices. */
struct expected_multi { unsigned a,b,rows,count; int64_t sum,minimum,maximum; };
/* Prehashed future rows must not raise errors past LIMIT, while an input
 * row actually requested must raise its payload error even if it cannot join. */
static void probe_payload(function run,unsigned limit,int strings,int indexed,int residual)
{
    struct qp_plan *p=calloc(1,sizeof(*p)); assert(p); p->count=4; p->scan_bytes=QP_SCAN_BYTES;
    scan(&p->nodes[2]); scan(&p->nodes[3]);
    struct qs_plan *left=&p->nodes[2].expr; left->nexprs=8;
    left->exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER};
    left->exprs[3]=(struct qs_expr){.op=Q_EQ,.type=Q_BOOLEAN,.left=0,.right=2};
    left->exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=INT64_MAX};
    left->exprs[5]=(struct qs_expr){.op=Q_MUL,.type=Q_INTEGER,.left=4,.right=4};
    left->exprs[6]=(struct qs_expr){.op=Q_MUL,.type=Q_INTEGER,.left=5,.right=4};
    left->exprs[7]=(struct qs_expr){.op=Q_CASE,.type=Q_DECIMAL,.left=3,.right=5,.otherwise=6};
    left->outputs[1]=(struct qs_output){.index=7,.type_oid=1700};
    if(strings) {
        left->nexprs=2; left->cols[1].length=-1; left->cols[1].type=Q_STRING;
        left->exprs[1].type=Q_STRING; left->outputs[1]=(struct qs_output){.index=1,.type_oid=25};
    }
    struct qp_node *j=&p->nodes[1]; j->kind=QP_JOIN; j->left=2; j->right=3; j->hash=1;
    j->nkeys=1; j->join_type=QP_INNER; j->join_qual=-1;
    j->expr=p->nodes[3].expr; j->expr.nattrs=4;
    j->expr.exprs[1].type=strings?Q_STRING:Q_DECIMAL; j->expr.outputs[1].type_oid=strings?25:1700;
    struct qp_node *top=&p->nodes[0]; top->kind=QP_LIMIT; top->left=1; top->right=-1;
    top->expr=j->expr; top->expr.limit=limit; top->expr.work_mem_bytes=64U*1024*1024;
    if(strings>=2) {
        /* Joined text expressions share column vectors and literal IDs. The
         * second row has a negative substring length: CASE can suppress it,
         * and LIMIT must not observe it before requesting that row. */
        struct qs_plan *e=&j->expr; e->nexprs=12; e->nstrings=2;
        e->strings[0]=(struct qs_string){.length=4,.bytes="skip"};
        e->strings[1]=(struct qs_string){.length=strings==2?2:1,.bytes="k%"};
        if(strings==3) e->strings[1].bytes[0]='%';
        e->exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_STRING,.constant=0};
        e->exprs[3]=(struct qs_expr){.op=Q_LT,.type=Q_BOOLEAN,.left=1,.right=2};
        e->exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=2};
        e->exprs[5]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=-1};
        e->exprs[6]=(struct qs_expr){.op=Q_CASE,.type=Q_INTEGER,.left=3,.right=4,.otherwise=5};
        e->exprs[7]=(struct qs_expr){.op=Q_SUBSTRING,.type=Q_STRING,.left=1,.right=4,.otherwise=6};
        e->exprs[8]=(struct qs_expr){.op=Q_CONST,.type=Q_STRING,.constant=1};
        e->exprs[9]=(struct qs_expr){.op=Q_LIKE,.type=Q_BOOLEAN,.left=1,.right=8};
        e->exprs[10]=(struct qs_expr){.op=Q_CONST,.type=Q_STRING,.isnull=1};
        e->exprs[11]=(struct qs_expr){.op=Q_CASE,.type=Q_STRING,.left=9,.right=7,.otherwise=10};
        e->outputs[1].index=11;
    }
    if(indexed) {
        /* The right projection fails only after the first valid index match.
         * LIMIT must consume that prefix before observing the later error. */
        assert(!strings); p->nodes[3].expr=*left;
        p->nodes[3].expr.exprs[2].constant=1;
        if(indexed==2) p->nodes[3].expr.predicate=3; /* rejected row's output overflows */
        if(indexed==3) {
            struct qs_plan *right=&p->nodes[3].expr;
            right->cols[1].length=-1; right->cols[1].type=right->exprs[1].type=Q_STRING;
            right->exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=2};
            right->exprs[5]=(struct qs_expr){.op=Q_SUBSTRING,.type=Q_STRING,.left=1,.right=4,.otherwise=4};
            right->exprs[6]=(struct qs_expr){.op=Q_CONST,.type=Q_STRING,.isnull=1};
            right->exprs[7]=(struct qs_expr){.op=Q_CASE,.type=Q_STRING,.left=3,.right=5,.otherwise=6};
            right->outputs[1].type_oid=j->expr.outputs[1].type_oid=top->expr.outputs[1].type_oid=25;
            j->expr.exprs[1].type=top->expr.exprs[1].type=Q_STRING;
        }
        left->nexprs=2; left->outputs[1]=(struct qs_output){.index=1,.type_oid=23};
        j->kind=QP_INDEX_JOIN; j->hash=0; j->expr.exprs[1].column=3;
        p->nodes[3].expr.join=(struct qs_join){.indexid=1,.indexroot=1,.index_key_bytes=4,.build_key=0};
        if(indexed==2 || indexed==3) {
            /* Keep exact wide numeric/text outputs while the residual only
             * reads a left column. Rejected right rows must remain inactive. */
            j->expr.nexprs=4;
            j->expr.exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=1};
            j->expr.exprs[3]=(struct qs_expr){.op=Q_GE,.type=Q_BOOLEAN,.left=0,.right=2};
            j->join_qual=3;
        }
        if(indexed>=4) {
            /* Only a duplicate of an already accepted key overflows. SEMI
             * and ANTI must stop that key before evaluating its next match. */
            struct qs_plan *right=&p->nodes[3].expr;
            right->exprs[2].constant=-486; right->exprs[3].op=Q_NE; right->exprs[3].left=1;
            j->join_type=indexed==4?QP_SEMI:QP_ANTI;
            j->expr.exprs[1].column=1;
        }
    }
    if(residual) {
        assert(!strings);
        if(indexed) scan(&p->nodes[3]); /* Keep a plain indexed payload; test errors in Join Filter itself. */
        left->nexprs=2; left->outputs[1]=(struct qs_output){.index=1,.type_oid=23};
        struct qs_plan *e=&j->expr; e->nexprs=10;
        e->exprs[1].type=Q_INTEGER; e->exprs[1].column=1;
        e->exprs[2]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=3};
        e->exprs[3]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=indexed?-498:-474};
        e->exprs[4]=(struct qs_expr){.op=Q_EQ,.type=Q_BOOLEAN,.left=2,.right=3};
        e->exprs[5]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=INT64_MAX};
        e->exprs[6]=(struct qs_expr){.op=Q_MUL,.type=Q_DECIMAL,.left=5,.right=5};
        e->exprs[7]=(struct qs_expr){.op=Q_MUL,.type=Q_DECIMAL,.left=6,.right=5};
        e->exprs[8]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=1};
        e->exprs[9]=(struct qs_expr){.op=Q_CASE,.type=Q_DECIMAL,.left=4,.right=8,.otherwise=7};
        j->join_qual=9; j->join_type=residual==2 || residual==4?QP_SEMI:residual==3 || residual==5?QP_ANTI:QP_INNER;
        if(residual>=4) {
            /* Reject the first candidate, accept the next vector candidate,
             * then suppress an error in the remaining candidate. */
            e->nexprs=15;
            e->exprs[10]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=-486};
            e->exprs[11]=(struct qs_expr){.op=Q_EQ,.type=Q_BOOLEAN,.left=2,.right=10};
            e->exprs[12]=(struct qs_expr){.op=Q_CASE,.type=Q_DECIMAL,.left=11,.right=8,.otherwise=7};
            e->exprs[13]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER};
            e->exprs[14]=(struct qs_expr){.op=Q_CASE,.type=Q_DECIMAL,.left=4,.right=13,.otherwise=12};
            j->join_qual=14;
        }
    }
    size_t sizes[2],bytes=(qs_dict_bytes(512,65536)+4095)&~(size_t)4095;
    unsigned char *raw[2]={heap(2,2,&sizes[0]),heap(indexed || residual?3:1,2,&sizes[1])};
    if(residual) for(unsigned r=0;r<3;++r) w32(raw[1]+QS_PAGE-(r+1)*40+24,0);
    unsigned char *index=indexed?index_page(indexed>=4 || residual?4:3):NULL;
    if(indexed) for(unsigned r=0;r<2;++r) w32(raw[0]+QS_PAGE-(r+1)*40+24,r+1);
    if(indexed && residual) {
        for(unsigned r=0;r<2;++r) w32(raw[0]+QS_PAGE-(r+1)*40+24,r);
        for(unsigned r=0;r<3;++r) {
            unsigned char *leaf=index+QS_PAGE; uint32_t lp; memcpy(&lp,leaf+24+r*4,4);
            unsigned char *entry=leaf+(lp&32767); w32(entry+8,0); w16(entry+4,r+1);
        }
    }
    if(indexed>=4) for(unsigned r=0;r<3;++r) {
        unsigned char *leaf=index+QS_PAGE; uint32_t lp; memcpy(&lp,leaf+24+r*4,4);
        unsigned char *entry=leaf+(lp&32767); w32(entry+8,r==2?2:1);
        w16(entry+4,r+1); w32(raw[1]+QS_PAGE-(r+1)*40+24,r==2?2:1);
    }
    if(indexed==3) for(unsigned r=0;r<3;++r) {
        unsigned char *tuple=raw[1]+QS_PAGE-(r+1)*40;
        tuple[28]=(5U<<1)|1U; memcpy(tuple+29,r==1?"keep":"skip",4);
    }
    if(strings) for(unsigned r=0;r<2;++r) {
        unsigned char *tuple=raw[0]+QS_PAGE-(r+1)*40;
        if(strings>=2) w32(tuple+24,0);
        tuple[28]=(5U<<1)|1U; memcpy(tuple+29,r?"skip":"keep",4);
    }
    for(unsigned i=2;i<4;++i) {
        struct qp_node *n=&p->nodes[i]; n->bytes=sizes[i-2]; n->input_offset=bytes; bytes+=QP_SCAN_BYTES;
        n->vector_offset=bytes; bytes+=qs_chunk_bytes(QP_SCAN_BYTES/24,2);
        bytes=(bytes+7)&~(size_t)7;
    }
    if(indexed) {
        j->input_offset=bytes; bytes+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE;
        j->vector_offset=bytes; bytes+=4096+qs_chunk_bytes(256,4);
    }
    unsigned char *arena=calloc(1,bytes),*io=calloc(1,QS_SORT_IO_BYTES); assert(arena && io);
    *(struct qs_dict *)arena=(struct qs_dict){.entries=512,.capacity=512,.data=65536};
    for(unsigned i=2;i<4;++i)
        *(struct qs_chunk *)(arena+p->nodes[i].vector_offset)=(struct qs_chunk){.capacity=QP_SCAN_BYTES/24,.ncols=2};
    struct qp_control control={0}; struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    void *mr[]={p,&control,out,arena,io}; long long len[]={sizeof(*p),sizeof(control),sizeof(*out),bytes,QS_SORT_IO_BYTES};
    struct femu_csd_args args={.numr=5,.mr_addr=mr,.mr_len=len}; int64_t status;
    do {
        status=run(&args); if(status<0) break;
        assert(!status);
        if(control.index_nreads && !control.ready) {
            for(unsigned i=0;i<control.index_nreads;++i) {
                const struct qs_index_read *r=&control.index_reads[i];
                memcpy(arena+j->input_offset+QS_SORT_IO_BYTES+(size_t)r->slot*QS_PAGE,
                    (r->heap?raw[1]:index)+(size_t)r->block*QS_PAGE,QS_PAGE);
            }
            control.ready=1;
        } else if(control.bytes && !control.ready) {
            struct qp_node *n=&p->nodes[control.node];
            memcpy(arena+n->input_offset,raw[control.node-2]+control.offset,control.bytes); control.ready=1;
        }
    } while(!out->finished);
    if(residual) {
        if(residual==1 && limit>1) { assert(status==-5); control.close=1; assert(!run(&args)); }
        else {
            assert(!status && out->rows==1);
            assert(out->data[0].cells[0].value==(residual==3 || residual==5?1:0));
            assert(out->data[0].cells[1].value==(residual==3 || residual==5?-486:-498));
        }
    }
    else if(indexed>=4) {
        assert(!status && out->finished && out->rows==(indexed==4?2:0));
        if(indexed==4) {
            assert(out->data[0].cells[0].value==1 && out->data[1].cells[0].value==2);
            assert(out->data[0].cells[1].value==-498 && out->data[1].cells[1].value==-486);
        }
    }
    else if(indexed==3) {
        if(status || out->rows!=2 || qs_cell_string(&out->data[0].cells[1])!=2)
            fprintf(stderr,"indexed text status=%lld rows=%u null=%u length=%u value=%lld\n",(long long)status,out->rows,
                out->data[0].cells[1].isnull,qs_cell_string(&out->data[0].cells[1]),(long long)out->data[0].cells[1].value);
        assert(!status && out->rows==2 && qs_cell_string(&out->data[0].cells[1])==2);
        assert(!memcmp(qs_cell_string_bytes(&out->data[0].cells[1]),"ee",2));
        assert(out->data[1].cells[1].isnull);
    }
    else if(strings==3 && limit>1) { assert(status==-5); control.close=1; assert(!run(&args)); }
    else if(strings>=2) {
        assert(!status && out->rows==(limit==1?1:2));
        assert(qs_cell_string(&out->data[0].cells[1])==2);
        assert(!memcmp(qs_cell_string_bytes(&out->data[0].cells[1]),"ee",2));
        if(out->rows==2) assert(out->data[1].cells[1].isnull);
    } else if(strings) {
        assert(!status && out->rows==1 && qs_cell_string(&out->data[0].cells[1])==4);
        assert(!memcmp(qs_cell_string_bytes(&out->data[0].cells[1]),"keep",4));
        assert(out->metrics.dictionary_entries==1); /* unmatched "skip" was never interned */
    } else if(limit==1 || indexed==2) {
        assert(!status && out->rows==1 && out->data[0].cells[0].value==(indexed?1:0));
        assert(out->data[0].cells[1].value==(__int128)INT64_MAX*INT64_MAX);
    }
    else { assert(status==-5); control.close=1; assert(!run(&args)); }
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
    free(raw[0]); free(raw[1]); free(index); free(arena); free(io); free(out); free(p);
}
/* InitPlan parameters remain batch constants with their runtime precision,
 * both over a Heap scan and over retained Hash Join payload columns. */
static void parameter_projection(function run,int joined,int wide,int null_parameter)
{
    unsigned rows=20003,leaf=joined?3:1;
    struct qp_plan *p=calloc(1,sizeof(*p)); assert(p);
    p->count=joined?5:3; p->scan_bytes=QP_SCAN_BYTES; p->nparams=p->ninit=1;
    p->init_nodes[0]=2; p->init_params[0]=0;
    scan(&p->nodes[leaf]);
    if(joined) {
        scan(&p->nodes[4]);
        struct qp_node *join=&p->nodes[1];
        *join=(struct qp_node){.kind=QP_JOIN,.left=3,.right=4,.hash=1,.join_type=QP_INNER,
            .join_qual=-1,.nkeys=1,.expr=p->nodes[3].expr};
        join->expr.nattrs=4;
    }
    struct qp_node *root=&p->nodes[0];
    *root=(struct qp_node){.kind=QP_PROJECT,.left=1,.right=-1,.expr=p->nodes[leaf].expr};
    root->expr.work_mem_bytes=64U*1024*1024; root->expr.nexprs=4;
    root->expr.exprs[2]=(struct qs_expr){.op=QP_PARAMETER,.type=Q_DECIMAL,.column=0};
    root->expr.exprs[3]=(struct qs_expr){.op=Q_ADD,.type=Q_DECIMAL,.left=1,.right=2};
    root->expr.outputs[1]=(struct qs_output){.index=3,.type_oid=1700,.typmod=-1};
    struct qp_node *init=&p->nodes[2];
    *init=(struct qp_node){.kind=QP_PROJECT,.left=-1,.right=-1};
    init->expr.predicate=init->expr.having=-1; init->expr.nexprs=wide?2:1; init->expr.nout=1;
    init->expr.exprs[0]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL,.constant=wide?INT64_MAX:12345,
        .scale=2,.dscale=3,.isnull=null_parameter};
    init->expr.exprs[1]=(struct qs_expr){.op=Q_MUL,.type=Q_DECIMAL,.left=0,.right=0};
    init->expr.outputs[0]=(struct qs_output){.index=wide?1:0,.type_oid=1700};
    size_t sizes[2],bytes=(qs_dict_bytes(512,65536)+4095)&~(size_t)4095;
    unsigned char *raw[]={heap(rows,2,&sizes[0]),NULL};
    if(joined) {
        raw[1]=heap(1,2,&sizes[1]);
        for(unsigned r=0;r<rows;++r) w32(raw[0]+(r/100)*QS_PAGE+QS_PAGE-(r%100+1)*40+24,0);
    }
    for(unsigned i=0;i<(joined?2U:1U);++i) {
        struct qp_node *n=&p->nodes[i?4:leaf]; n->bytes=sizes[i]; n->estimated_rows=i?1:rows;
        n->input_offset=bytes; bytes+=QP_SCAN_BYTES; n->vector_offset=bytes;
        bytes=(bytes+qs_chunk_bytes(QP_VECTOR_ROWS,2)+4095)&~(size_t)4095;
    }
    unsigned char *arena=calloc(1,bytes),*io=calloc(1,QS_SORT_IO_BYTES); assert(arena && io);
    *(struct qs_dict *)arena=(struct qs_dict){.entries=512,.capacity=512,.data=65536};
    for(unsigned i=0;i<(joined?2U:1U);++i) {
        struct qp_node *n=&p->nodes[i?4:leaf];
        *(struct qs_chunk *)(arena+n->vector_offset)=(struct qs_chunk){.capacity=qp_scan_capacity(p,n),.ncols=2};
    }
    struct qp_control control={0}; struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    void *mr[]={p,&control,out,arena,io};
    long long lengths[]={sizeof(*p),sizeof(control),sizeof(*out),bytes,QS_SORT_IO_BYTES};
    struct femu_csd_args args={.numr=5,.mr_addr=mr,.mr_len=lengths};
    __int128 parameter=wide?(__int128)INT64_MAX*INT64_MAX:12345; unsigned seen=0;
    while(!out->finished) {
        assert(!run(&args));
        for(unsigned r=0;r<out->rows;++r) {
            struct qs_cell *v=out->data[r].cells;
            assert(seen<rows && v[0].value==(joined?0:seen) && v[1].isnull==null_parameter);
            if(!null_parameter) {
                int value=(int)((uint64_t)seen*1009%997)-498;
                assert(v[1].value==parameter+(__int128)value*(wide?10000:100));
                assert(v[1].scale==(wide?4:2) && v[1].dscale==(wide?6:3));
            }
            ++seen;
        }
        if(control.bytes && !control.ready) {
            unsigned source=control.node==leaf?0:1; assert(!source || joined);
            memcpy(arena+p->nodes[control.node].input_offset,raw[source]+control.offset,control.bytes); control.ready=1;
        }
    }
    assert(seen==rows);
    free(raw[0]); free(raw[1]); free(arena); free(io); free(out); free(p);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
/* A grouped probe must join its final SUM, not expose the aggregate's
 * internal input vectors as ordinary rows. */
static void grouped_probe(function run)
{
    struct qp_plan *p=calloc(1,sizeof(*p)); assert(p); p->count=5; p->scan_bytes=QP_SCAN_BYTES;
    scan(&p->nodes[3]); scan(&p->nodes[4]);
    struct qp_node *group=&p->nodes[2]; group->kind=QP_AGGREGATE; group->hash=1; group->left=3; group->right=-1;
    group->nkeys=group->expr.nkeys=1; group->expr=p->nodes[3].expr;
    group->expr.nkeys=1; group->expr.naggs=1; group->expr.nexprs=3;
    group->expr.exprs[2]=(struct qs_expr){.op=Q_AGGREF,.column=0,.type=Q_INTEGER};
    group->expr.aggs[0]=(struct qs_agg){.kind=Q_SUM,.expr=1}; group->expr.outputs[1].index=2;
    struct qp_node *join=&p->nodes[1]; join->kind=QP_JOIN; join->hash=1; join->left=2; join->right=4;
    join->join_type=QP_INNER; join->join_qual=-1; join->nkeys=1; join->expr=p->nodes[3].expr; join->expr.nattrs=4;
    p->nodes[0]=(struct qp_node){.kind=QP_LIMIT,.left=1,.right=-1,.expr=join->expr};
    p->nodes[0].expr.limit=5; p->nodes[0].expr.work_mem_bytes=64U*1024*1024;
    size_t sizes[2],bytes=(qs_dict_bytes(512,65536)+4095)&~(size_t)4095;
    unsigned char *raw[]={heap(3,2,&sizes[0]),heap(1,2,&sizes[1])};
    for(unsigned r=0;r<3;++r) {
        unsigned char *tuple=raw[0]+QS_PAGE-(r+1)*40; w32(tuple+24,r/2); w32(tuple+28,(r+1)*10);
    }
    for(unsigned i=3;i<5;++i) {
        struct qp_node *n=&p->nodes[i]; n->bytes=sizes[i-3]; n->input_offset=bytes; bytes+=QP_SCAN_BYTES;
        n->vector_offset=bytes; bytes+=qs_chunk_bytes(QP_SCAN_BYTES/24,2);
        bytes=(bytes+7)&~(size_t)7;
    }
    unsigned char *arena=calloc(1,bytes),*io=calloc(1,QS_SORT_IO_BYTES); assert(arena && io);
    *(struct qs_dict *)arena=(struct qs_dict){.entries=512,.capacity=512,.data=65536};
    for(unsigned i=3;i<5;++i)
        *(struct qs_chunk *)(arena+p->nodes[i].vector_offset)=(struct qs_chunk){.capacity=QP_SCAN_BYTES/24,.ncols=2};
    struct qp_control control={0}; struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    void *mr[]={p,&control,out,arena,io}; long long len[]={sizeof(*p),sizeof(control),sizeof(*out),bytes,QS_SORT_IO_BYTES};
    struct femu_csd_args args={.numr=5,.mr_addr=mr,.mr_len=len};
    do {
        assert(!run(&args));
        if(control.bytes && !control.ready) {
            struct qp_node *n=&p->nodes[control.node];
            memcpy(arena+n->input_offset,raw[control.node-3]+control.offset,control.bytes); control.ready=1;
        }
    } while(!out->finished);
    assert(out->rows==1 && out->data[0].cells[0].value==0 && out->data[0].cells[1].value==30);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
    free(raw[0]); free(raw[1]); free(arena); free(io); free(out); free(p);
}
static int multi_order(const void *ap,const void *bp)
{
    const struct expected_multi *a=ap,*b=bp;
    if(a->a!=b->a) return (a->a>b->a)-(a->a<b->a);
    if(a->b==17 || b->b==17) return (a->b==17)-(b->b==17);
    return (a->b<b->b)-(a->b>b->b);
}
enum group_test_mode { GROUP_SINGLE,GROUP_COUNTS,GROUP_NUMERIC_EXTREMES,GROUP_TEXT_EXTREMES };
static void multiple_group_aggregates(function run,unsigned memory,unsigned kind,int integer_keys,unsigned workers,enum group_test_mode mode)
{
    int counts=mode!=GROUP_SINGLE,extremes=mode>=GROUP_NUMERIC_EXTREMES,text=mode==GROUP_TEXT_EXTREMES;
    unsigned columns=extremes?3:2;
    unsigned scan_bytes=workers>4?QS_PAGE:QP_SCAN_BYTES;
    unsigned rows=50003,total=1022*18,groups=0; size_t raw_bytes;
    unsigned char *raw=heap(rows,2,&raw_bytes);
    struct expected_multi *expected=calloc(total,sizeof(*expected)); assert(expected);
    struct qp_plan *p=calloc(1,sizeof(*p)); assert(p); p->count=3; p->scan_bytes=scan_bytes;
    struct qp_node *leaf=&p->nodes[2]; scan(leaf); leaf->bytes=raw_bytes;
    if(extremes) {
        leaf->expr.nattrs=leaf->expr.ncols=leaf->expr.nexprs=leaf->expr.nout=3;
        leaf->expr.cols[2]=(struct qs_column){.length=text?-1:4,.align=text?1:4,.slot=2,.type=text?Q_STRING:Q_INTEGER};
        leaf->expr.exprs[2]=(struct qs_expr){.op=Q_COLUMN,.column=2,.type=text?Q_STRING:Q_INTEGER};
        leaf->expr.outputs[2]=(struct qs_output){.index=2,.type_oid=text?25:23};
    }
    leaf->expr.exprs[0].type=Q_STRING; leaf->expr.outputs[0].type_oid=25;
    leaf->input_offset=(qs_dict_bytes(2048,65536)+4095)&~(size_t)4095;
    leaf->vector_offset=leaf->input_offset+scan_bytes;
    size_t bytes=leaf->vector_offset+qs_chunk_bytes(scan_bytes/24,columns);
    unsigned char *arena=calloc(1,bytes),*io=calloc(1,QS_SORT_IO_BYTES); assert(arena && io);
    struct qs_dict *dict=(void *)arena; *dict=(struct qs_dict){.entries=2048,.capacity=2048,.data=65536};
    int ids[1021];
    for(unsigned i=0;i<1021;++i) {
        /* Reverse ID order for text extrema, so comparing IDs cannot pass. */
        unsigned key=text?1020-i:i; char value[8]; int n=snprintf(value,sizeof(value),"k%04u",key);
        ids[key]=qs_dict_insert(dict,value,n); assert(ids[key]>=0);
    }
    for(unsigned i=0;i<rows;++i) {
        /* Cluster integer inputs; retain shuffled string keys in other cases. */
        unsigned position=integer_keys?i/4:i;
        unsigned a=position%17?position%1021:1021,b=position%19?(position/1021)%17:17;
        struct expected_multi *w=&expected[a*18+b]; w->a=a; w->b=b; ++w->rows;
        int64_t value=text?(int64_t)((i/1021)%1021):extremes?((int)(i%29)-14)*125:((int)b-8)*125;
        if(b!=17) {
            if(!w->count || value<w->minimum) w->minimum=value;
            if(!w->count || value>w->maximum) w->maximum=value;
            w->sum+=value; ++w->count;
        }
        unsigned char *t=raw+(i/100)*QS_PAGE+QS_PAGE-(i%100+1)*40;
        t[23]=(a!=1021?1:0)|(b!=17?2:0); unsigned off=24;
        if(a!=1021) { w32(t+off,ids[a]); off+=4; }
        if(b!=17) w32(t+off,(int)b-8);
        if(extremes) {
            w16(t+18,0x8003);
            if(b!=17) {
                t[23]|=4; off+=4;
                if(text) {
                    char bytes[8]; int n=snprintf(bytes,sizeof(bytes),"k%04u",(unsigned)value);
                    t[off]=((n+1)<<1)|1; memcpy(t+off+1,bytes,n);
                } else w32(t+off,(int)(i%29)-14);
            }
        }
    }
    for(unsigned i=0;i<total;++i) if(expected[i].rows) expected[groups++]=expected[i];
    qsort(expected,groups,sizeof(*expected),multi_order);
    struct qp_node *a=&p->nodes[1]; a->kind=QP_AGGREGATE; a->left=2; a->right=-1; a->hash=1;
    /* Force full-table handoff with a partially consumed expression vector.
     * The owner must grow/spill without losing or counting those rows twice. */
    a->estimated_rows=a->estimated_groups=128;
    a->nkeys=a->expr.nkeys=2; a->left_keys[0]=a->expr.keys[0]=0; a->left_keys[1]=a->expr.keys[1]=1;
    a->expr.predicate=-1; a->expr.nattrs=2; a->expr.nexprs=5; a->expr.nout=3; a->expr.naggs=1;
    a->expr.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=0,.type=Q_STRING};
    a->expr.exprs[1]=(struct qs_expr){.op=Q_COLUMN,.column=1,.type=Q_INTEGER};
    a->expr.exprs[2]=(struct qs_expr){.op=Q_AGGREF,.column=0,.type=Q_DECIMAL};
    a->expr.exprs[3]=(struct qs_expr){.op=Q_CONST,.type=Q_DECIMAL,.constant=125,.scale=2,.dscale=2};
    /* Generic PG scale metadata must be derived from imported children. */
    a->expr.exprs[4]=(struct qs_expr){.op=Q_MUL,.type=Q_DECIMAL,.left=1,.right=3,.scale=0,.dscale=2};
    a->expr.aggs[0]=(struct qs_agg){.kind=kind,.expr=4};
    a->expr.outputs[0]=(struct qs_output){.index=0,.type_oid=25};
    a->expr.outputs[1]=(struct qs_output){.index=1,.type_oid=23};
    a->expr.outputs[2]=(struct qs_output){.index=2,.type_oid=1700};
    if(integer_keys) {
        leaf->expr.exprs[0].type=a->expr.exprs[0].type=Q_INTEGER;
        leaf->expr.outputs[0].type_oid=a->expr.outputs[0].type_oid=23;
    }
    if(kind==Q_COUNT) {
        /* DISTINCT stage emits each nullable pair once; the outer COUNT
         * must ignore a NULL argument even when its group is retained. */
        p->count=4; p->nodes[3]=*a;
        p->nodes[3].expr.nout=p->nodes[3].expr.nexprs=2;
        p->nodes[3].expr.aggs[0]=(struct qs_agg){.kind=Q_COUNT_STAR,.expr=-1};
        a->left=3; a->expr.aggs[0].expr=1;
        a->expr.outputs[2].type_oid=20; a->expr.exprs[2].type=Q_INTEGER;
    }
    if(counts) {
        assert(kind==Q_SUM || kind==Q_AVG || kind==Q_MIN || kind==Q_MAX);
        /* One key lookup updates the main aggregate, COUNT and COUNT(*).
         * Their independent counts must survive both growth and spill. */
        a->expr.naggs=3; a->expr.nout=5; a->expr.nexprs=7;
        a->expr.aggs[1]=(struct qs_agg){.kind=Q_COUNT,.expr=4};
        a->expr.aggs[2]=(struct qs_agg){.kind=Q_COUNT_STAR,.expr=-1};
        for(unsigned i=0;i<2;++i) {
            a->expr.exprs[5+i]=(struct qs_expr){.op=Q_AGGREF,.type=Q_INTEGER,.column=1+i};
            a->expr.outputs[3+i]=(struct qs_output){.index=5+(int)i,.type_oid=20};
        }
    }
    if(extremes) {
        a->expr.nattrs=3; a->expr.naggs=5; a->expr.nout=7; a->expr.nexprs=10;
        a->expr.exprs[2]=(struct qs_expr){.op=Q_COLUMN,.column=2,.type=text?Q_STRING:Q_INTEGER};
        if(text) a->expr.exprs[4]=a->expr.exprs[2];
        else a->expr.exprs[4].left=2;
        a->expr.exprs[7]=(struct qs_expr){.op=Q_AGGREF,.column=0,.type=text?Q_STRING:Q_DECIMAL};
        a->expr.outputs[2]=(struct qs_output){.index=7,.type_oid=text?25:1700};
        for(unsigned i=0;i<2;++i) {
            a->expr.aggs[3+i]=(struct qs_agg){.kind=i?Q_MAX:Q_MIN,.expr=4};
            a->expr.exprs[8+i]=(struct qs_expr){.op=Q_AGGREF,.column=3+i,.type=text?Q_STRING:Q_DECIMAL};
            a->expr.outputs[5+i]=(struct qs_output){.index=8+(int)i,.type_oid=text?25:1700};
        }
    }
    struct qp_node *sort=&p->nodes[0]; sort->kind=QP_SORT; sort->left=1; sort->right=-1;
    sort->expr=a->expr; sort->expr.nexprs=sort->expr.nattrs=a->expr.nout; sort->expr.naggs=0;
    sort->expr.work_mem_bytes=(uint64_t)memory*1024*1024; sort->expr.limit=-1;
    for(unsigned i=0;i<sort->expr.nout;++i) {
        sort->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.column=i,.type=a->expr.exprs[a->expr.outputs[i].index].type};
        sort->expr.outputs[i].index=i;
    }
    sort->expr.norder=2; sort->expr.order[0]=(struct qs_order){.output=0}; sort->expr.order[1]=(struct qs_order){.output=1,.descending=1};
    p->workers=workers;
    if(workers) {
        size_t old=bytes,base=(bytes+4095)&~(size_t)4095;
        bytes=base;
        leaf->parallel_input=bytes; bytes+=scan_bytes;
        leaf->parallel_vector=bytes; bytes+=(qs_chunk_bytes(scan_bytes/24,columns)+4095)&~(size_t)4095;
        p->parallel_stride=bytes-base; bytes=base+workers*p->parallel_stride;
        arena=realloc(arena,bytes); assert(arena); memset(arena+old,0,bytes-old);
        io=realloc(io,(workers+1)*QS_SORT_IO_BYTES); assert(io);
        leaf->parallel=1;
    }
    *(struct qs_chunk *)(arena+leaf->vector_offset)=(struct qs_chunk){.capacity=qp_scan_capacity(p,leaf),.ncols=columns};
    struct qp_control *controls=calloc(workers+1,sizeof(*controls));
    struct qs_result *out=calloc(workers+1,sizeof(*out)); assert(controls && out);
#define control controls[0]
    void *mr[]={p,&control,out,arena,io}; long long len[]={sizeof(*p),(workers+1)*sizeof(*controls),(workers+1)*sizeof(*out),bytes,(workers+1)*QS_SORT_IO_BYTES};
    struct femu_csd_args args={.numr=5,.mr_addr=mr,.mr_len=len,.cparam1=workers?1:0};
    uint64_t available=0,done=0; unsigned epoch=0;
    atomic_uint_fast64_t host_offset=0;
    bool need_input[QS_WORKERS]={0};
    void **tokens=NULL; unsigned ntokens=0,seen=0;
    while(!out->finished) {
        if(control.parallel && !csIsReentry(&args)) {
            pthread_t threads[QS_WORKERS]; struct probe_test tasks[QS_WORKERS]; uint64_t started=0;
            for(unsigned w=0;w<p->workers;++w) if(!((available|done)&(UINT64_C(1)<<w))) {
                tasks[w]=(struct probe_test){run,args,p,&controls[w+1],&out[w+1],arena,raw,raw,NULL,w+1,tokens,ntokens,NULL,NULL,0};
                tasks[w].args.cparam1=((w+1)<<1)|1;
                if(test_scheduled && control.scan_node) {
                    tasks[w].next_offset=&host_offset; tasks[w].need_input=&need_input[w];
                    tasks[w].scan_node=control.scan_node;
                }
                assert(!pthread_create(&threads[w],NULL,probe_worker,&tasks[w])); started|=UINT64_C(1)<<w;
            }
            for(unsigned w=0;w<p->workers;++w) if(started&(UINT64_C(1)<<w)) {
                assert(!pthread_join(threads[w],NULL)); available|=UINT64_C(1)<<w;
                if(out[w+1].finished) done|=UINT64_C(1)<<w;
            }
            control.available=available;
        }
        int64_t status=run(&args); assert(status>=0);
        uint64_t op=(uint64_t)status>>60,id=(uint64_t)status&CS_IO_VALUE_MASK;
        if(op) {
            if(op==1) { tokens=realloc(tokens,(ntokens+1)*sizeof(*tokens)); assert(tokens); tokens[ntokens]=malloc(id); assert(tokens[ntokens]); memcpy(tokens[ntokens],io,id); args.cparam2=++ntokens; }
            else if(op==2) { assert(id && id<=ntokens && tokens[id-1]); memcpy(io,tokens[id-1],QS_SORT_IO_BYTES); args.cparam2=id; }
            else { assert(op==3 && id && id<=ntokens && tokens[id-1]); free(tokens[id-1]); tokens[id-1]=NULL; args.cparam2=id; }
            continue;
        }
        args.cparam2=0; available&=~control.drained;
        if(control.epoch!=epoch) {
            epoch=control.epoch; available=done=0;
            atomic_store(&host_offset,0);
            for(unsigned w=0;w<p->workers;++w) need_input[w]=true;
        }
        for(unsigned r=0;r<out->rows;++r) {
            assert(seen<groups); const struct expected_multi *w=&expected[seen++]; struct qs_cell *v=out->data[r].cells;
            assert(v[0].isnull==(w->a==1021) && v[1].isnull==(w->b==17));
            if(w->a!=1021 && integer_keys) assert(v[0].value==ids[w->a]);
            else if(w->a!=1021) { char text[8]; int n=snprintf(text,sizeof(text),"k%04u",w->a); assert(qs_cell_string(&v[0])==(unsigned)n && !memcmp(qs_cell_string_bytes(&v[0]),text,n)); }
            if(w->b!=17) assert(v[1].value==(int)w->b-8);
            if(kind==Q_COUNT) { assert(!v[2].isnull && v[2].value==!!w->count); continue; }
            assert(v[2].isnull==!w->count);
            if(counts) {
                assert(!v[3].isnull && v[3].value==w->count);
                assert(!v[4].isnull && v[4].value==w->rows);
            }
            if(w->count) {
                int64_t value=kind==Q_MIN?w->minimum:kind==Q_MAX?w->maximum:w->sum;
                struct qs_cell sum={.value=value,.scale=2,.dscale=2}; int cmp;
                if(kind==Q_AVG) assert(!qs_average(sum,w->count,&sum));
                if(text) {
                    char want[8]; int n=snprintf(want,sizeof(want),"k%04u",(unsigned)value);
                    assert(qs_cell_string(&v[2])==(unsigned)n && !memcmp(qs_cell_string_bytes(&v[2]),want,n));
                } else assert(!qs_compare(v[2],sum,&cmp) && !cmp);
            }
            if(extremes) for(unsigned a=0;a<2;++a) {
                assert(v[5+a].isnull==!w->count);
                if(!w->count) continue;
                int64_t value=a?w->maximum:w->minimum;
                if(text) {
                    char want[8]; int n=snprintf(want,sizeof(want),"k%04u",(unsigned)value);
                    assert(qs_cell_string(&v[5+a])==(unsigned)n && !memcmp(qs_cell_string_bytes(&v[5+a]),want,n));
                } else {
                    struct qs_cell want={.value=value,.scale=2,.dscale=2}; int cmp;
                    assert(!qs_compare(v[5+a],want,&cmp) && !cmp);
                }
            }
        }
        if(control.bytes && !control.ready) { memcpy(arena+leaf->input_offset,raw+control.offset,control.bytes); control.ready=1; }
    }
    if(workers && memory>=64) {
        assert(out->metrics.workers==workers);
        assert(p->pipelines[2].sink_rows && p->pipelines[2].sink_ns);
        assert(p->pipelines[2].sink_ns<=p->pipelines[2].worker_ns);
    }
    assert(seen==groups && (memory==4?ntokens>0:ntokens==0));
    if(counts && workers) assert(out->metrics.work_peak && out->metrics.work_peak<=(uint64_t)memory*1024*1024);
    for(unsigned i=0;i<ntokens;++i) assert(!tokens[i]);
    free(controls); free(tokens); free(raw); free(expected); free(p); free(arena); free(io); free(out);
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
#undef control
static void multiple_groups(function run,unsigned memory,unsigned kind,int integer_keys,unsigned workers)
{ multiple_group_aggregates(run,memory,kind,integer_keys,workers,GROUP_SINGLE); }
struct dictionary_test { struct qs_dict *dictionary; unsigned worker; };
static void *dictionary_worker(void *arg)
{
    struct dictionary_test *t=arg;
    for(unsigned i=0;i<5000;++i) {
        char text[64]; const char *value; unsigned length;
        unsigned n=snprintf(text,sizeof(text),"shared-%u",i%997);
        int id=qs_dict_insert(t->dictionary,text,n); assert(id>0);
        assert(!qs_dict_get(t->dictionary,id,&value,&length) && length==n && !memcmp(value,text,n));
        n=snprintf(text,sizeof(text),"unique-%u-%u",t->worker,i);
        id=qs_dict_insert(t->dictionary,text,n); assert(id>0);
        assert(!qs_dict_get(t->dictionary,id,&value,&length) && length==n && !memcmp(value,text,n));
    }
    return NULL;
}
static void parallel_dictionary(void)
{
    struct qs_dict *d=calloc(1,qs_dict_bytes(32768,1024*1024)); assert(d);
    *d=(struct qs_dict){.entries=32768,.capacity=32768,.data=1024*1024};
    pthread_t threads[4]; struct dictionary_test tasks[4];
    for(unsigned w=0;w<4;++w) { tasks[w]=(struct dictionary_test){d,w}; assert(!pthread_create(&threads[w],NULL,dictionary_worker,&tasks[w])); }
    for(unsigned w=0;w<4;++w) assert(!pthread_join(threads[w],NULL));
    assert(d->count==20997); free(d);
}
static void merge_input_parallelism(void)
{
    struct qp_plan *p=calloc(1,sizeof(*p)); assert(p);
    p->count=5;
    for(unsigned i=0;i<p->count;++i) p->nodes[i].left=p->nodes[i].right=-1;
    p->nodes[0].kind=QP_JOIN; p->nodes[0].left=1; p->nodes[0].right=2;
    p->nodes[1].kind=QP_SCAN;
    p->nodes[2].kind=QP_AGGREGATE; p->nodes[2].left=3;
    p->nodes[3].kind=QP_SCAN; p->nodes[3].bytes=8*QP_SCAN_BYTES;
    qp_parallel_boundaries(p,0); assert(p->nodes[3].parallel);
    p->nodes[3].parallel=0; p->nodes[0].kind=QP_INDEX_JOIN;
    qp_parallel_boundaries(p,0); assert(!p->nodes[3].parallel);
    free(p);
}
int main(int argc,char **argv)
{
    dictionary_size();
    merge_input_parallelism();
    assert(qp_worker_mask(0)==0 && qp_worker_mask(32)==UINT32_MAX && qp_worker_mask(64)==UINT64_MAX);
    assert(argc==2 || argc==3 || argc==4); void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); assert(library);
    function capacity=(function)dlsym(library,"pg_sina_workmem_capacity"); assert(capacity);
    if(argc==3) {
        struct femu_csd_args args={0}; struct cs_work_stats stats;
        uint64_t expected=strtoull(argv[2],NULL,10)*1024*1024;
        assert(capacity(&args)==(int64_t)expected);
        assert(!cs_work_get_stats(&stats) && stats.capacity_bytes==expected && !stats.used_bytes && !stats.active_contexts);
        args.numr=1; assert(capacity(&args)<0);
        printf("PASS device resource query: %llu MiB, no query allocations\n",(unsigned long long)(expected/1024/1024));
        return 0;
    }
    function run=(function)dlsym(library,"pg_sina_plan"); assert(run);
    if(argc==4) {
        assert(!strcmp(argv[2],"--workers")); test_workers=strtoul(argv[3],NULL,10);
        assert(test_workers && test_workers<=QS_WORKERS && !cs_work_init(256*1024*1024));
        test_scheduled=true;
        test(run,QP_INNER,20003,23,256,1,TEST_VECTOR_AGG);
        test(run,QP_INNER,0,23,256,1,TEST_VECTOR_AGG);
        test(run,QP_INNER,1,23,256,1,TEST_VECTOR_AGG);
        for(unsigned join=QP_INNER;join<=QP_ANTI;++join)
            test(run,join,20003,23,256,1,TEST_BUILD_PHASE);
        test(run,QP_INNER,31,50003,256,1,TEST_BUILD_PHASE);
        for(unsigned join=QP_INNER;join<=QP_ANTI;++join)
            test(run,join,31,5003,256,1,TEST_HASH_QUAL);
        test_scheduled=false;
        for(unsigned join=QP_INNER;join<=QP_ANTI;++join)
            test(run,join,20003,23,256,1,TEST_BUILD_PHASE);
        test(run,QP_INNER,31,50003,256,1,TEST_BUILD_PHASE);
        test(run,QP_LEFT,20003,23,256,1,TEST_HASH_GROUP);
        test(run,QP_LEFT,20003,23,256,2,TEST_HASH_GROUP);
        test(run,QP_LEFT,20003,23,256,2,TEST_INDEX_EXPR);
        test(run,QP_SEMI,20003,257,256,2,TEST_INDEX_EXPR);
        test(run,QP_ANTI,20003,257,256,2,TEST_INDEX_EXPR);
        test(run,QP_LEFT,20003,23,256,2,TEST_DEVICE_IO);
        multiple_groups(run,256,Q_SUM,0,test_workers);
        multiple_groups(run,256,Q_AVG,1,test_workers);
        multiple_groups(run,256,Q_COUNT,0,test_workers);
        multiple_group_aggregates(run,256,Q_SUM,0,test_workers,GROUP_COUNTS);
        multiple_group_aggregates(run,256,Q_AVG,1,test_workers,GROUP_COUNTS);
        multiple_group_aggregates(run,256,Q_AVG,1,test_workers,GROUP_NUMERIC_EXTREMES);
        multiple_group_aggregates(run,256,Q_MIN,0,test_workers,GROUP_TEXT_EXTREMES);
        printf("PASS %u CU slots: parallel build/probe, exact SUM/AVG, global DISTINCT, cleanup\n",test_workers);
        return 0;
    }
    assert(!cs_work_init(64*1024*1024));
    dictionary_growth();
    probe_payload(run,1,0,0,0); probe_payload(run,2,0,0,0); probe_payload(run,2,1,0,0);
    probe_payload(run,2,2,0,0); probe_payload(run,1,3,0,0); probe_payload(run,2,3,0,0);
    probe_payload(run,1,0,1,0); probe_payload(run,2,0,1,0); grouped_probe(run);
    probe_payload(run,2,0,2,0);
    probe_payload(run,2,0,3,0);
    probe_payload(run,2,0,4,0); probe_payload(run,2,0,5,0);
    probe_payload(run,1,0,0,1); probe_payload(run,2,0,0,1);
    probe_payload(run,2,0,0,2); probe_payload(run,2,0,0,3);
    probe_payload(run,2,0,0,4); probe_payload(run,2,0,0,5);
    probe_payload(run,1,0,1,1); probe_payload(run,2,0,1,1);
    probe_payload(run,2,0,1,2); probe_payload(run,2,0,1,3);
    namespace_reentry(run);
    for(int joined=0;joined<2;++joined) {
        parameter_projection(run,joined,0,0);
        parameter_projection(run,joined,1,0);
        parameter_projection(run,joined,1,1);
    }
    test_scheduled=true;
    for(unsigned join=QP_INNER;join<=QP_ANTI;++join) {
        test(run,join,31,5003,64,1,TEST_HASH_QUAL);
        test(run,join,31,5003,4,1,TEST_HASH_QUAL);
    }
    test(run,QP_INNER,31,150000,4,1,TEST_HASH_QUAL);
    test(run,QP_INNER,20003,23,64,1,TEST_VECTOR_AGG);
    test(run,QP_INNER,0,23,64,1,TEST_VECTOR_AGG);
    test(run,QP_INNER,1,23,64,1,TEST_VECTOR_AGG);
    test(run,QP_INNER,20003,23,4,1,TEST_VECTOR_AGG);
    for(unsigned join=QP_INNER;join<=QP_ANTI;++join) {
        test(run,join,20003,23,64,1,TEST_BUILD_PHASE);
        test(run,join,31,50003,64,1,TEST_BUILD_PHASE);
    }
    test(run,QP_INNER,31,23,64,1,TEST_BUILD_PHASE);
    test(run,QP_INNER,0,23,64,1,TEST_COUNT_ROWS);
    test(run,QP_LEFT,20003,23,4,1,TEST_HASH_GROUP);
    test(run,QP_INNER,31,150000,4,1,TEST_PARTITION);
    multiple_groups(run,64,Q_SUM,0,4); multiple_groups(run,64,Q_AVG,1,4);
    multiple_groups(run,64,Q_COUNT,0,4); multiple_groups(run,4,Q_SUM,1,4);
    test_scheduled=false;
    for(int hash=0;hash<3;++hash)
        for(unsigned join=QP_INNER;join<=QP_ANTI;++join) test(run,join,517,23,64,hash,TEST_SERIAL);
    for(unsigned join=QP_INNER;join<=QP_ANTI;++join) {
        test(run,join,5170,23,64,1,TEST_PARALLEL);
        test(run,join,5170,23,64,1,TEST_BUILD_PHASE);
        test(run,join,5170,23,64,2,TEST_PARALLEL);
    }
    /* Concurrent duplicate-key builds, including multiple growth barriers. */
    test(run,QP_INNER,31,50003,64,1,TEST_BUILD_PHASE);
    test(run,QP_ANTI,31,50003,4,1,TEST_BUILD_PHASE);
    test(run,QP_INNER,20003,23,64,2,TEST_INDEX_PIPELINE);
    test(run,QP_INNER,20003,23,4,2,TEST_SERIAL);
    test(run,QP_INNER,20003,23,64,2,TEST_INDEX_HASH);
    test(run,QP_INNER,20003,23,4,2,TEST_INDEX_HASH);
    test(run,QP_INNER,20003,23,64,2,TEST_INDEX_CHAIN);
    test(run,QP_INNER,20003,23,4,2,TEST_INDEX_CHAIN);
    test(run,QP_LEFT,20003,23,4,2,TEST_HASH_GROUP);
    test(run,QP_INNER,20003,23,4,2,TEST_INDEX_EXPR);
    test(run,QP_LEFT,20003,23,64,2,TEST_INDEX_EXPR);
    test(run,QP_SEMI,20003,257,64,2,TEST_INDEX_EXPR);
    test(run,QP_ANTI,20003,257,4,2,TEST_INDEX_EXPR);
    test(run,QP_LEFT,20003,23,4,2,TEST_DEVICE_IO);
    test(run,QP_INNER,120000,23,8,1,TEST_PARALLEL);
    test(run,QP_INNER,120000,23,8,1,TEST_LARGE_SCAN);
    test(run,QP_INNER,5170,23,64,1,TEST_COUNT_ROWS);
    test(run,QP_LEFT,23,23,64,1,TEST_WIDE_HASH);
    test(run,QP_LEFT,23,23,64,2,TEST_WIDE_HASH);
    test(run,QP_LEFT,23,23,64,1,TEST_SIGNED_HASH);
    test(run,QP_LEFT,23,23,64,1,TEST_BIGINT_HASH);
    test(run,QP_LEFT,5170,23,4,1,TEST_HASH_GROUP);
    test(run,QP_INNER,5170,23,64,1,TEST_HASH_GROUP);
    test(run,QP_INNER,31,150000,4,1,TEST_PARTITION);
    test(run,QP_INNER,31,150000,8,1,TEST_PARTITION);
    test(run,QP_INNER,31,150000,4,1,TEST_PARTITION_SERIAL);
    test(run,QP_INNER,31,150000,8,1,TEST_PARTITION_SERIAL);
    test(run,QP_INNER,31,150000,4,1,TEST_PARTITION_SKEW);
    test(run,QP_LEFT,20000,23,4,0,TEST_SERIAL);
    test(run,QP_LEFT,73,20000,8,1,TEST_SERIAL);
    test(run,QP_INNER,73,60000,8,1,TEST_BUILD_FALLBACK);
    test(run,QP_INNER,73,60000,4,1,TEST_BUILD_FALLBACK);
    nested_sort(run,23,4,Q_SUM); nested_sort(run,23,4,Q_AVG);
    nested_sort(run,23,5,Q_SUM); nested_sort(run,23,5,Q_AVG);
    nested_sort(run,23,0,Q_SUM); nested_sort(run,1700,0,Q_SUM);
    nested_sort(run,1700,1,Q_SUM); nested_sort(run,1700,1,Q_AVG); nested_sort(run,1700,2,Q_AVG);
    nested_sort(run,20,1,Q_COUNT); nested_sort(run,20,1,Q_COUNT_STAR); nested_sort(run,1700,3,Q_AVG); parallel_dictionary();
    multiple_groups(run,4,Q_SUM,0,0); multiple_groups(run,64,Q_SUM,0,0); multiple_groups(run,4,Q_AVG,0,0);
    multiple_groups(run,4,Q_SUM,1,0); multiple_groups(run,64,Q_SUM,1,0); multiple_groups(run,4,Q_AVG,1,0);
    multiple_groups(run,4,Q_COUNT,0,0); multiple_groups(run,64,Q_COUNT,0,0);
    multiple_groups(run,64,Q_SUM,0,4); multiple_groups(run,64,Q_AVG,1,4);
    multiple_groups(run,64,Q_COUNT,0,4); multiple_groups(run,4,Q_SUM,1,4);
    multiple_groups(run,4,Q_AVG,0,4); multiple_groups(run,4,Q_COUNT,1,4);
    multiple_group_aggregates(run,4,Q_SUM,0,0,GROUP_COUNTS);
    multiple_group_aggregates(run,64,Q_AVG,1,4,GROUP_COUNTS);
    multiple_group_aggregates(run,4,Q_AVG,1,4,GROUP_COUNTS);
    multiple_group_aggregates(run,4,Q_SUM,0,0,GROUP_NUMERIC_EXTREMES);
    multiple_group_aggregates(run,4,Q_MAX,0,0,GROUP_TEXT_EXTREMES);
    multiple_group_aggregates(run,64,Q_AVG,1,4,GROUP_NUMERIC_EXTREMES);
    multiple_group_aggregates(run,4,Q_MIN,0,4,GROUP_TEXT_EXTREMES);
    puts("PASS native Plan joins, NULL/duplicate keys, MIN/MAX/AVG, spill and cleanup");
}
