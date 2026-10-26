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
static void aggregate_plan(struct qp_plan *p)
{
    memset(&p->nodes[0],0,sizeof(p->nodes[0]));
    struct qp_node *n=&p->nodes[0]; n->kind=QP_AGGREGATE;
    struct qs_plan *e=&n->expr; e->nattrs=3; e->ncols=3; e->nexprs=6; e->naggs=e->nout=6;
    e->exprs[0]=(struct qs_expr){.op=Q_COLUMN,.type=Q_INTEGER,.column=0};
    e->exprs[1]=(struct qs_expr){.op=Q_COLUMN,.type=Q_STRING,.column=1};
    e->exprs[2]=(struct qs_expr){.op=Q_COLUMN,.type=Q_STRING,.column=2};
    e->exprs[3]=(struct qs_expr){.op=QP_SUBSTRING,.type=Q_STRING,.left=1,.right=4,.otherwise=5};
    e->exprs[4]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=2};
    e->exprs[5]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=6};
    e->aggs[0]=(struct qs_agg){.kind=Q_COUNT_STAR,.expr=-1};
    e->aggs[1]=(struct qs_agg){.kind=Q_SUM,.expr=0};
    e->aggs[2]=(struct qs_agg){.kind=Q_MIN,.expr=1};
    e->aggs[3]=(struct qs_agg){.kind=Q_MAX,.expr=2};
    e->aggs[4]=(struct qs_agg){.kind=Q_COUNT,.expr=1};
    e->aggs[5]=(struct qs_agg){.kind=Q_MIN,.expr=3};
}
static unsigned aggregate_scalar(struct execution *s,struct qs_dictionary *d,struct qs_dictionary_access *reader)
{
    aggregate_plan(s->plan);
    struct qp_node *n=&s->plan->nodes[0]; struct operator_state *o=&s->nodes[0];
    memset(o->accum,0,sizeof(o->accum)); n->distinct_aggs=(1U<<4)|(1U<<5);
    unsigned waits=0,min=KEYS,max=0,nonnull=0,nmax=0,distinct=0;
    unsigned char seen[KEYS]={0}; uint64_t sum=0;
    for(unsigned row=0;row<512;++row) {
        unsigned left=(row/2*193)%KEYS,right=(row*97+2000)%KEYS;
        int ln=row%13==0,rn=row%11==0;
        o->left[0]=(struct qs_cell){.value=row%17};
        o->left[1]=(struct qs_cell){.value=ids[left][0],.isnull=ln};
        o->left[2]=(struct qs_cell){.value=ids[right][1],.isnull=rn};
        for(unsigned tries=0;;++tries) {
            assert(tries<10000); int status=accumulate(s,n,o); if(!status) break;
            assert(status<0 && s->expression_wait && o->aggregate_input->active);
            /* A missing argument must not partially update this row. */
            if(!o->aggregate_input->ready) assert(o->accum[0].count==row && o->accum[1].sum.value==sum);
            waits+=resume(s);
        }
        sum+=row%17;
        if(!ln) { ++nonnull; if(left<min) min=left; if(!seen[left]) { seen[left]=1; ++distinct; } }
        if(!rn) { ++nmax; if(right>max) max=right; }
        assert(o->accum[0].count==row+1 && o->accum[1].sum.value==sum);
        assert(o->accum[2].count==nonnull && o->accum[3].count==nmax);
        assert(o->accum[4].count==distinct && o->accum[5].count==distinct);
    }
    assert(o->accum[2].extreme.value==ids[min][0] && o->accum[3].extreme.value==ids[max][1]);
    char bytes[LENGTH]; make_text(bytes,min,0); check_string(d,reader,o->accum[5].extreme.value,bytes+2,6);
    assert(waits && !o->aggregate_input->active);
    return waits;
}
static unsigned scalar_group_keys(struct execution *s,struct qs_dictionary *d,struct qs_dictionary_access *reader)
{
    aggregate_plan(s->plan);
    struct qp_node *n=&s->plan->nodes[0]; struct operator_state *o=&s->nodes[0];
    n->nkeys=n->expr.nkeys=3; n->expr.nexprs=8; n->expr.nstrings=1;
    n->expr.keys[0]=3; n->expr.keys[1]=6; n->expr.keys[2]=7;
    n->expr.exprs[6]=(struct qs_expr){.op=QP_SUBSTRING,.type=Q_STRING,.left=2,.right=4,.otherwise=5};
    n->expr.exprs[7]=(struct qs_expr){.op=Q_CONST,.type=Q_STRING,.constant=0};
    n->expr.strings[0].length=5; memcpy(n->expr.strings[0].bytes,"group",5);
    o->left[1]=(struct qs_cell){.value=ids[0][0]};
    o->left[2]=(struct qs_cell){.value=ids[KEYS-1][1]};
    unsigned waits=0;
    while(initialize_group_keys(s,n,o)) { assert(s->expression_wait); waits+=resume(s); }
    /* Evict the input strings before equality re-evaluates the key expressions. */
    for(unsigned i=512;i<KEYS-512;i+=512) {
        char key[LENGTH]; make_text(key,i,0); check_string(d,reader,ids[i][0],key,LENGTH);
    }
    int status;
    while((status=group_same(s,n,o))<0) { assert(s->expression_wait); waits+=resume(s); }
    assert(status==1 && !o->aggregate_input->active);
    o->left[2].value=ids[4000][1];
    while((status=group_same(s,n,o))<0) { assert(s->expression_wait); waits+=resume(s); }
    assert(!status && !o->aggregate_input->active && waits);
    return waits;
}
static unsigned aggregate_vectors(struct execution *s,struct projection_batch *b,int grouped)
{
    aggregate_plan(s->plan);
    struct qp_node *n=&s->plan->nodes[0]; struct operator_state *o=&s->nodes[0];
    n->expr.naggs=n->expr.nout=5; memset(o->accum,0,sizeof(o->accum));
    b->plan=n->expr; b->aggregate_position=b->aggregate_row=0;
    s->nodes[0].projection=b;
    b->plan.outputs[0].index=-1;
    for(unsigned a=1;a<5;++a) b->plan.outputs[a].index=n->expr.aggs[a].expr;
    for(unsigned a=0;a<5;++a) b->group_values[a]=n->expr.aggs[a].expr;
    if(grouped) {
        n->nkeys=n->expr.nkeys=1; n->expr.keys[0]=3; n->expr.exprs[3].type=Q_STRING;
        b->group_keys[0]=3; b->plan.nout=3;
        for(unsigned i=0;i<3;++i) b->plan.outputs[i].index=i;
    }
    unsigned waits=0,min=KEYS,max=0,nonleft=0,nonright=0,total=0; uint64_t sum=0;
    for(unsigned batch=0;batch<(grouped?1U:3U);++batch) {
        b->position=0; b->count=grouped?129:128;
        for(unsigned col=0;col<4;++col) { memset(&b->vectors[col],0,sizeof(b->vectors[col])); b->vectors[col].kind=QS_NARROW; }
        for(unsigned row=0;row<b->count;++row) {
            unsigned at=batch*128+row,left=at*173%KEYS,right=(at*101+3000)%KEYS;
            b->vectors[0].narrow[row]=at%19;
            b->vectors[1].narrow[row]=ids[left][0]; b->vectors[1].nulls[row]=at%13==0;
            b->vectors[2].narrow[row]=ids[right][1]; b->vectors[2].nulls[row]=at%11==0;
            b->vectors[3].narrow[row]=ids[row<128?10:11][0];
            if(row==128) continue; /* Next group's first row stays unconsumed. */
            ++total; sum+=at%19;
            if(at%13) { ++nonleft; if(left<min) min=left; }
            if(at%11) { ++nonright; if(right>max) max=right; }
        }
        for(unsigned tries=0;;++tries) {
            assert(tries<10000);
            int status=grouped?stream_group_vectors(s,0):aggregate_sink_vectors(s,0);
            if(status==(grouped?2:0)) break;
            assert(status<0 && s->expression_wait); waits+=resume(s);
        }
        assert(b->position==128 && !b->aggregate_position && !b->aggregate_row);
        assert(o->accum[0].count==total && o->accum[1].sum.value==sum);
        assert(o->accum[2].count==nonleft && o->accum[3].count==nonright && o->accum[4].count==nonleft);
        assert(o->accum[2].extreme.value==ids[min][0] && o->accum[3].extreme.value==ids[max][1]);
    }
    assert(waits); o->projection=NULL; o->phase=0; return waits;
}
static unsigned combine_values(struct execution *s)
{
    aggregate_plan(s->plan); s->plan->nodes[0].expr.naggs=5;
    struct operator_state *to=&s->nodes[0]; memset(to->accum,0,sizeof(to->accum));
    unsigned waits=0,min=KEYS,max=0;
    for(unsigned worker=0;worker<8;++worker) {
        /* Test fixtures model another CU's immutable partial aggregate. */
        struct operator_state *from=calloc(1,sizeof(*from)); assert(from);
        unsigned a=(worker*701+1000)%KEYS,b=(worker*997+500)%KEYS;
        if(a<min) min=a;
        if(b>max) max=b;
        from->accum[0].count=2; from->accum[1].count=2; from->accum[1].sum.value=worker+1;
        from->accum[2]=(struct accumulator){.count=2,.extreme={.value=ids[a][0]}};
        from->accum[3]=(struct accumulator){.count=2,.extreme={.value=ids[b][1]}};
        from->accum[4].count=2;
        for(unsigned tries=0;;++tries) {
            assert(tries<10000); int status=aggregate_combine(s,0,from); if(!status) break;
            assert(status<0 && s->expression_wait); waits+=resume(s);
        }
        assert(!to->aggregate_position && to->accum[0].count==2*(worker+1) &&
            to->accum[1].sum.value==(worker+1)*(worker+2)/2);
        free(from);
    }
    assert(to->accum[2].extreme.value==ids[min][0] && to->accum[3].extreme.value==ids[max][1]);
    assert(waits); return waits;
}
static void equality_no_io(struct execution *s)
{
    struct qp_node *n=&s->plan->nodes[0]; n->nkeys=2;
    n->expr.keys[0]=1; n->expr.keys[1]=0;
    struct qs_cell a[2]={{.value=ids[0][0]},{.value=10,.scale=1}},b[2]={a[0],{.value=1}};
    unsigned before=loads; assert(group_equal(s,n,a,b)==1);
    b[0].value=ids[KEYS-1][0]; assert(!group_equal(s,n,a,b));
    a[0].isnull=b[0].isnull=1; assert(group_equal(s,n,a,b)==1);
    assert(loads==before && !s->text_wait);
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
    if(exporting==2) {
        aggregate_plan(s->plan); s->plan->nodes[0].expr.naggs=5;
        s->nodes[0].left[0]=(struct qs_cell){.value=7};
        s->nodes[0].left[1]=(struct qs_cell){.value=1};
        s->nodes[0].left[2]=(struct qs_cell){.value=4096};
        s->nodes[0].accum[2]=(struct accumulator){.count=1,.extreme={.value=4096}};
    }
    int cancelled=0;
    for(unsigned attempts=0;attempts<10000 && !cancelled;++attempts) {
        int status=exporting==2?accumulate(s,&s->plan->nodes[0],&s->nodes[0]):
            exporting?export_row(s,&root,out->data[0].cells,out):
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
int main(int argc,char **argv)
{
    assert(argc==1 || (argc==2 && !strcmp(argv[1],"--aggregate")));
    int aggregates=argc==2;
    assert(!cs_work_init(BUDGET));
    cs_work_context *ctx=cs_work_begin_limit(BUDGET); assert(ctx);
    struct qs_dictionary *d=qs_dictionary_create(ctx); assert(d);
    struct qs_dictionary_access *access=qs_dictionary_access_create(d); assert(access);
    struct execution *s=cs_work_calloc(ctx,1,sizeof(*s)); assert(s);
    s->context=ctx; s->io=io; s->text=qs_dictionary_text_create(d); assert(s->text);
    s->plan=calloc(1,sizeof(*s->plan)); assert(s->plan); plan(s->plan);
    s->nodes=cs_work_calloc(ctx,1,sizeof(*s->nodes)); assert(s->nodes);
    struct projection_batch *b=NULL;
    if(aggregates) {
        b=cs_work_calloc(ctx,1,sizeof(*b)); assert(b);
        b->vectors=cs_work_calloc(ctx,4,sizeof(*b->vectors)); assert(b->vectors);
    }
    for(unsigned row=0;row<KEYS;++row) for(unsigned col=0;col<2;++col) {
        char bytes[LENGTH]; make_text(bytes,row,col); ids[row][col]=insert(d,access,bytes,LENGTH);
    }
    unsigned scalar_waits,output_waits;
    if(aggregates) {
        scalar_waits=aggregate_scalar(s,d,access)+scalar_group_keys(s,d,access);
        output_waits=aggregate_vectors(s,b,0)+aggregate_vectors(s,b,1)+combine_values(s);
        equality_no_io(s);
    } else { scalar_waits=projection(s,d,access); output_waits=output(s); }
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
    if(aggregates) {
        cancel_operation(CS_NEED_STORE,2); cancel_operation(CS_NEED_LOAD,2);
    } else {
    cancel_operation(CS_NEED_STORE,0); cancel_operation(CS_NEED_LOAD,0);
    cancel_operation(CS_NEED_STORE,1); cancel_operation(CS_NEED_LOAD,1);
    cancel_operation(0,1);
    }
    puts(aggregates?"PASS dictionary ID grouping and resumable scalar/vector/stream MIN/MAX and partial combine":
        "PASS scalar text memo, retained input, short circuit, partial export and cancellation/error cleanup");
    return 0;
}
