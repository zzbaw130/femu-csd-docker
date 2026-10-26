/* Compile the private executor into this test: exercise its actual scalar
 * projection and output conversion without adding a test-only CSF/API. */
#include "../plan.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define KEYS 8192U
#define LENGTH 192U
#define BUDGET (1024U*1024)
static void **tokens;
static unsigned ntokens,live,stores,loads;
static unsigned char io[QS_SORT_IO_BYTES];
static uint32_t ids[KEYS][2];
static uint64_t complete(int64_t request)
{
    uint64_t op=(uint64_t)request&~CS_IO_VALUE_MASK,value=(uint64_t)request&CS_IO_VALUE_MASK;
    if(op==CS_NEED_STORE) {
        assert(value==QS_SORT_IO_BYTES); value=++ntokens;
        tokens=realloc(tokens,(ntokens+1)*sizeof(*tokens)); assert(tokens);
        tokens[value]=malloc(QS_SORT_IO_BYTES); assert(tokens[value]);
        memcpy(tokens[value],io,QS_SORT_IO_BYTES); ++live; ++stores;
    } else {
        assert(value && value<=ntokens && tokens[value]);
        if(op==CS_NEED_LOAD) { memcpy(io,tokens[value],QS_SORT_IO_BYTES); ++loads; }
        else { assert(op==CS_NEED_FREE); free(tokens[value]); tokens[value]=NULL; --live; }
    }
    return value;
}
static void make_text(char *bytes,unsigned row,unsigned col)
{
    memset(bytes,'a'+row%26,LENGTH);
    int n=snprintf(bytes,32,"\xc3\xa9%06u-%c-",row,col?'b':'a'); assert(n>0 && n<32);
    bytes[n]='x';
}
static uint32_t insert(struct qs_dictionary *d,struct qs_dictionary_access *a,const char *key,unsigned length)
{
    uint32_t id=0; uint64_t completion=0;
    for(;;) {
        int64_t rc=qs_dictionary_insert(d,a,key,length,&id,io,completion);
        if(!rc) return id;
        assert(rc>QS_DICTIONARY_BUSY); completion=complete(rc);
    }
}
static void check_string(struct qs_dictionary *d,struct qs_dictionary_access *a,uint32_t id,const char *oracle,unsigned expected)
{
    unsigned length=0; const char *bytes=NULL; uint64_t completion=0;
    for(;;) {
        int64_t rc=qs_dictionary_pin(d,a,id,&bytes,&length,io,completion);
        if(!rc) break;
        assert(rc>QS_DICTIONARY_BUSY); completion=complete(rc);
    }
    assert(length==expected && !memcmp(bytes,oracle,length)); qs_dictionary_unpin(a);
}
static unsigned resume(struct execution *s)
{
    unsigned count=0; int64_t request=s->text_wait;
    assert(s->expression_wait && request>QS_DICTIONARY_BUSY);
    do {
        uint64_t completion=complete(request); ++count;
        request=qs_dictionary_text_resume(s->text,io,completion);
        assert(request==0 || request>QS_DICTIONARY_BUSY);
    } while(request);
    s->text_wait=0; s->expression_wait=0; return count;
}
static void plan(struct qp_plan *p)
{
    p->count=1; p->nodes[0].kind=QP_PROJECT;
    struct qs_plan *e=&p->nodes[0].expr; e->predicate=-1; e->nattrs=4; e->ncols=4; e->nexprs=18; e->nstrings=2;
    e->exprs[0]=(struct qs_expr){.op=Q_COLUMN,.type=Q_STRING,.column=0};
    e->exprs[1]=(struct qs_expr){.op=Q_COLUMN,.type=Q_STRING,.column=1};
    e->exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=2};
    e->exprs[3]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=6};
    e->exprs[4]=(struct qs_expr){.op=QP_SUBSTRING,.type=Q_STRING,.left=0,.right=2,.otherwise=3};
    e->exprs[5]=(struct qs_expr){.op=QP_SUBSTRING,.type=Q_STRING,.left=1,.right=2,.otherwise=3};
    e->exprs[6]=(struct qs_expr){.op=Q_EQ,.type=Q_BOOLEAN,.left=4,.right=5};
    e->exprs[7]=(struct qs_expr){.op=Q_CONST,.type=Q_STRING,.constant=0};
    e->strings[0].length=5; memcpy(e->strings[0].bytes,"%-a-%",5);
    e->exprs[8]=(struct qs_expr){.op=QP_LIKE,.type=Q_BOOLEAN,.left=0,.right=7};
    e->exprs[9]=(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=6,.right=8};
    e->exprs[10]=(struct qs_expr){.op=Q_CASE,.type=Q_STRING,.left=9,.right=4,.otherwise=5};
    e->exprs[11]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=2};
    e->exprs[12]=(struct qs_expr){.op=Q_CONST,.type=Q_STRING,.constant=1};
    e->strings[1].length=8; memcpy(e->strings[1].bytes,"fallback",8);
    e->exprs[13]=(struct qs_expr){.op=QP_COALESCE,.type=Q_STRING,.left=17,.right=12};
    e->exprs[14]=(struct qs_expr){.op=Q_LT,.type=Q_BOOLEAN,.left=0,.right=1};
    e->exprs[15]=(struct qs_expr){.op=Q_CONST,.type=Q_BOOLEAN,.constant=0};
    e->exprs[16]=(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=15,.right=14};
    e->exprs[17]=(struct qs_expr){.op=Q_COLUMN,.type=Q_STRING,.column=3};
    unsigned outputs[]={4,8,10,13,14,11,0,1}; e->nout=sizeof(outputs)/sizeof(outputs[0]);
    for(unsigned i=0;i<e->nout;++i) e->outputs[i].index=outputs[i];
}
static unsigned projection(struct execution *s,struct qs_dictionary *d,struct qs_dictionary_access *reader)
{
    unsigned waits=0;
    for(unsigned i=0;i<512;++i) {
        unsigned left=(i*173)%KEYS,right=i%7?(i*71+4000)%KEYS:left;
        int right_null=i%13==0;
        struct qs_cell input[4]={{.value=ids[left][0]},{.value=ids[right][1],.isnull=right_null},
            {.value=i},{.value=ids[left][0],.isnull=i%3==0}},out[QS_OUT];
        int status;
        for(unsigned attempts=0;;++attempts) {
            assert(attempts<10000);
            status=project(s,&s->plan->nodes[0],input,NULL,out);
            if(status>=0) break;
            assert(s->nodes[0].project_retry && s->expression_wait);
            /* Once the source advances its stack row may be overwritten. */
            memset(input,0,sizeof(input)); waits+=resume(s);
        }
        assert(status==1 && !s->nodes[0].project_retry);
        char a[LENGTH],b[LENGTH]; make_text(a,left,0); make_text(b,right,1);
        check_string(d,reader,out[0].value,a+2,6);
        assert(!out[1].isnull && out[1].value==1);
        assert(out[2].isnull==right_null);
        if(!right_null) check_string(d,reader,out[2].value,(left==right?a:b)+2,6);
        check_string(d,reader,out[3].value,i%3?a:"fallback",i%3?LENGTH:8);
        assert(out[4].isnull==right_null && out[5].value==i);
        if(!right_null) assert(out[4].value==(memcmp(a,b,LENGTH)<0));
        assert(out[6].value==ids[left][0] && out[7].value==ids[right][1] && out[7].isnull==right_null);
        /* Exercise a filter using the same scalar memo before its outputs. */
        s->plan->nodes[0].expr.predicate=9;
        struct qs_cell filtered[4]={{.value=ids[left][0]},{.value=ids[right][1],.isnull=right_null},
            {.value=i},{.isnull=1}};
        for(;;) {
            status=project(s,&s->plan->nodes[0],filtered,NULL,out);
            if(status>=0) break;
            waits+=resume(s);
        }
        assert(status==(!right_null && left==right));
        s->plan->nodes[0].expr.predicate=-1;
    }
    /* The unneeded operand contains invalid IDs and must perform no I/O. */
    struct qs_eval e; struct qs_cell bad[4]={{.value=UINT32_MAX},{.value=UINT32_MAX}},value;
    init_eval(&e,&s->plan->nodes[0].expr,bad,NULL);
    unsigned before=loads; assert(!evaluate(s,&e,16,&value) && !value.isnull && !value.value);
    assert(loads==before && !s->text_wait);
    return waits;
}
static unsigned output(struct execution *s)
{
    struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    unsigned short_id,empty_id;
    while(text_insert(s,"tiny",4,&short_id)) resume(s);
    assert(!text_release(s));
    while(text_insert(s,"",0,&empty_id)) resume(s);
    assert(!text_release(s));
    struct qs_plan root={.nout=6,.nexprs=6};
    for(unsigned i=0;i<6;++i) { root.outputs[i].index=i; root.exprs[i].type=i==2?Q_INTEGER:Q_STRING; }
    unsigned waits=0,partial=0;
    for(unsigned i=0;i<512;++i) {
        unsigned a=i*43%KEYS,b=(i*179+3000)%KEYS;
        out->str_bytes=0;
        struct qs_cell *row=out->data[0].cells;
        row[0]=(struct qs_cell){.value=ids[a][0]}; row[1]=(struct qs_cell){.value=empty_id};
        row[2]=(struct qs_cell){.value=i}; row[3]=(struct qs_cell){.value=UINT32_MAX,.isnull=1};
        row[4]=(struct qs_cell){.value=short_id}; row[5]=(struct qs_cell){.value=ids[b][1]};
        for(unsigned attempts=0;export_row(s,&root,row,out);++attempts) {
            assert(attempts<10000 && s->output_pending);
            partial+=s->output_column>0; waits+=resume(s);
        }
        assert(!s->output_pending && !s->output_column && row[2].value==i && row[3].isnull);
        assert(out->str_bytes==2*LENGTH && !row[1].isnull && !qs_cell_string(&row[1]));
        assert(qs_cell_string(&row[4])==4 && !qs_cell_is_string_ref(&row[4]) &&
            !memcmp(qs_cell_string_bytes(&row[4]),"tiny",4));
        for(unsigned col=0;col<2;++col) {
            char oracle[LENGTH]; make_text(oracle,col?b:a,col);
            unsigned slot=col?5:0;
            assert(qs_cell_is_string_ref(&row[slot]) && qs_cell_string(&row[slot])==LENGTH);
            assert(!memcmp(out->str_data+(unsigned)row[slot].value,oracle,LENGTH));
        }
    }
    assert(waits && partial); free(out); return waits;
}
/* Abort after I/O completion but before its token is acknowledged. A query
 * scope also owns this token, so it must reclaim it without needing a retry. */
static void cancel_operation(uint64_t operation,int exporting)
{
    cs_work_context *ctx=cs_work_begin_limit(384U*1024); assert(ctx);
    struct qs_dictionary *d=qs_dictionary_create(ctx); assert(d);
    struct qs_dictionary_access *access=qs_dictionary_access_create(d); assert(access);
    struct execution *s=cs_work_calloc(ctx,1,sizeof(*s)); assert(s);
    s->context=ctx; s->io=io; s->text=qs_dictionary_text_create(d); assert(s->text);
    s->plan=calloc(1,sizeof(*s->plan)); assert(s->plan); plan(s->plan);
    s->nodes=cs_work_calloc(ctx,1,sizeof(*s->nodes)); assert(s->nodes);
    for(unsigned row=0;row<4096;++row) {
        char bytes[LENGTH]; make_text(bytes,row,0); assert(insert(d,access,bytes,LENGTH)==row+1);
    }
    struct qs_cell input[4]={{.value=operation?1:UINT32_MAX},{.value=4096},{.value=7},{.isnull=1}},result[QS_OUT];
    struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    struct qs_plan root={.nout=1,.nexprs=1}; root.exprs[0].type=Q_STRING;
    out->data[0].cells[0]=input[0];
    int cancelled=0;
    for(unsigned attempts=0;attempts<10000 && !cancelled;++attempts) {
        int status=exporting?export_row(s,&root,out->data[0].cells,out):
            project(s,&s->plan->nodes[0],input,NULL,result);
        assert(status<0);
        if(!operation) { assert(!s->expression_wait); cancelled=1; break; }
        assert(s->text_wait>QS_DICTIONARY_BUSY && s->expression_wait);
        int64_t request=s->text_wait;
        do {
            uint64_t completion=complete(request);
            if(((uint64_t)request&~CS_IO_VALUE_MASK)==operation) { cancelled=1; break; }
            request=qs_dictionary_text_resume(s->text,io,completion);
            assert(request==0 || request>QS_DICTIONARY_BUSY);
        } while(request);
        if(!cancelled) { s->expression_wait=0; s->text_wait=0; }
    }
    assert(cancelled); free(out); free(s->plan);
    qs_dictionary_abort(d); assert(!cs_work_end(ctx));
    for(unsigned i=1;i<=ntokens;++i) free(tokens[i]);
    free(tokens); tokens=NULL; ntokens=live=stores=loads=0;
    cs_work_stats total; assert(!cs_work_get_stats(&total) && !total.used_bytes && !total.active_contexts);
}
int main(void)
{
    assert(!cs_work_init(BUDGET));
    cs_work_context *ctx=cs_work_begin_limit(BUDGET); assert(ctx);
    struct qs_dictionary *d=qs_dictionary_create(ctx); assert(d);
    struct qs_dictionary_access *access=qs_dictionary_access_create(d); assert(access);
    struct execution *s=cs_work_calloc(ctx,1,sizeof(*s)); assert(s);
    s->context=ctx; s->io=io; s->text=qs_dictionary_text_create(d); assert(s->text);
    s->plan=calloc(1,sizeof(*s->plan)); assert(s->plan); plan(s->plan);
    s->nodes=cs_work_calloc(ctx,1,sizeof(*s->nodes)); assert(s->nodes);
    for(unsigned row=0;row<KEYS;++row) for(unsigned col=0;col<2;++col) {
        char bytes[LENGTH]; make_text(bytes,row,col); ids[row][col]=insert(d,access,bytes,LENGTH);
    }
    unsigned scalar_waits=projection(s,d,access),output_waits=output(s);
    struct qs_dictionary_stats ds; qs_dictionary_get_stats(d,&ds);
    cs_work_context_stats stats; assert(!cs_work_get_context_stats(ctx,&stats));
    assert(stats.peak_used_bytes<=BUDGET && ds.stores && ds.loads);
    printf("scalar waits=%u output waits=%u stores=%u loads=%u peak=%zu budget=%u\n",
        scalar_waits,output_waits,stores,loads,stats.peak_used_bytes,BUDGET);
    assert(scalar_waits && output_waits && !qs_dictionary_text_release(s->text));
    free(s->plan);
    uint64_t completion=0;
    for(;;) {
        int64_t rc=qs_dictionary_close(d,access,io,completion); if(!rc) break;
        assert(rc>QS_DICTIONARY_BUSY); completion=complete(rc);
    }
    assert(!live); free(tokens); tokens=NULL; ntokens=stores=loads=0; assert(!cs_work_end(ctx));
    cs_work_stats total; assert(!cs_work_get_stats(&total) && !total.used_bytes && !total.active_contexts);
    cancel_operation(CS_NEED_STORE,0); cancel_operation(CS_NEED_LOAD,0);
    cancel_operation(CS_NEED_STORE,1); cancel_operation(CS_NEED_LOAD,1);
    cancel_operation(0,1);
    puts("PASS scalar text memo, retained input, short circuit, partial export and cancellation/error cleanup");
    return 0;
}
