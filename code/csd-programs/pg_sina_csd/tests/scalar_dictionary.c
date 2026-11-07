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
static uint64_t complete_io(int64_t request,void *buffer)
{
    uint64_t op=(uint64_t)request&~CS_IO_VALUE_MASK,value=(uint64_t)request&CS_IO_VALUE_MASK;
    if(op==CS_NEED_STORE) {
        assert(value==QS_SORT_IO_BYTES); value=++ntokens;
        tokens=realloc(tokens,(ntokens+1)*sizeof(*tokens)); assert(tokens);
        tokens[value]=malloc(QS_SORT_IO_BYTES); assert(tokens[value]);
        memcpy(tokens[value],buffer,QS_SORT_IO_BYTES); ++live; ++stores;
    } else {
        assert(value && value<=ntokens && tokens[value]);
        if(op==CS_NEED_LOAD) { memcpy(buffer,tokens[value],QS_SORT_IO_BYTES); ++loads; }
        else { assert(op==CS_NEED_FREE); free(tokens[value]); tokens[value]=NULL; --live; }
    }
    return value;
}
static uint64_t complete(int64_t request)
{ return complete_io(request,io); }
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
        uint64_t completion=complete_io(request,s->io); ++count;
        request=qs_dictionary_text_resume(s->text,s->io,completion);
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
    /* Fill the remaining budget before a scalar miss. Its continuation must
     * already exist: cache eviction can reload a page but must not require
     * another allocation just to retain the consumed source row. */
    void *pressure=NULL,*block;
    while((block=cs_work_alloc(s->context,sizeof(void *)))) {
        *(void **)block=pressure; pressure=block;
    }
    assert(pressure);
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
    while(pressure) { void *next=*(void **)pressure; assert(!cs_work_free(s->context,pressure)); pressure=next; }
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
static void drop_cache(struct qs_dictionary *d,struct qs_dictionary_access *a)
{
    uint64_t completion=0;
    for(;;) {
        int64_t rc=qs_dictionary_evict(d,a,io,completion);
        if(rc==-4) break;
        assert(rc>=0 && rc!=QS_DICTIONARY_BUSY);
        completion=rc?complete(rc):0;
    }
}
static unsigned output_batch(struct execution *s,struct qs_dictionary *d,struct qs_dictionary_access *reader)
{
    drop_cache(d,reader);
    struct qs_result *out=calloc(1,sizeof(*out)); assert(out); out->rows=64;
    struct qs_plan root={.nout=6,.nexprs=6};
    for(unsigned col=0;col<root.nout;++col) { root.outputs[col].index=col; root.exprs[col].type=col==2?Q_INTEGER:Q_STRING; }
    uint32_t empty=insert(d,reader,"",0),short_id=insert(d,reader,"tiny",4);
    for(unsigned row=0;row<64;++row) {
        struct qs_cell *v=out->data[row].cells;
        v[0]=(struct qs_cell){.value=ids[row*43%KEYS][0]};
        v[1]=(struct qs_cell){.value=empty}; v[2]=(struct qs_cell){.value=row};
        v[3]=(struct qs_cell){.value=UINT32_MAX,.isnull=1};
        v[4]=(struct qs_cell){.value=short_id}; v[5]=(struct qs_cell){.value=ids[(row*179+3000)%KEYS][1]};
    }
    unsigned waits=0,partial=0; s->output_exported=0;
    /* A namespace read can resume the producer after a partial batch was
     * already encoded. Appending rows must not decode that prefix as IDs. */
    for(unsigned batch=1;batch<=2;++batch) {
        out->rows=batch*32;
        for(unsigned tries=0;export_batch(s,&root,out);++tries) {
            assert(tries<10000 && s->output_active);
            partial+=s->output_position>0; waits+=resume(s);
        }
        assert(s->output_exported==out->rows);
    }
    assert(waits && partial && !s->output_active && out->rows==64 && out->str_bytes==128*LENGTH);
    for(unsigned row=0;row<out->rows;++row) {
        struct qs_cell *v=out->data[row].cells;
        assert(!v[1].isnull && !qs_cell_string(&v[1]) && v[2].value==row && v[3].isnull);
        assert(qs_cell_string(&v[4])==4 && !memcmp(qs_cell_string_bytes(&v[4]),"tiny",4));
        for(unsigned col=0;col<2;++col) {
            char oracle[LENGTH]; make_text(oracle,col?(row*179+3000)%KEYS:row*43%KEYS,col);
            struct qs_cell *text=&v[col?5:0];
            assert(qs_cell_is_string_ref(text) && qs_cell_string(text)==LENGTH);
            assert((unsigned)text->value+LENGTH<=out->str_bytes && !memcmp(out->str_data+(unsigned)text->value,oracle,LENGTH));
        }
    }
    free(out); return waits;
}
static void hash_table(struct execution *s)
{
    struct operator_state *o=&s->nodes[0];
    o->group_capacity=128;
    o->group_stride=(sizeof(struct compact_group)+6*sizeof(struct group_value)+15)&~15U;
    o->groups=cs_work_calloc(s->context,112,o->group_stride);
    o->group_entries=cs_work_calloc(s->context,128,sizeof(*o->group_entries));
    o->group_count=cs_work_calloc(s->context,1,sizeof(*o->group_count));
    assert(o->groups && o->group_entries && o->group_count);
}
static void hash_reset(struct execution *s)
{
    struct operator_state *o=&s->nodes[0];
    memset(o->group_entries,0,128*sizeof(*o->group_entries)); atomic_store(o->group_count,0);
    assert(!s->group_update->active);
}
static unsigned shared_interleave(struct execution *s,struct execution *peer,
    struct qs_dictionary *d,struct qs_dictionary_access *access)
{
    aggregate_plan(s->plan); struct qp_node *n=&s->plan->nodes[0];
    n->hash=1; n->nkeys=n->expr.nkeys=1; n->expr.keys[0]=0; n->expr.naggs=5;
    struct operator_state *o=&s->nodes[0]; struct qs_cell keys[1]={{.value=7}};
    struct group_value seed[QS_AGGS]={0};
    for(unsigned a=0;a<5;++a) seed[a].count=1;
    seed[1].sum.value=10; seed[2].sum.value=ids[KEYS-1][0]; seed[3].sum.value=ids[0][1];
    assert(group_sink_values(s,0,o,keys,seed,GROUP_SHARED)==1);
    unsigned waits=0; uint64_t sum=10;
    for(unsigned round=0;round<8;++round) {
        drop_cache(d,access);
        struct group_value input[QS_AGGS]={0},other[QS_AGGS]={0};
        for(unsigned a=0;a<5;++a) input[a].count=1;
        input[1].sum.value=17+round; input[2].sum.value=ids[100+round*700][0];
        input[3].sum.value=ids[KEYS-1-round*51][1];
        other[0].count=other[1].count=1; other[1].sum.value=31+round;
        int status=group_sink_values(s,0,o,keys,input,GROUP_SHARED);
        assert(status<0 && s->expression_wait && s->group_update->active);
        /* The other CU must be able to commit before the first CU's LOAD
         * completes. No aggregate from the first CU is published yet. */
        assert(!(atomic_load(&s->group_update->entry->state)&GROUP_LOCK));
        assert(group_sink_values(peer,0,o,keys,other,GROUP_SHARED)==1);
        struct compact_group *g=group_at(o,0);
        assert(g->values[0].count==2+2*round && g->values[1].sum.value==sum+31+round);
        for(unsigned attempts=0;;++attempts) {
            assert(attempts<10000); waits+=resume(s);
            status=group_sink_values(s,0,o,keys,input,GROUP_SHARED); if(status==1) break;
            assert(status<0 && s->expression_wait && !(atomic_load(&s->group_update->entry->state)&GROUP_LOCK));
        }
        sum+=48+2*round;
        assert(g->values[0].count==3+2*round && g->values[1].sum.value==sum);
        assert(g->values[2].sum.value==ids[100][0] && g->values[3].sum.value==ids[KEYS-1][1]);
        assert(g->values[4].count==round+2 && !s->group_update->active);
    }
    assert(waits && atomic_load(o->group_count)==1); hash_reset(s); return waits;
}
static unsigned hash_vectors(struct execution *s,struct projection_batch *b)
{
    aggregate_plan(s->plan); struct qp_node *n=&s->plan->nodes[0];
    n->hash=1; n->nkeys=n->expr.nkeys=1; n->expr.keys[0]=0; n->expr.naggs=5;
    struct operator_state *o=&s->nodes[0]; b->plan=n->expr; b->plan.nout=3;
    for(unsigned col=0;col<3;++col) b->plan.outputs[col].index=col;
    b->group_keys[0]=0;
    for(unsigned a=0;a<5;++a) b->group_values[a]=n->expr.aggs[a].expr;
    o->projection=b; s->owner=s;
    struct qs_result *out=calloc(1,sizeof(*out)); assert(out);
    unsigned waits=0,min=KEYS,max=0,left_count=0,right_count=0;
    for(unsigned batch=0;batch<3;++batch) {
        b->position=0; b->count=128;
        for(unsigned col=0;col<3;++col) { memset(&b->vectors[col],0,sizeof(b->vectors[col])); b->vectors[col].kind=QS_NARROW; }
        for(unsigned row=0;row<128;++row) {
            unsigned at=batch*128+row,left=at*173%KEYS,right=(at*107+4000)%KEYS;
            b->vectors[0].narrow[row]=7;
            b->vectors[1].narrow[row]=ids[left][0]; b->vectors[1].nulls[row]=at%13==0;
            b->vectors[2].narrow[row]=ids[right][1]; b->vectors[2].nulls[row]=at%11==0;
            if(at%13) { ++left_count; if(left<min) min=left; }
            if(at%11) { ++right_count; if(right>max) max=right; }
        }
        for(unsigned attempts=0;;++attempts) {
            assert(attempts<10000); int status=group_sink_vectors(s,0,out,QS_RESULT_ROWS);
            if(!status) break;
            assert(status<0 && s->expression_wait); waits+=resume(s);
        }
        assert(b->position==128 && b->group_input && !b->group_input->phase && !out->rows);
        struct compact_group *g=group_at(o,0);
        assert(atomic_load(o->group_count)==1 && g->values[0].count==128*(batch+1));
        assert(g->values[1].sum.value==7*128*(batch+1) && g->values[4].count==left_count);
        assert(g->values[2].count==left_count && g->values[3].count==right_count);
        assert(g->values[2].sum.value==ids[min][0] && g->values[3].sum.value==ids[max][1]);
    }
    assert(waits); free(out); o->projection=NULL; hash_reset(s); return waits;
}
static unsigned partition_combine(struct execution *s,struct execution *peer,
    struct qs_dictionary *d,struct qs_dictionary_access *access)
{
    drop_cache(d,access); aggregate_plan(s->plan);
    struct qp_node *n=&s->plan->nodes[0]; n->hash=1; n->nkeys=n->expr.nkeys=1;
    n->expr.keys[0]=0; n->expr.naggs=5; s->plan->workers=2;
    struct operator_state *global=&s->nodes[0];
    struct operator_state *local=cs_work_calloc(s->context,1,sizeof(*local)); assert(local);
    local->groups=cs_work_calloc(s->context,10,global->group_stride);
    local->group_stride=global->group_stride;
    local->group_count=cs_work_calloc(s->context,1,sizeof(*local->group_count));
    global->group_locals=cs_work_calloc(s->context,1,sizeof(*global->group_locals));
    peer->nodes=cs_work_calloc(s->context,1,sizeof(*peer->nodes));
    assert(local->groups && local->group_count && global->group_locals && peer->nodes);
    global->group_locals[0]=local; global->group_local_count=1; atomic_store(local->group_count,10);
    for(unsigned row=0;row<10;++row) {
        struct qs_cell key={.value=row+1}; struct group_value seed[QS_AGGS]={0};
        for(unsigned a=0;a<5;++a) seed[a].count=1;
        seed[1].sum.value=10; seed[2].sum.value=ids[KEYS-1][0]; seed[3].sum.value=ids[0][1];
        assert(group_sink_values(s,0,global,&key,seed,GROUP_SHARED)==1);
        struct compact_group *g=group_at(local,row); g->key=row+1; g->key_flags=1;
        for(unsigned a=0;a<5;++a) g->values[a].count=2;
        g->values[1].sum.value=17+row;
        g->values[2].sum.value=ids[1000+row*509][0]; g->values[3].sum.value=ids[2000+row*571][1];
    }
    peer->worker=1; peer->owner=s; peer->group_shard=true; peer->group_combine=1;
    unsigned waits=0,owned=0;
    for(unsigned attempts=0;;++attempts) {
        assert(attempts<10000); int64_t rc=group_combine_local(peer); if(!rc) break;
        assert(rc<0 && peer->expression_wait); waits+=resume(peer);
    }
    for(unsigned row=0;row<10;++row) {
        int owns=!(mix_hash(row+1)&1); owned+=owns;
        const struct compact_group *g=group_at(global,row);
        assert(g->values[0].count==(owns?3U:1U) && g->values[1].sum.value==(owns?27+row:10));
        assert(g->values[2].sum.value==ids[owns?1000+row*509:KEYS-1][0]);
        assert(g->values[3].sum.value==ids[owns?2000+row*571:0][1]);
    }
    assert(owned>=2 && waits && peer->nodes[0].group_local_slot==1 && !peer->nodes[0].group_local_row);
    cs_work_free(s->context,peer->nodes); peer->nodes=NULL;
    cs_work_free(s->context,local->groups); cs_work_free(s->context,local->group_count); cs_work_free(s->context,local);
    cs_work_free(s->context,global->group_locals); global->group_locals=NULL; global->group_local_count=0;
    peer->worker=peer->group_combine=0; peer->group_shard=false; peer->owner=NULL; s->plan->workers=0;
    hash_reset(s); return waits;
}
static unsigned hash_scalar(struct execution *s,struct qs_dictionary *d,struct qs_dictionary_access *access)
{
    aggregate_plan(s->plan); struct qp_node *n=&s->plan->nodes[0];
    n->hash=1; n->nkeys=n->expr.nkeys=1; n->expr.keys[0]=0;
    struct operator_state *o=&s->nodes[0];
    unsigned waits=0,min=KEYS,max=0;
    for(unsigned row=0;row<32;++row) {
        unsigned a=(row*701+1700)%KEYS,b=(row*239+4000)%KEYS;
        struct qs_cell input[3]={{.value=7},{.value=ids[a][0]},{.value=ids[b][1]}};
        if(a<min) min=a;
        if(b>max) max=b;
        for(unsigned attempts=0;;++attempts) {
            assert(attempts<10000); int status=group_sink_row(s,0,o,input,GROUP_SHARED);
            if(status==1) break;
            assert(status<0 && s->expression_wait); waits+=resume(s);
        }
        struct compact_group *g=group_at(o,0);
        assert(g->values[0].count==row+1 && g->values[1].sum.value==7*(row+1));
        assert(g->values[2].sum.value==ids[min][0] && g->values[3].sum.value==ids[max][1]);
    }
    char bytes[LENGTH]; make_text(bytes,min,0);
    check_string(d,access,group_at(o,0)->values[5].sum.value,bytes+2,6);
    assert(waits); return waits;
}
static unsigned spill_partial_merge(struct execution *s,struct qs_dictionary *d,struct qs_dictionary_access *access)
{
    aggregate_plan(s->plan); struct qp_node *n=&s->plan->nodes[0];
    n->hash=1; n->nkeys=n->expr.nkeys=1; n->expr.keys[0]=0; n->expr.naggs=5;
    struct operator_state *o=&s->nodes[0];
    drop_cache(d,access);
    /* Model a Sort Source's already decoded partial rows, using its actual
     * workmem result buffer. The dictionary reads still complete via NVM. */
    struct sorted_rows *sort=&o->left_sort;
    sort->output=cs_work_calloc(s->context,1,sizeof(*sort->output)); assert(sort->output);
    sort->output->rows=8; sort->drained=1; sort->position=0;
    o->group_phase=2; o->phase=o->have_right=o->group_ready=o->finished=0;
    unsigned min=KEYS,max=0,waits=0;
    for(unsigned row=0;row<8;++row) {
        unsigned a=(row*701+1000)%KEYS,b=(row*997+500)%KEYS;
        if(a<min) min=a;
        if(b>max) max=b;
        struct qs_cell *values=sort->output->data[row].cells; values[0].value=7;
        for(unsigned agg=0;agg<5;++agg) values[2*agg+2].value=2;
        values[3].value=row+1; values[5].value=ids[a][0]; values[7].value=ids[b][1];
    }
    struct qs_cell result[QS_OUT];
    for(unsigned attempts=0;;++attempts) {
        assert(attempts<10000);
        int64_t rc=hash_aggregate_next_impl(s,0,result,0,1);
        if(rc==2) break;
        assert(rc<0 && s->expression_wait); waits+=resume(s);
    }
    assert(result[0].value==7 && result[1].value==16 && result[2].value==36 &&
        result[3].value==ids[min][0] && result[4].value==ids[max][1] && result[5].value==16);
    assert(!o->aggregate_position && hash_aggregate_next_impl(s,0,result,0,1)==0 && !sort->output);
    assert(waits); return waits;
}
static const unsigned join_left_keys[8][3]={
    {1000,100,10},{1000,100,5000},{1000,200,2000},{2000,100,6000},
    {2000,200,3000},{3000,100,100},{4000,100,7000},{0,100,10}};
static const unsigned join_right_keys[8][3]={
    {1000,100,10},{1000,100,10},{1000,200,2000},{1500,100,123},
    {2000,100,6000},{2000,100,7000},{4000,100,7001},{0,100,10}};
static void join_plan(struct qp_plan *p,unsigned kind)
{
    memset(p->nodes,0,3*sizeof(*p->nodes)); p->count=3; p->workers=0;
    for(unsigned id=1;id<=2;++id) {
        struct qp_node *n=&p->nodes[id];
        /* These materialized inputs are already complete; left supplies
         * column metadata only, so no input production runs in this test. */
        n->kind=QP_MATERIALIZED; n->left=id; n->expr.nout=n->expr.nattrs=n->expr.nexprs=4;
        n->expr.predicate=-1;
        for(unsigned i=0;i<4;++i) {
            n->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.column=i,.type=i<3?Q_STRING:Q_INTEGER};
            n->expr.outputs[i].index=i; n->expr.outputs[i].type_oid=i<3?25:23;
            n->expr.cols[i]=(struct qs_column){.slot=i,.length=i<3?-1:4,.align=4,.type=i<3?Q_STRING:Q_INTEGER};
        }
    }
    struct qp_node *n=&p->nodes[0]; n->kind=QP_JOIN; n->left=1; n->right=2;
    n->join_type=kind; n->nkeys=2; n->left_keys[0]=n->right_keys[0]=0; n->left_keys[1]=n->right_keys[1]=1;
    n->expr.nattrs=8; n->expr.nexprs=10; n->expr.nstrings=1; n->expr.norder=2;
    n->expr.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.type=Q_STRING,.column=2};
    n->expr.exprs[1]=(struct qs_expr){.op=Q_COLUMN,.type=Q_STRING,.column=6};
    n->expr.exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=2};
    n->expr.exprs[3]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=6};
    n->expr.exprs[4]=(struct qs_expr){.op=QP_SUBSTRING,.type=Q_STRING,.left=0,.right=2,.otherwise=3};
    n->expr.exprs[5]=(struct qs_expr){.op=QP_SUBSTRING,.type=Q_STRING,.left=1,.right=2,.otherwise=3};
    n->expr.exprs[6]=(struct qs_expr){.op=Q_EQ,.type=Q_BOOLEAN,.left=4,.right=5};
    n->expr.exprs[7]=(struct qs_expr){.op=Q_CONST,.type=Q_STRING,.constant=0};
    n->expr.strings[0].length=5; memcpy(n->expr.strings[0].bytes,"%-a-%",5);
    n->expr.exprs[8]=(struct qs_expr){.op=QP_LIKE,.type=Q_BOOLEAN,.left=0,.right=7};
    n->expr.exprs[9]=(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=8,.right=6}; n->join_qual=9;
}
static unsigned join_rows(struct execution *s,struct qs_dictionary *d,struct qs_dictionary_access *access)
{
    unsigned kinds[]={QP_INNER,QP_LEFT,QP_SEMI,QP_ANTI};
    unsigned char *tails[3]={s->nodes[0].group.tail,s->nodes[1].group.tail,s->nodes[2].group.tail};
    struct join_condition_state *condition=s->nodes[0].join_condition;
    unsigned char *table=s->nodes[0].hash_data;
    unsigned waits=0,key_waits=0,condition_waits=0;
    struct qp_control control={0}; s->control=&control;
    for(unsigned hash=0;hash<2;++hash) for(unsigned mode=0;mode<4;++mode) {
        drop_cache(d,access); join_plan(s->plan,kinds[mode]);
        memset(s->nodes,0,3*sizeof(*s->nodes));
        struct operator_state *o=&s->nodes[0];
        o->join_condition=condition; assert(!condition->active);
        o->group.tail=tails[0]; o->group.loaded=UINT_MAX;
        o->merge_ordered=7; o->merge_qual_checked=o->hash_qual_checked=1;
        for(unsigned side=1;side<=2;++side) {
            struct operator_state *input=&s->nodes[side];
            input->group_ready=1; input->group.tail=tails[side]; input->group.rows=input->group.used=8;
            for(unsigned row=0;row<8;++row) {
                const unsigned *keys=side==1?join_left_keys[row]:join_right_keys[row];
                struct qs_cell values[4]={{.value=ids[keys[0]][0],.isnull=row==7},
                    {.value=ids[keys[1]][0]},{.value=ids[keys[2]][side-1]},{.value=row}};
                memcpy(input->group.tail+row*sizeof(values),values,sizeof(values));
            }
        }
        struct qp_node *n=&s->plan->nodes[0];
        if(hash) {
            n->hash=1; o->hash_phase=1; o->hash_shared=1; o->hash_capacity=8; o->hash_buckets=16; o->hash_data=table;
            unsigned stride=hash_row_bytes(&s->plan->nodes[2].expr);
            uint32_t *next=(void *)(table+8*stride),*buckets=next+8;
            for(unsigned i=0;i<16;++i) buckets[i]=UINT_MAX;
            for(unsigned row=0;row<8;++row) {
                struct qs_cell *values=(void *)(tails[2]+row*4*sizeof(*values));
                hash_store(table+row*stride,values,&s->plan->nodes[2].expr);
                unsigned slot=row_hash(n,values,1)&15; next[row]=buckets[slot]; buckets[slot]=row;
            }
            /* No dictionary I/O is needed to accept/reject equal-key hash
             * candidates, even when their string bodies were just evicted. */
            struct qs_cell *left=(void *)tails[1],*right=(void *)tails[2];
            unsigned before=loads; assert(keys_equal(s,n,left,right)==1 && loads==before);
        }
        unsigned seen[8][9]={0};
        for(unsigned attempts=0;;++attempts) {
            assert(attempts<10000); struct qs_cell values[QS_COLS];
            int64_t status=hash?hash_match_next(s,0,0):merge_match_next(s,0,values);
            if(!status) break;
            if(status<0) {
                assert(s->expression_wait);
                key_waits+=o->merge_key>0; condition_waits+=condition->active;
                waits+=resume(s); continue;
            }
            assert(status==2);
            if(hash) {
                memcpy(values,o->left,4*sizeof(*values));
                if(o->hash_null_right) for(unsigned i=4;i<8;++i) values[i]=(struct qs_cell){.isnull=1};
                else memcpy(values+4,o->right,4*sizeof(*values));
            }
            unsigned l=values[3].value,r=values[7].isnull?8:(unsigned)values[7].value;
            assert(l<8 && r<=8); ++seen[l][r];
        }
        for(unsigned l=0;l<8;++l) {
            unsigned matches=0,actual=0;
            for(unsigned r=0;r<8;++r) {
                int match=l!=7 && r!=7 && join_left_keys[l][0]==join_right_keys[r][0] &&
                    join_left_keys[l][1]==join_right_keys[r][1] && join_left_keys[l][2]==join_right_keys[r][2];
                matches+=match; actual+=seen[l][r];
                if(kinds[mode]==QP_INNER || kinds[mode]==QP_LEFT) assert(seen[l][r]==(unsigned)match);
                else assert(!seen[l][r] || (kinds[mode]==QP_SEMI && match && seen[l][r]==1));
            }
            if(kinds[mode]==QP_SEMI) assert(actual==(matches!=0) && !seen[l][8]);
            else if(kinds[mode]==QP_ANTI) assert(!actual && seen[l][8]==(matches==0));
            else assert(seen[l][8]==(kinds[mode]==QP_LEFT && !matches));
        }
        assert(s->nodes[1].position==8 && !condition->active && !o->merge_key);
    }
    s->control=NULL;
    assert(waits && key_waits && condition_waits);
    printf("join dictionary key waits=%u condition waits=%u\n",key_waits,condition_waits);
    return waits;
}
static unsigned index_condition_rows(struct execution *s,struct qs_dictionary *d,struct qs_dictionary_access *access)
{
    drop_cache(d,access); join_plan(s->plan,QP_INNER);
    struct operator_state *o=&s->nodes[0]; struct qp_node *n=&s->plan->nodes[0];
    n->kind=QP_INDEX_JOIN; n->expr.nout=8; n->expr.nexprs=20; n->expr.predicate=-1;
    for(unsigned col=0;col<8;++col) {
        n->expr.exprs[10+col]=(struct qs_expr){.op=Q_COLUMN,.type=col%4<3?Q_STRING:Q_INTEGER,.column=col};
        n->expr.outputs[col].index=10+col;
    }
    n->expr.exprs[18]=(struct qs_expr){.op=Q_COLUMN,.type=Q_STRING,.column=0};
    n->expr.exprs[19]=(struct qs_expr){.op=QP_SUBSTRING,.type=Q_STRING,.left=18,.right=2,.otherwise=3};
    n->expr.outputs[2].index=19;
    o->index_left=(void *)s->nodes[1].group.tail; o->index_count=8;
    o->index_output=cs_work_calloc(s->context,1,sizeof(*o->index_output)); assert(o->index_output);
    o->index_active=o->index_flush=o->index_consume_position=0;
    struct qs_chunk *chunk=calloc(1,qs_chunk_bytes(64,6)); assert(chunk); chunk->capacity=64; chunk->ncols=6;
    for(unsigned l=0;l<7;++l) for(unsigned r=0;r<7;++r) {
        if(join_left_keys[l][0]!=join_right_keys[r][0] || join_left_keys[l][1]!=join_right_keys[r][1]) continue;
        unsigned at=chunk->rows++; qs_selection(chunk)[at]=at; qs_values(chunk)[chunk->capacity+at]=l;
        struct qs_cell *right=(void *)(s->nodes[2].group.tail+r*4*sizeof(*right));
        for(unsigned col=0;col<4;++col) qs_values(chunk)[(size_t)(col+2)*chunk->capacity+at]=right[col].value;
    }
    chunk->selected=chunk->rows; index_execution=s; index_node=0; index_columns=NULL;
    unsigned waits=0,projection_waits=0;
    for(unsigned attempts=0;;++attempts) {
        assert(attempts<10000); int64_t rc=qs_join_consume(0,chunk,NULL,NULL,NULL,NULL,0);
        if(rc==QP_INDEX_YIELD) break;
        assert(rc<0 && s->expression_wait); projection_waits+=o->project_retry!=NULL; waits+=resume(s);
    }
    unsigned seen[8][8]={0};
    for(unsigned i=0;i<o->index_output->rows;++i) {
        const struct qs_cell *row=o->index_output->data[i].cells;
        unsigned l=row[3].value,r=row[7].value; assert(l<8 && r<8); ++seen[l][r];
        char bytes[LENGTH]; make_text(bytes,join_left_keys[l][0],0);
        check_string(d,access,row[2].value,bytes+2,6);
    }
    for(unsigned l=0;l<8;++l) for(unsigned r=0;r<8;++r) {
        int match=l!=7 && r!=7 && join_left_keys[l][0]==join_right_keys[r][0] &&
            join_left_keys[l][1]==join_right_keys[r][1] && join_left_keys[l][2]==join_right_keys[r][2];
        assert(seen[l][r]==(unsigned)match);
    }
    assert(waits && projection_waits && !o->join_condition->active && qs_join_consume(0,chunk,NULL,NULL,NULL,NULL,0)==0);
    index_execution=NULL; cs_work_free(s->context,o->index_output); o->index_output=NULL; free(chunk);
    printf("index dictionary projection waits=%u\n",projection_waits);
    return waits;
}
static void pipeline_plan(struct execution *s,struct projection_batch *b,
    struct qs_dictionary *d,struct qs_dictionary_access *access)
{
    plan(s->plan); b->plan=s->plan->nodes[0].expr;
    b->plan.exprs[7].constant=insert(d,access,"%-a-%",5);
    b->plan.exprs[12].constant=insert(d,access,"fallback",8);
    /* Native import orders children before parents. The scalar fixture has
     * a forward column reference; put it before COALESCE in this vector DAG. */
    struct qs_expr column=b->plan.exprs[17];
    memmove(b->plan.exprs+13,b->plan.exprs+12,5*sizeof(*b->plan.exprs));
    b->plan.exprs[12]=column; b->plan.exprs[14].left=12; b->plan.exprs[14].right=13;
    b->plan.exprs[17].left=16; b->plan.exprs[17].right=15;
    for(unsigned o=0;o<b->plan.nout;++o) if(b->plan.outputs[o].index>=12) ++b->plan.outputs[o].index;
    for(unsigned col=0;col<4;++col) b->plan.cols[col]=(struct qs_column){.slot=col,.type=col==2?Q_INTEGER:Q_STRING};
    for(unsigned o=0;o<b->plan.nout;++o) assert(!qs_vector_require(&b->plan,b->plan.outputs[o].index,b->needed));
    assert(!qs_vector_require(&b->plan,8,b->needed) && !qs_vector_require(&b->plan,6,b->needed));
    b->filters[0].expression=8; b->filters[1].expression=6; b->nfilters=2;
    s->nodes[0].projection=b; s->paged=d;
}
static void pipeline_input(struct projection_batch *b,unsigned batch,unsigned count)
{
    b->count=count; b->position=0;
    for(unsigned row=0;row<count;++row) {
        unsigned at=batch*64+row,left=at*193%KEYS,right=at%3?left:(at*97+3000)%KEYS;
        b->cells[row]=(struct qs_cell){.value=ids[left][0]};
        b->cells[QS_VECTOR_ROWS+row]=(struct qs_cell){.value=ids[right][1],.isnull=at%13==0};
        b->cells[2*QS_VECTOR_ROWS+row]=(struct qs_cell){.value=at};
        b->cells[3*QS_VECTOR_ROWS+row]=(struct qs_cell){.value=ids[left][0],.isnull=at%5==0};
    }
}
static unsigned pipeline_columns(struct execution *s,struct projection_batch *b,
    struct qs_dictionary *d,struct qs_dictionary_access *access,unsigned *materialize_waits)
{
    pipeline_plan(s,b,d,access);
    unsigned waits=0; *materialize_waits=0;
    for(unsigned batch=0;batch<3;++batch) {
        drop_cache(d,access); pipeline_input(b,batch,64);
        struct qs_cell original[4*QS_VECTOR_ROWS]; memcpy(original,b->cells,sizeof(original));
        struct qs_vector_input input={.cells=b->cells,.set_state=s};
        for(unsigned attempts=0;;++attempts) {
            assert(attempts<10000); int status=projection_execute(b,NULL,NULL,0,input); if(!status) break;
            assert(status<0 && s->expression_wait && b->retry && !b->materializing); waits+=resume(s);
        }
        unsigned expected[64],count=0;
        for(unsigned row=0;row<64;++row) { unsigned at=batch*64+row; if(at%3 && at%13) expected[count++]=at; }
        assert(b->count==count && !b->retry);
        int status=materialize_outputs(s,b);
        for(unsigned attempts=0;status;++attempts) {
            assert(attempts<10000 && status<0 && b->materializing && !b->retry);
            *materialize_waits+=resume(s);
            status=projection_fill(s,0,0); if(status==2) status=0;
        }
        assert(!b->materializing && !b->materialized_column && b->count==count);
        for(unsigned row=0;row<count;++row) {
            unsigned at=expected[row],left=at*193%KEYS;
            struct qs_cell value=qs_vector_cell(&b->plan,b->vectors,11,row); assert(value.value==at);
            char bytes[LENGTH]; make_text(bytes,left,0);
            value=qs_vector_cell(&b->plan,b->vectors,4,row); check_string(d,access,value.value,bytes+2,6);
            struct qs_cell same=qs_vector_cell(&b->plan,b->vectors,10,row); assert(same.value==value.value);
            value=qs_vector_cell(&b->plan,b->vectors,14,row);
            check_string(d,access,value.value,at%5?bytes:"fallback",at%5?LENGTH:8);
            value=qs_vector_cell(&b->plan,b->vectors,8,row); assert(!value.isnull && value.value);
            value=qs_vector_cell(&b->plan,b->vectors,15,row); assert(!value.isnull && value.value);
            assert(qs_vector_cell(&b->plan,b->vectors,0,row).value==ids[left][0]);
            assert(qs_vector_cell(&b->plan,b->vectors,1,row).value==ids[left][1]);
        }
        assert(!memcmp(original,b->cells,sizeof(original)));
    }
    assert(waits && *materialize_waits);
    cs_work_context_stats before,after;
    assert(!cs_work_get_context_stats(s->context,&before));
    projection_clear(s,&s->nodes[0].projection);
    assert(!cs_work_get_context_stats(s->context,&after) && after.allocations+4==before.allocations);
    return waits;
}
/* Abort after I/O completion but before its token is acknowledged. A query
 * scope also owns this token, so it must reclaim it without needing a retry. */
static void cancel_operation(uint64_t operation,int exporting)
{
    cs_work_context *ctx=cs_work_begin_limit((exporting>=6?768U:384U)*1024); assert(ctx);
    struct qs_dictionary *d=qs_dictionary_create(ctx); assert(d);
    struct qs_dictionary_access *access=qs_dictionary_access_create(d); assert(access);
    struct execution *s=cs_work_calloc(ctx,1,sizeof(*s)); assert(s);
    s->context=ctx; s->io=io; s->text=qs_dictionary_text_create(d); assert(s->text);
    s->plan=calloc(1,sizeof(*s->plan)); assert(s->plan); plan(s->plan);
    s->nodes=cs_work_calloc(ctx,1,sizeof(*s->nodes)); assert(s->nodes);
    struct projection_batch *batch=NULL;
    if(exporting>=6) {
        s->paged=d; batch=cs_work_calloc(ctx,1,sizeof(*batch)); assert(batch);
        batch->vectors=cs_work_calloc(ctx,3,sizeof(*batch->vectors)); assert(batch->vectors);
        batch->count=1; batch->plan.nout=1;
        if(exporting==6) {
            batch->plan.nattrs=batch->plan.ncols=1; batch->plan.nexprs=3;
            batch->plan.cols[0]=(struct qs_column){.slot=0,.type=Q_STRING};
            batch->plan.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.type=Q_STRING,.column=0};
            batch->plan.exprs[1]=(struct qs_expr){.op=Q_CONST,.type=Q_STRING};
            batch->plan.exprs[2]=(struct qs_expr){.op=QP_LIKE,.type=Q_BOOLEAN,.left=0,.right=1};
            batch->plan.outputs[0].index=2;
            assert(!qs_vector_require(&batch->plan,2,batch->needed));
            batch->cells=cs_work_calloc(ctx,QS_VECTOR_ROWS,sizeof(*batch->cells)); assert(batch->cells);
            batch->cells[0].value=1;
        } else {
            batch->plan.nexprs=1; batch->plan.exprs[0].type=Q_STRING;
            batch->vectors[0].kind=QS_TEXT;
            batch->vectors[0].strings[0]=(struct qs_text_view){.id=1,.offset=2,.length=6};
        }
    }
    if(exporting==4) {
        s->plan->nodes[0].join_qual=9;
        s->nodes[0].join_condition=cs_work_calloc(ctx,1,sizeof(*s->nodes[0].join_condition)); assert(s->nodes[0].join_condition);
    }
    if(exporting==5) {
        struct qp_node *n=&s->plan->nodes[0]; n->left=1; n->right=2; n->nkeys=2;
        for(unsigned i=0;i<2;++i) {
            n->left_keys[i]=n->right_keys[i]=i;
            s->plan->nodes[1].expr.outputs[i].index=i; s->plan->nodes[1].expr.exprs[i].type=Q_STRING;
        }
    }
    if(exporting==3) {
        aggregate_plan(s->plan); s->plan->nodes[0].expr.naggs=5;
        s->plan->nodes[0].nkeys=s->plan->nodes[0].expr.nkeys=1;
        hash_table(s);
        s->group_update=cs_work_calloc(ctx,1,sizeof(*s->group_update)); assert(s->group_update);
        assert(aggregate_input(s,&s->nodes[0]));
    }
    for(unsigned row=0;row<4096;++row) {
        char bytes[LENGTH]; make_text(bytes,row,0); assert(insert(d,access,bytes,LENGTH)==row+1);
    }
    if(exporting==6) batch->plan.exprs[1].constant=insert(d,access,"%-a-%",5);
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
    if(exporting==3) {
        struct group_value seed[QS_AGGS]={0};
        for(unsigned a=0;a<5;++a) seed[a].count=1;
        seed[1].sum.value=7; seed[2].sum.value=4096; seed[3].sum.value=1;
        struct qs_cell keys[1]={{.value=7}};
        assert(group_sink_values(s,0,&s->nodes[0],keys,seed,GROUP_SHARED)==1);
        input[0]=(struct qs_cell){.value=7}; input[1]=(struct qs_cell){.value=1}; input[2]=(struct qs_cell){.value=4096};
    }
    int cancelled=0;
    for(unsigned attempts=0;attempts<10000 && !cancelled;++attempts) {
        if(exporting==5) {
            s->nodes[0].left[0]=(struct qs_cell){.value=1}; s->nodes[0].left[1]=(struct qs_cell){.value=1000};
            s->nodes[0].right[0]=(struct qs_cell){.value=4096}; s->nodes[0].right[1]=(struct qs_cell){.value=2000};
        }
        struct qs_vector_input vector_input={.cells=batch?batch->cells:NULL,.set_state=s};
        int status=exporting==7?materialize_outputs(s,batch):
            exporting==6?projection_execute(batch,NULL,NULL,0,vector_input):
            exporting==5?key_compare(s,&s->plan->nodes[0],s->nodes[0].left,s->nodes[0].right):
            exporting==4?join_condition(s,0,input,&result[0]):
            exporting==3?group_sink_row(s,0,&s->nodes[0],input,GROUP_SHARED):
            exporting==2?accumulate(s,&s->plan->nodes[0],&s->nodes[0]):
            exporting?export_row(s,&root,out->data[0].cells,out):
            project(s,&s->plan->nodes[0],input,NULL,result);
        assert(status<0);
        if(!operation) { assert(!s->expression_wait); cancelled=1; break; }
        assert(s->text_wait>QS_DICTIONARY_BUSY && s->expression_wait);
        if(exporting==3) assert(s->group_update->active && !(atomic_load(&s->group_update->entry->state)&GROUP_LOCK));
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
/* Reproduce lazy Join payload insertion yielding to another CU. The match
 * is already consumed when materialization waits; neither adapter may select
 * another pair before consuming the insertion completion. */
static void join_payload_resume(void)
{
    const size_t budget=64U*1024*1024;
    assert(!cs_work_init(budget));
    for(unsigned scalar=0;scalar<2;++scalar) {
        cs_work_context *ctx=cs_work_begin_limit(budget); assert(ctx);
        struct qs_dictionary *d=qs_dictionary_create(ctx); assert(d);
        struct qs_dictionary_access *peer=qs_dictionary_access_create(d); assert(peer);
        struct execution *s=cs_work_calloc(ctx,1,sizeof(*s)); assert(s);
        struct qp_control control={0}; s->control=&control;
        s->context=ctx; s->paged=d; s->io=io; s->text=qs_dictionary_text_create(d); assert(s->text);
        s->plan=calloc(1,sizeof(*s->plan)); assert(s->plan); s->plan->count=3;
        s->nodes=cs_work_calloc(ctx,3,sizeof(*s->nodes)); assert(s->nodes);
        struct qp_node *n=&s->plan->nodes[0];
        *n=(struct qp_node){.kind=QP_JOIN,.hash=1,.left=1,.right=2,.nkeys=1,.join_qual=-1};
        n->expr=(struct qs_plan){.predicate=-1,.nattrs=3,.ncols=3,.nexprs=3,.nout=3};
        for(unsigned col=0;col<3;++col) {
            n->expr.cols[col]=(struct qs_column){.slot=col,.type=col==1?Q_STRING:Q_INTEGER};
            n->expr.exprs[col]=(struct qs_expr){.op=Q_COLUMN,.column=col,.type=col==1?Q_STRING:Q_INTEGER};
            n->expr.outputs[col]=(struct qs_output){.index=col,.type_oid=col==1?25:23};
        }
        s->plan->nodes[1].expr.nout=2;
        s->plan->nodes[2].expr.nout=1; s->plan->nodes[2].expr.outputs[0].type_oid=23;
        struct projection_batch *left=cs_work_calloc(ctx,1,sizeof(*left)); assert(left);
        left->plan=n->expr; left->plan.nout=2; left->count=left->position=1;
        left->vectors=cs_work_calloc(ctx,3,sizeof(*left->vectors)); assert(left->vectors);
        const char payload[]="retained lazy join payload";
        left->vectors[1].kind=QS_TEXT;
        left->vectors[1].strings[0]=(struct qs_text_view){.data=payload,.length=sizeof(payload)-1};
        s->nodes[1].projection=left;
        struct operator_state *o=&s->nodes[0];
        o->hash_phase=o->have_left=o->hash_match=1; o->left[0].value=42; o->hash_payload=1U<<1;
        o->hash_capacity=128; o->hash_buckets=256;
        o->hash_data=cs_work_calloc(ctx,128,5+12); assert(o->hash_data);
        struct qs_cell right={.value=42}; hash_store(o->hash_data,&right,&s->plan->nodes[2].expr);
        uint32_t *next=(void *)(o->hash_data+128*5); next[0]=UINT_MAX;
        struct projection_batch *b=cs_work_calloc(ctx,1,sizeof(*b)); assert(b);
        b->plan=n->expr; b->cells=cs_work_calloc(ctx,3*QS_VECTOR_ROWS,sizeof(*b->cells));
        b->vectors=cs_work_calloc(ctx,3,sizeof(*b->vectors)); assert(b->cells && b->vectors);
        for(unsigned col=0;col<3;++col) assert(!qs_vector_require(&b->plan,col,b->needed));
        assert(insert(d,peer,"seed",4));
        int64_t request=qs_dictionary_evict(d,peer,io,0); assert(request>QS_DICTIONARY_BUSY);
        unsigned char peer_io[QS_SORT_IO_BYTES]; memcpy(peer_io,io,sizeof(peer_io));
        struct qs_cell row[QS_OUT];
        int64_t status=scalar?hash_next(s,0,row):projection_join_fill(s,b);
        assert(status<0 && s->expression_wait && s->text_wait==QS_DICTIONARY_BUSY);
        assert(!b->error && !o->hash_match);
        if(scalar) assert(o->hash_output_pending);
        else assert(b->joined_row && b->joined_column==1 && !b->input_count);
        do {
            uint64_t completion=complete_io(request,peer_io);
            request=qs_dictionary_evict(d,peer,peer_io,completion);
        } while(request>QS_DICTIONARY_BUSY);
        assert(!request);
        request=qs_dictionary_text_resume(s->text,io,0);
        while(request) {
            assert(request>QS_DICTIONARY_BUSY);
            request=qs_dictionary_text_resume(s->text,io,complete(request));
        }
        s->text_wait=0; s->expression_wait=0;
        o->finished=1; /* No more matches, but the retained one must survive. */
        status=scalar?hash_next(s,0,row):projection_join_fill(s,b); assert(status==2);
        if(!scalar) {
            assert(b->count==1 && !b->joined_row && !b->joined_column);
            for(unsigned col=0;col<3;++col) row[col]=qs_vector_cell(&b->plan,b->vectors,col,0);
        } else assert(!o->hash_output_pending);
        assert(row[0].value==42 && row[2].value==42 && !row[1].isnull);
        check_string(d,peer,(uint32_t)row[1].value,payload,sizeof(payload)-1);
        assert(stores && loads);
        assert(!qs_dictionary_text_release(s->text));
        request=qs_dictionary_close(d,peer,io,0);
        while(request) { assert(request>QS_DICTIONARY_BUSY); request=qs_dictionary_close(d,peer,io,complete(request)); }
        free(s->plan); assert(!cs_work_end(ctx) && !live);
        free(tokens); tokens=NULL; ntokens=stores=loads=0;
        cs_work_stats total; assert(!cs_work_get_stats(&total) && !total.used_bytes && !total.active_contexts);
    }
    puts("PASS 64 MiB vector/scalar Join payload resumes its selected pair after dictionary wait");
}
static void signed_probe_slots(void)
{
    int64_t input[]={INT64_MIN,-1,0,1,INT64_MAX,7,8};
    struct projection_batch batch={.count=7};
    struct qs_vector *keys=calloc(2,sizeof(*keys)); assert(keys);
    batch.vectors=keys; batch.plan.exprs[0].type=Q_INTEGER;
    batch.plan.outputs[1].index=1;
    keys[0].kind=QS_NARROW;
    memcpy(keys[0].narrow,input,sizeof(input));
    keys[0].nulls[5]=1; keys[1].errors[6]=1;
    struct qp_node join={.nkeys=1};
    struct hash_probe_batch probe;
    assert(hash_narrow_probe_slots(&join,&batch,256,3,&probe));
    for(unsigned i=0;i<5;++i) {
        struct qs_cell row={.value=input[i]};
        assert(probe.matches[i]==(row_hash(&join,&row,0)&255));
    }
    assert(probe.matches[5]==UINT_MAX && !probe.errors[5]);
    assert(probe.matches[6]==UINT_MAX && probe.errors[6]);
    free(keys);
}
/* Hash-cache collisions, signed sentinels and failed insertion must never
 * manufacture a successful existence match. Use the same bounded allocator. */
static void existence_cache(void)
{
    const size_t limit=64U*1024*1024;
    assert(!cs_work_init(limit));
    cs_work_context *context=cs_work_begin_limit(limit); assert(context);
    struct execution s={.context=context};
    s.plan=calloc(1,sizeof(*s.plan)); assert(s.plan); s.plan->count=2;
    s.nodes=cs_work_calloc(context,2,sizeof(*s.nodes)); assert(s.nodes);
    struct qp_node *join=&s.plan->nodes[0];
    *join=(struct qp_node){.kind=QP_JOIN,.hash=1,.right=1,.nkeys=1,.join_type=QP_ANTI,.join_qual=-1};
    struct qs_plan *right=&s.plan->nodes[1].expr;
    right->nout=right->nexprs=1; right->outputs[0].type_oid=20;
    right->exprs[0].type=Q_INTEGER;
    struct operator_state *o=&s.nodes[0]; o->hash_capacity=128; o->hash_cache_slots=8;
    o->hash_data=cs_work_calloc(context,128,hash_row_bytes(right)+12); assert(o->hash_data);
    o->hash_cache=cs_work_calloc(context,10,sizeof(*o->hash_cache)); assert(o->hash_cache);
    hash_reindex(&s,0);
    int64_t special[]={-1,INT64_MIN,INT64_MAX,0};
    for(unsigned round=0;round<3;++round) for(unsigned k=0;k<68;++k) {
        struct qs_cell row[1]={{.value=k<4?special[k]:(int64_t)k-36}};
        unsigned previous=o->hash_rows;
        assert(hash_insert_exists(&s,join,o,right,row)==1);
        if(round) assert(o->hash_rows==previous);
    }
    unsigned before=o->hash_rows;
    /* The last bitmap key and the first tagged key both need a successful
     * insertion before subsequent rows may be skipped. */
    for(int key=127;key<=128;++key) {
        struct qs_cell row[1]={{.value=key}};
        o->hash_rows=o->hash_capacity-1;
        assert(hash_insert_exists(&s,join,o,right,row)==0);
        o->hash_rows=before;
        assert(hash_insert_exists(&s,join,o,right,row)==1);
        assert(o->hash_rows==++before);
        assert(hash_insert_exists(&s,join,o,right,row)==1);
        assert(o->hash_rows==before);
    }
    free(s.plan); assert(!cs_work_end(context));
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
/* Exercise bound propagation through the real Sort CSF, including OFFSET,
 * zero LIMIT, and a second consumer that requires the complete ordering. */
static void topn_bound(void)
{
    const size_t limit=64U*1024*1024;
    assert(!cs_work_init(limit));
    for(unsigned shared=0;shared<2;++shared) for(unsigned top=0;top<6;top+=5) {
        cs_work_context *ctx=cs_work_begin_limit(limit); assert(ctx);
        struct execution *s=cs_work_calloc(ctx,1,sizeof(*s)); assert(s);
        s->context=ctx; s->sort_limit=16U*1024*1024;
        s->paged=qs_dictionary_create(ctx); assert(s->paged);
        s->plan=calloc(1,sizeof(*s->plan)); assert(s->plan); s->plan->count=3;
        s->nodes=cs_work_calloc(ctx,3,sizeof(*s->nodes)); assert(s->nodes);
        s->io=io; s->io_bytes=sizeof(io);
        struct qp_node *parent=&s->plan->nodes[0],*node=&s->plan->nodes[1];
        *parent=(struct qp_node){.kind=QP_LIMIT,.left=1,.right=-1};
        parent->expr.predicate=-1; parent->expr.limit=top; parent->expr.offset=top?3:0;
        *node=(struct qp_node){.kind=QP_SORT,.left=2,.right=-1}; node->expr.predicate=-1;
        if(shared) s->plan->cte_nodes[0]=1;
        struct qs_plan input={.nout=2}; input.outputs[0].type_oid=20; input.outputs[1].type_oid=25;
        struct sorted_rows *sort=&s->nodes[1].left_sort;
        struct qs_order order={.output=0,.descending=1};
        assert(!sort_init(s,sort,&input,1,NULL,&order));
        assert(sort->plan.limit==(shared?-1:(int64_t)top+parent->expr.offset));
        for(unsigned row=0;row<1001;++row) {
            sort->input->rows=1; sort->input->data[0].cells[0]=(struct qs_cell){.value=row};
            sort->input->data[0].cells[1]=(struct qs_cell){.value=qs_late_text(2,(uint64_t)row*QS_PAGE+31,7)};
            assert(!sort_call(s,sort,0));
        }
        sort->group.count=1; sort->group.states[0]=sort->task.state;
        unsigned count=0;
        do {
            assert(!sort_call(s,sort,1));
            for(unsigned row=0;row<sort->output->rows;++row) {
                unsigned expected=1000-count++;
                assert(sort->output->data[row].cells[0].value==expected);
                assert(sort->output->data[row].cells[1].value==qs_late_text(2,(uint64_t)expected*QS_PAGE+31,7));
            }
        } while(!sort->output->finished);
        assert(count==(shared?1001:top+(top?3:0)));
        qs_dictionary_abort(s->paged);
        free(s->plan); assert(!cs_work_end(ctx));
    }
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
/* Dependent payload is retained once, not hashed; preserve NULL, Numeric
 * scales and deferred text through Hash storage, spilling and final output. */
static void grouped_payload(void)
{
    const size_t limit=64U*1024*1024; assert(!cs_work_init(limit));
    cs_work_context *ctx=cs_work_begin_limit(limit); assert(ctx);
    struct execution s={.context=ctx,.io=io,.io_bytes=sizeof(io)};
    size_t bytes=sizeof(struct qp_plan)+sizeof(struct qs_extent);
    s.plan=calloc(1,bytes); assert(s.plan); s.plan->count=2;
    s.nodes=cs_work_calloc(ctx,2,sizeof(*s.nodes)); assert(s.nodes);
    struct qp_node *n=&s.plan->nodes[0];
    *n=(struct qp_node){.kind=QP_AGGREGATE,.left=1,.right=-1,.nkeys=3,.group_keys=1};
    n->expr.naggs=1; n->expr.nkeys=3; n->expr.nexprs=3; n->expr.aggs[0].kind=Q_SUM;
    unsigned types[]={Q_INTEGER,Q_STRING,Q_DECIMAL};
    for(unsigned k=0;k<3;++k) { n->expr.keys[k]=k; n->left_keys[k]=k; n->expr.exprs[k]=(struct qs_expr){.type=types[k],.op=Q_COLUMN,.column=k}; }
    struct operator_state *o=&s.nodes[0]; o->group_integer=1; o->group_capacity=128;
    o->group_count=&o->hash_rows; atomic_init(o->group_count,0);
    o->group_stride=(sizeof(struct compact_group)+sizeof(struct group_value)+group_key_bytes(n,o)+15)&~15U;
    o->groups=cs_work_alloc(ctx,128*o->group_stride); o->group_entries=cs_work_calloc(ctx,128,sizeof(*o->group_entries));
    assert(o->groups && o->group_entries);
    struct qs_cell keys[3]={{.value=1},{.value=qs_late_text(1,QS_PAGE+123,3)},{.value=-12345,.scale=2,.dscale=2}};
    struct group_value value={.sum={.value=7},.count=1};
    assert(group_sink_values(&s,0,o,keys,&value,0)==1);
    struct qs_cell second[3]; memcpy(second,keys,sizeof(keys)); second[1].value=qs_late_text(1,5,1); second[2].value=99;
    assert(group_hash(n,keys)==group_hash(n,second) && group_equal(&s,n,keys,second)==1);
    assert(group_sink_values(&s,0,o,second,&value,0)==1 && o->hash_rows==1);
    struct qs_cell stored[3]; group_keys(n,o,group_at(o,0),stored);
    assert(stored[1].value==keys[1].value && stored[2].value==-12345 && stored[2].scale==2 && stored[2].dscale==2);
    assert(group_at(o,0)->values[0].sum.value==14);
    keys[0].isnull=1; keys[1].isnull=1; keys[2].isnull=1;
    assert(group_sink_values(&s,0,o,keys,&value,0)==1 && o->hash_rows==2);
    group_keys(n,o,group_at(o,1),stored); assert(stored[0].isnull && stored[1].isnull && stored[2].isnull);
    /* A deferred string survives existing packed Hash rows. */
    struct qs_plan packed={.nout=1}; packed.outputs[0].type_oid=25;
    unsigned char row[32]; hash_store(row,second+1,&packed); hash_load(stored,row,&packed);
    assert(stored[0].value==second[1].value);
    struct qs_extent *extent=(void *)((unsigned char *)s.plan+sizeof(*s.plan));
    *extent=(struct qs_extent){.logical=0,.lba=100,.bytes=2*QS_PAGE};
    s.plan->nodes[1].bytes=2*QS_PAGE;
    s.plan->files[1]=(struct qp_file_mapping){.offset=sizeof(*s.plan),.nsid=1,.lba_bytes=512,.count=1};
    long long lengths[]={bytes}; struct femu_csd_args args={.mr_len=lengths}; s.args=&args;
    const char *text; unsigned length;
    assert(output_text(&s,(struct qs_cell){.value=qs_late_text(1,QS_PAGE+123,3)},&text,&length)<0 && s.late_wait==csNeedNvmRead(1));
    struct femu_graph_read *read=(void *)io;
    assert(le32toh(read->afdm_index)==4 && le64toh(read->lba)==116 && le64toh(read->offset)==QS_PAGE);
    memcpy(s.late_page+123,"abc",3); s.late_valid=1; s.late_wait=0;
    assert(!output_text(&s,(struct qs_cell){.value=qs_late_text(1,QS_PAGE+123,3)},&text,&length) && length==3 && !memcmp(text,"abc",3));
    assert(output_text(&s,(struct qs_cell){.value=qs_late_text(1,2*QS_PAGE-1,3)},&text,&length)<0);
    struct qs_plan output={.nout=1}; output.exprs[0].type=Q_STRING;
    struct qs_result *out=cs_work_calloc(ctx,1,sizeof(*out)); assert(out);
    struct qs_cell empty={.value=qs_late_text(1,QS_PAGE+123,0)};
    assert(!export_row(&s,&output,&empty,out) && qs_cell_is_string_ref(&empty) && !qs_cell_string(&empty));
    assert(!export_row(&s,&output,&empty,out)); /* Empty text must stay resolved on reentry. */
    free(s.plan); assert(!cs_work_end(ctx));
    struct cs_work_stats stats; assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--group-payload-64m")) { grouped_payload(); return 0; }
    if(argc==2 && !strcmp(argv[1],"--topn-bound-64m")) { topn_bound(); return 0; }
    if(argc==2 && !strcmp(argv[1],"--existence-cache-64m")) { existence_cache(); return 0; }
    if(argc==2 && !strcmp(argv[1],"--join-payload-64m")) { join_payload_resume(); return 0; }
    assert(argc==1 || (argc==2 && (!strcmp(argv[1],"--aggregate") || !strcmp(argv[1],"--hash-aggregate") || !strcmp(argv[1],"--join") || !strcmp(argv[1],"--pipeline"))));
    int aggregates=argc==2 && !strcmp(argv[1],"--aggregate"),hashed=argc==2 && !strcmp(argv[1],"--hash-aggregate");
    int joining=argc==2 && !strcmp(argv[1],"--join"),pipeline=argc==2 && !strcmp(argv[1],"--pipeline");
    if(joining) signed_probe_slots();
    assert(!cs_work_init(BUDGET));
    cs_work_context *ctx=cs_work_begin_limit(BUDGET); assert(ctx);
    struct qs_dictionary *d=qs_dictionary_create(ctx); assert(d);
    struct qs_dictionary_access *access=qs_dictionary_access_create(d); assert(access);
    struct execution *s=cs_work_calloc(ctx,1,sizeof(*s)); assert(s);
    s->context=ctx; s->io=io; s->text=qs_dictionary_text_create(d); assert(s->text);
    s->plan=calloc(1,sizeof(*s->plan)); assert(s->plan); plan(s->plan);
    s->nodes=cs_work_calloc(ctx,joining?3:1,sizeof(*s->nodes)); assert(s->nodes);
    if(joining) {
        s->nodes[0].join_condition=cs_work_calloc(ctx,1,sizeof(*s->nodes[0].join_condition)); assert(s->nodes[0].join_condition);
        for(unsigned i=0;i<3;++i) { s->nodes[i].group.tail=cs_work_alloc(ctx,QS_SORT_IO_BYTES); assert(s->nodes[i].group.tail); }
        s->nodes[0].hash_data=cs_work_alloc(ctx,4096); assert(s->nodes[0].hash_data);
    }
    struct projection_batch *b=NULL;
    if(aggregates || hashed || pipeline) {
        b=cs_work_calloc(ctx,1,sizeof(*b)); assert(b);
        b->vectors=cs_work_calloc(ctx,pipeline?18:4,sizeof(*b->vectors)); assert(b->vectors);
    }
    if(pipeline) {
        b->cells=cs_work_calloc(ctx,4*QS_VECTOR_ROWS,sizeof(*b->cells)); assert(b->cells);
        b->group_input=cs_work_calloc(ctx,1,sizeof(*b->group_input)); assert(b->group_input);
    }
    if(!aggregates && !hashed && !joining && !pipeline) {
        s->output_order=cs_work_calloc(ctx,QS_RESULT_ROWS*5,sizeof(*s->output_order)); assert(s->output_order);
    }
    struct execution *peer=NULL;
    if(hashed) {
        hash_table(s);
        s->group_update=cs_work_calloc(ctx,1,sizeof(*s->group_update)); assert(s->group_update);
        assert(aggregate_input(s,&s->nodes[0]));
        b->group_input=cs_work_calloc(ctx,1,sizeof(*b->group_input)); assert(b->group_input);
        peer=cs_work_calloc(ctx,1,sizeof(*peer)); assert(peer);
        peer->context=ctx; peer->plan=s->plan; peer->text=qs_dictionary_text_create(d); assert(peer->text);
        peer->group_update=cs_work_calloc(ctx,1,sizeof(*peer->group_update)); assert(peer->group_update);
        peer->io=calloc(1,QS_SORT_IO_BYTES); assert(peer->io);
    }
    for(unsigned row=0;row<KEYS;++row) for(unsigned col=0;col<2;++col) {
        char bytes[LENGTH]; make_text(bytes,row,col); ids[row][col]=insert(d,access,bytes,LENGTH);
    }
    unsigned scalar_waits,output_waits;
    if(pipeline) {
        scalar_waits=pipeline_columns(s,b,d,access,&output_waits);
    } else if(joining) {
        scalar_waits=join_rows(s,d,access); output_waits=index_condition_rows(s,d,access);
    } else if(hashed) {
        scalar_waits=shared_interleave(s,peer,d,access);
        output_waits=hash_vectors(s,b)+partition_combine(s,peer,d,access)+hash_scalar(s,d,access)+spill_partial_merge(s,d,access);
        assert(!qs_dictionary_text_release(peer->text)); free(peer->io);
    } else if(aggregates) {
        scalar_waits=aggregate_scalar(s,d,access)+scalar_group_keys(s,d,access);
        output_waits=aggregate_vectors(s,b,0)+aggregate_vectors(s,b,1)+combine_values(s);
        equality_no_io(s);
    } else { scalar_waits=projection(s,d,access); output_waits=output(s)+output_batch(s,d,access); }
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
    if(pipeline) {
        cancel_operation(CS_NEED_STORE,6); cancel_operation(CS_NEED_LOAD,6);
        cancel_operation(CS_NEED_STORE,7); cancel_operation(CS_NEED_LOAD,7);
    } else if(joining) {
        cancel_operation(CS_NEED_STORE,4); cancel_operation(CS_NEED_LOAD,4);
        cancel_operation(CS_NEED_STORE,5); cancel_operation(CS_NEED_LOAD,5);
    } else if(hashed) {
        cancel_operation(CS_NEED_STORE,3); cancel_operation(CS_NEED_LOAD,3);
    } else if(aggregates) {
        cancel_operation(CS_NEED_STORE,2); cancel_operation(CS_NEED_LOAD,2);
    } else {
    cancel_operation(CS_NEED_STORE,0); cancel_operation(CS_NEED_LOAD,0);
    cancel_operation(CS_NEED_STORE,1); cancel_operation(CS_NEED_LOAD,1);
    cancel_operation(0,1);
    }
    puts(pipeline?"PASS native projection text binding, selected rows, materialization continuation and batch cleanup":
        joining?"PASS paged multi-key Merge/Hash INNER, LEFT, SEMI and ANTI with resumable scalar conditions":
        hashed?"PASS shared hash rebase during dictionary I/O, vector partial retention and scalar arguments":
        aggregates?"PASS dictionary ID grouping and resumable scalar/vector/stream MIN/MAX and partial combine":
        "PASS scalar text memo, retained input, short circuit, partial export and cancellation/error cleanup");
    return 0;
}
