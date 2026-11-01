/* Device external sort with an in-memory NVM emulator. Only the test owns
 * host allocations; all operator runs/scratch must fit the shared workmem. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "query_vector.h"
#include "dictionary.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int64_t (*function)(struct femu_csd_args *);
typedef int64_t (*columns_function)(struct femu_csd_args *,const struct qs_plan *,const struct qs_vector *,unsigned,unsigned);
static columns_function columns_sink;
/* Borrow evaluated cells with a nonzero begin and a column stride. The
 * operator must retain input_position while this same batch spills. */
static int64_t sink_columns(struct femu_csd_args *a)
{
    const struct qs_plan *p=a->mr_addr[1]; const struct qs_result *in=a->mr_addr[0];
    struct qs_vector vectors[3]={0};
    for(unsigned col=0;col<3;++col)
        vectors[col]=(struct qs_vector){.kind=QS_CELLS,.cells=(const struct qs_cell *)in->data+col,.cell_stride=3};
    return columns_sink(a,p,vectors,in->begin,in->rows);
}
static void *blocks[8192];
static unsigned next_token=1,stored,loaded,workers=4;
static size_t capacity=8U*1024*1024,budget=8U*1024*1024,text_quota_extra;
static uint64_t complete_io(int64_t value,void *io)
{
    uint64_t action=(uint64_t)value&~CS_IO_VALUE_MASK,token=(uint64_t)value&CS_IO_VALUE_MASK;
    if(action==CS_NEED_STORE) {
        assert(token==QS_SORT_IO_BYTES && next_token<8192);
        token=next_token++; blocks[token]=malloc(QS_SORT_IO_BYTES); assert(blocks[token]);
        memcpy(blocks[token],io,QS_SORT_IO_BYTES); ++stored;
    } else if(action==CS_NEED_LOAD) {
        assert(token && token<next_token && blocks[token]);
        memcpy(io,blocks[token],QS_SORT_IO_BYTES); ++loaded;
    } else {
        assert(action==CS_NEED_FREE && token && token<next_token && blocks[token]);
        free(blocks[token]); blocks[token]=NULL;
    }
    return token;
}
static int64_t run(function f,struct femu_csd_args *args)
{
    args->cparam2=0;
    for(;;) {
        int64_t value=f(args);
        if(value<0) { struct cs_work_stats stats; assert(!cs_work_get_stats(&stats));
            fprintf(stderr,"sort status=%lld args=%d workers=%u used=%zu peak=%zu tokens=%u\n",(long long)value,args->numr,workers,stats.used_bytes,stats.peak_used_bytes,next_token); }
        assert(value>=0);
        if(!((uint64_t)value&~CS_IO_VALUE_MASK)) return value;
        args->cparam2=complete_io(value,args->mr_addr[args->numr-1]);
    }
}
static int host_order(const void *left,const void *right)
{
    unsigned a=*(const unsigned *)left,b=*(const unsigned *)right;
    if((a%11==0)!=(b%11==0)) return a%11==0?-1:1;
    return a%11==0?(a<b)-(a>b):(a>b)-(a<b);
}
static void check(function sink,function source,unsigned rows,int64_t limit,int64_t offset,int cancel,int profile,int packed)
{
    next_token=1; stored=loaded=0;
    unsigned *expected=malloc((rows?rows:1)*sizeof(*expected)); assert(expected);
    for(unsigned i=0;i<rows;++i) expected[i]=i;
    qsort(expected,rows,sizeof(*expected),host_order);
    struct qs_plan p={.having=-1,.nattrs=3,.ncols=3,.nexprs=3,.nout=3,.norder=2,.ordered_rows=1,.work_mem_bytes=budget/workers,.predicate=-1,.limit=limit,.offset=offset};
    struct qs_result *input=packed?calloc(1,sizeof(*input)):NULL;
    if(packed) { assert(input); p.ordered_rows=3; input->begin=7; }
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
    unsigned chunk=0,chunk_rows=packed==2?QS_VECTOR_ROWS-7:1001;
    for(unsigned first=0;first<rows;first+=chunk_rows) {
        unsigned n=rows-first; if(n>chunk_rows) n=chunk_rows;
        memset(qs_nulls(c),0,((c->capacity+7)/8)*3); c->rows=c->selected=n;
        for(unsigned i=0;i<n;++i) {
            unsigned key=(uint64_t)(rows-1-first-i)*1009%rows; qs_values(c)[i]=key;
            qs_values(c)[c->capacity+i]=(int64_t)key*123; qs_values(c)[2*c->capacity+i]=-(int64_t)key;
            qs_dscales(c)[c->capacity+i]=key%3;
            qs_dscales(c)[2*c->capacity+i]=key%5;
            if(key%11==0) qs_nulls(c)[i/8]|=1U<<(i%8);
            qs_selection(c)[i]=i;
            if(packed) {
                struct qs_cell *v=(struct qs_cell *)input->data+(size_t)(input->begin+i)*3;
                v[0]=(struct qs_cell){.value=key,.isnull=key%11==0};
                v[1]=(struct qs_cell){.value=(int64_t)key*123,.scale=2,.dscale=key%3};
                v[2]=(struct qs_cell){.value=-(int64_t)key,.dscale=key%5};
            }
        }
        if(packed) input->rows=n+input->begin;
        unsigned worker=chunk++%workers; tasks[worker].workers=workers; tasks[worker].input_bytes=n*24;
        void *mr[]={packed?(void *)input:c,&p,&tasks[worker],profile?(void *)&counters[worker]:io,io};
        long long len[]={packed?sizeof(*input):bytes,sizeof(p),sizeof(tasks[0]),profile?sizeof(counters[0]):QS_SORT_IO_BYTES,QS_SORT_IO_BYTES};
        struct femu_csd_args a={.numr=profile?5:4,.mr_addr=mr,.mr_len=len,.cparam1=profile};
        assert(!run(sink,&a));
        if(packed) assert(input->begin==7 && input->rows==n+7);
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
    free(input); free(out); free(io); free(c); free(expected);
}
enum { TEXT_KEYS=4096, TEXT_LENGTH=2048 };
static unsigned char *text_keys;
static unsigned text_lengths[TEXT_KEYS];
static int text_descending,text_nulls_first;
static unsigned text_key(unsigned row) { return row*1009U%TEXT_KEYS; }
static int text_order(const void *left,const void *right)
{
    unsigned a=*(const unsigned *)left,b=*(const unsigned *)right;
    int cmp;
    if((a%13==0)!=(b%13==0)) {
        cmp=(a%13==0)?1:-1; if(text_nulls_first) cmp=-cmp; return cmp;
    }
    if(a%13) {
        unsigned ka=text_key(a),kb=text_key(b),la=text_lengths[ka],lb=text_lengths[kb];
        cmp=memcmp(text_keys+(size_t)ka*TEXT_LENGTH,text_keys+(size_t)kb*TEXT_LENGTH,la<lb?la:lb);
        if(!cmp) cmp=(la>lb)-(la<lb);
        if(cmp) return text_descending?-cmp:cmp;
    }
    return (a<b)-(a>b); /* Second key is descending row number. */
}
static void text_sort(void *so,function sink,function source,int64_t limit,int64_t offset,int descending,int cancel,size_t memory)
{
    enum { CUS=4 };
    const unsigned ROWS=memory>=64U*1024*1024?262147:50003;
    int (*set_dictionary)(uint64_t,struct qs_dictionary *)=(void *)dlsym(so,"qs_sort_paged_dictionary");
    assert(set_dictionary);
    next_token=1; stored=loaded=0;
    cs_work_context *parent=cs_work_begin_limit(memory); assert(parent);
    struct qs_dictionary *dictionary=qs_dictionary_create(parent); assert(dictionary);
    struct qs_dictionary_access *access=qs_dictionary_access_create(dictionary); assert(access);
    struct qs_plan p={.nout=3,.norder=2,.ordered_rows=2,.having=-1,.predicate=-1,
        .limit=limit,.offset=offset,.work_mem_bytes=memory/(CUS*2)+text_quota_extra,.work_mem_parent=(uintptr_t)parent};
    p.outputs[0].type_oid=25; p.outputs[1].type_oid=p.outputs[2].type_oid=23;
    for(unsigned col=0;col<3;++col) p.outputs[col].index=col;
    p.order[0]=(struct qs_order){.output=0,.descending=descending,.nulls_first=!descending};
    p.order[1]=(struct qs_order){.output=1,.descending=1};
    struct qs_result *input=calloc(1,sizeof(*input)),*output=calloc(1,sizeof(*output));
    void *io=calloc(1,QS_SORT_IO_BYTES); assert(input && output && io);
    struct qs_task tasks[CUS]={{0}};
    for(unsigned i=0;i<CUS;++i) {
        void *mr[]={input,&p,&tasks[i],io}; long long len[]={sizeof(*input),sizeof(p),sizeof(tasks[i]),QS_SORT_IO_BYTES};
        struct femu_csd_args a={.numr=4,.mr_addr=mr,.mr_len=len};
        tasks[i].workers=CUS; assert(!run(sink,&a)); assert(tasks[i].state);
        assert(!set_dictionary(tasks[i].state,dictionary));
    }
    uint32_t ids[TEXT_KEYS]; text_keys=malloc((size_t)TEXT_KEYS*TEXT_LENGTH); assert(text_keys);
    for(unsigned i=0;i<TEXT_KEYS;++i) {
        char prefix[9]; assert(snprintf(prefix,sizeof(prefix),"%08u",i*7919U%TEXT_KEYS)==8);
        char *key=(char *)text_keys+(size_t)i*TEXT_LENGTH;
        memset(key,'a'+i%26,TEXT_LENGTH); memcpy(key,prefix,8);
        text_lengths[i]=memory>=64U*1024*1024?16+i*7919U%(TEXT_LENGTH-15):TEXT_LENGTH;
        /* Empty/short strings cross the 12-byte boundary; long strings
         * sharing a prefix must resume the exact dictionary comparison. */
        if(i<16) { text_lengths[i]=i; memset(key,'x',i); }
        else if(i%16==0) { memcpy(key,"shared-prefix",12); memcpy(key+12,prefix,8); }
        uint64_t completion=0;
        for(;;) {
            int64_t status=qs_dictionary_insert(dictionary,access,key,text_lengths[i],&ids[i],io,completion);
            if(!status) break;
            assert(status>QS_DICTIONARY_BUSY); completion=complete_io(status,io);
        }
    }
    struct qs_dictionary_stats before_sink; qs_dictionary_get_stats(dictionary,&before_sink);
    unsigned *expected=malloc(ROWS*sizeof(*expected)); assert(expected);
    text_descending=descending; text_nulls_first=!descending;
    for(unsigned i=0;i<ROWS;++i) expected[i]=i;
    qsort(expected,ROWS,sizeof(*expected),text_order);
    int columns=limit>=0;
    unsigned batch=columns?QS_VECTOR_ROWS-7:QS_RESULT_ROWS;
    if(columns) { p.ordered_rows=3; input->begin=7; }
    for(unsigned first=0,chunk=0;first<ROWS;first+=batch,++chunk) {
        unsigned count=ROWS-first; if(count>batch) count=batch;
        input->rows=count+input->begin;
        for(unsigned i=0;i<count;++i) {
            unsigned row=ROWS-1-first-i;
            struct qs_cell *cells=columns?(struct qs_cell *)input->data+(size_t)(i+input->begin)*3:input->data[i].cells;
            cells[0]=(struct qs_cell){.value=ids[text_key(row)],.isnull=row%13==0};
            cells[1]=(struct qs_cell){.value=row};
            cells[2]=(struct qs_cell){.value=row*7};
        }
        unsigned worker=chunk%CUS;
        void *mr[]={input,&p,&tasks[worker],io}; long long len[]={sizeof(*input),sizeof(p),sizeof(tasks[worker]),QS_SORT_IO_BYTES};
        struct femu_csd_args a={.numr=4,.mr_addr=mr,.mr_len=len}; assert(!run(columns?sink_columns:sink,&a));
        assert(input->begin==(columns?7U:0U) && input->rows==count+input->begin);
    }
    struct qs_group group={.count=CUS}; for(unsigned i=0;i<CUS;++i) group.states[i]=tasks[i].state;
    struct qs_dictionary_stats after_sink; qs_dictionary_get_stats(dictionary,&after_sink);
    if(memory<64U*1024*1024) assert(after_sink.loads>before_sink.loads);
    void *mr[]={&group,output,io}; long long len[]={sizeof(group),sizeof(*output),QS_SORT_IO_BYTES};
    struct femu_csd_args a={.numr=3,.mr_addr=mr,.mr_len=len};
    unsigned seen=0; uint64_t row_stores=0,row_loads=0;
    if(cancel==2) {
        /* Complete an I/O but cancel before the CSF acknowledges it. Both
         * dictionary pins and the just-completed token belong to this query. */
        for(;;) {
            int64_t status=source(&a); assert(status>QS_DICTIONARY_BUSY);
            a.cparam2=complete_io(status,io);
            if(((uint64_t)status&~CS_IO_VALUE_MASK)==CS_NEED_LOAD) break;
        }
        void *close[]={&group}; long long sizes[]={sizeof(group)};
        struct femu_csd_args args={.numr=1,.mr_addr=close,.mr_len=sizes}; assert(!source(&args));
        qs_dictionary_abort(dictionary);
        for(unsigned i=1;i<next_token;++i) { free(blocks[i]); blocks[i]=NULL; }
        goto released;
    }
    while(!output->finished) {
        assert(!run(source,&a));
        row_stores=output->metrics.spill_write_bytes; row_loads=output->metrics.spill_read_bytes;
        for(unsigned i=0;i<output->rows;++i) {
            assert(seen+(uint64_t)offset<ROWS); unsigned row=expected[seen++ +offset];
            assert(output->data[i].cells[0].isnull==(row%13==0));
            if(row%13) assert(output->data[i].cells[0].value==ids[text_key(row)]);
            if(output->data[i].cells[1].value!=row || output->data[i].cells[2].value!=row*7)
                fprintf(stderr,"text row=%u expected=%u got=%llu key=%u null=%u limit=%lld\n",seen-1,row,
                    (unsigned long long)output->data[i].cells[1].value,(unsigned)output->data[i].cells[0].value,
                    output->data[i].cells[0].isnull,(long long)limit);
            assert(output->data[i].cells[1].value==row && output->data[i].cells[2].value==row*7);
        }
        if(cancel && !output->finished) {
            void *close[]={&group,io}; long long sizes[]={sizeof(group),QS_SORT_IO_BYTES};
            struct femu_csd_args args={.numr=2,.mr_addr=close,.mr_len=sizes}; assert(!run(source,&args)); break;
        }
    }
    if(!cancel) {
        unsigned count=ROWS-offset; if(limit>=0 && count>(uint64_t)limit) count=limit;
        assert(seen==count);
    }
    struct qs_dictionary_stats stats; qs_dictionary_get_stats(dictionary,&stats);
    assert(stats.strings==TEXT_KEYS);
    if(memory<64U*1024*1024) assert(stats.stores && stats.loads);
    if(limit<0) assert(row_stores>(uint64_t)ROWS*3*sizeof(struct qs_cell)+4*QS_SORT_IO_BYTES &&
        row_loads && (memory>=64U*1024*1024 || stats.loads>after_sink.loads)); /* Row/text spill; the dictionary may remain resident at 64 MiB. */
    uint64_t completion=0;
    for(;;) {
        int64_t status=qs_dictionary_close(dictionary,access,io,completion);
        if(!status) break;
        assert(status>QS_DICTIONARY_BUSY); completion=complete_io(status,io);
    }
    printf("text sort limit=%lld descending=%d cancel=%d rows=%u dict=%llu/%llu run=%llu/%llu\n",
        (long long)limit,descending,cancel,seen,(unsigned long long)stats.stores,(unsigned long long)stats.loads,
        (unsigned long long)row_stores,(unsigned long long)row_loads);
released:;
    cs_work_context_stats peak; assert(!cs_work_get_context_stats(parent,&peak)); assert(peak.peak_used_bytes<=memory);
    printf("text sort query workmem peak=%zu limit=%zu\n",peak.peak_used_bytes,memory);
    assert(!peak.used_bytes && !peak.allocations); assert(!cs_work_end(parent));
    for(unsigned i=1;i<next_token;++i) assert(!blocks[i]);
    free(expected); free(text_keys); text_keys=NULL; free(io); free(input); free(output);
}
int main(int argc,char **argv)
{
    assert(argc>=2 && argc<=5);
    int dictionary64=argc==3 && !strcmp(argv[2],"--dictionary-64m");
    int dictionary=argc==3 && (!strcmp(argv[2],"--dictionary") || dictionary64);
    if(dictionary64) capacity=budget=64U*1024*1024;
    if(argc>=3 && !dictionary) capacity=(size_t)strtoul(argv[2],NULL,10)*1024*1024;
    budget=argc>=4?(size_t)strtoul(argv[3],NULL,10)*1024*1024:capacity;
    if(argc==5) workers=strtoul(argv[4],NULL,10);
    assert(workers && workers<=QS_WORKERS);
    if(budget>capacity) budget=capacity;
    assert(capacity && budget && !cs_work_init(capacity));
    void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); assert(so);
    function sink=(function)dlsym(so,"pg_sina_sort_sink"),source=(function)dlsym(so,"pg_sina_sort_source"); assert(sink && source);
    columns_sink=(columns_function)dlsym(so,"qs_sort_sink_columns"); assert(columns_sink);
    if(dictionary64) {
        /* Parent/device stay at 64 MiB. Child limits from real concurrent
         * operators are not I/O-block aligned; cover a block of remainders. */
        for(text_quota_extra=46;text_quota_extra<65536;text_quota_extra+=4096)
            text_sort(so,sink,source,-1,0,0,0,capacity);
        dlclose(so); return 0;
    }
    if(dictionary) {
        text_sort(so,sink,source,-1,0,0,0,4U*1024*1024);
        text_sort(so,sink,source,17,5,1,0,4U*1024*1024);
        text_sort(so,sink,source,-1,0,1,1,4U*1024*1024);
        text_sort(so,sink,source,-1,0,0,2,4U*1024*1024);
        dlclose(so); return 0;
    }
    check(sink_columns,source,262147,-1,0,0,0,2);
    check(sink_columns,source,262147,17,5,0,0,2);
    check(sink_columns,source,262147,-1,0,1,0,2);
    check(sink,source,0,-1,0,0,0,0);
    check(sink,source,5003,-1,0,0,0,0);
    check(sink,source,262147,-1,0,0,1,0);
    check(sink,source,262147,17,5,0,1,0);
    check(sink,source,262147,-1,0,1,0,0);
    check(sink,source,262147,-1,0,2,0,0);
    check(sink,source,1048583,-1,0,0,0,0); /* Merge read buffers exceed workmem. */
    check(sink,source,5003,INT64_MAX,INT64_MAX,0,0,0);
    check(sink,source,0,-1,0,0,0,1);
    check(sink,source,262147,-1,0,0,1,1);
    check(sink,source,262147,17,5,0,1,1);
    check(sink,source,262147,-1,0,1,0,1);
    dlclose(so);
    puts("PASS external row sort, multiple sinks, memory pressure/reentry, NULL/precision, chunked output, TopN/OFFSET and abort cleanup");
    return 0;
}
