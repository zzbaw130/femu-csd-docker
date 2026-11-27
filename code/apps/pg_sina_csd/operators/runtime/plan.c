/* Execute a PG physical tree. PG submits page LOADs and SNIA handles spill;
 * tuple bytes and intermediate rows never cross back to the PG executor. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "plan.h"
#include "query_vector.h"
#include "index_scan.h"
#include "graph_state.h"
#include "graph_batch.h"
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <endian.h>

static int prepare_projection(struct execution *s,unsigned id,int source)
{
#ifdef QP_SPLIT_RUNTIME
    return s->projection_ops?s->projection_ops->prepare(s,id,source):-1;
#else
    return projection_init(s,id,source);
#endif
}

static int resume_projection(struct execution *s,unsigned id,struct qs_cell *row)
{
#ifdef QP_SPLIT_RUNTIME
    return s->projection_ops?s->projection_ops->retry(s,id,row):-1;
#else
    return retry_project(s,id,row);
#endif
}

/* Store PG integer/date widths directly; numeric and other values retain all
 * 128 bits. Keep scale/dscale/NULL, and avoid unaligned integer accesses. */
void hash_store(unsigned char *to,const struct qs_cell *from,const struct qs_plan *p)
{
    for(unsigned i=0;i<p->nout;++i) {
        unsigned bytes=hash_value_bytes(p,i);
        hash_store_value(to,&from[i],bytes); to+=bytes+(bytes<16?1:3);
    }
}
/* Observe waits at CSF boundaries, never per tuple. A completed input wait
 * includes LOAD/queueing; dictionary BUSY includes the host retry policy. */
void wait_resume(struct execution *s)
{
    if(s->wait_started) {
        s->wait_ns[s->wait_kind]+=now_ns(CLOCK_MONOTONIC)-s->wait_started;
        s->wait_started=0;
    }
}
static void wait_begin(struct execution *s,unsigned kind)
{
    if(s->profiling && s->worker) {
        s->wait_kind=kind; ++s->wait_calls[kind]; s->wait_started=now_ns(CLOCK_MONOTONIC);
    }
}
void wait_io(struct execution *s,int64_t status)
{
    uint64_t operation=(uint64_t)status&~CS_IO_VALUE_MASK;
    if(status==QS_DICTIONARY_BUSY) wait_begin(s,QP_WAIT_DICTIONARY);
    else if(operation==CS_NEED_NVM_READ) wait_begin(s,QP_WAIT_NVM);
    else if(operation==CS_NEED_STORE || operation==CS_NEED_LOAD || operation==CS_NEED_FREE)
        wait_begin(s,QP_WAIT_SPILL);
}
/* Blocking-call boundaries, never per input tuple. Child Scan/Sort and nested
 * blocking CPU is subtracted; merged worker counters are not thread-local CPU. */
static int64_t next_row_impl(struct execution *,unsigned,struct qs_cell *);
bool graph_input_edge(const struct execution *s,unsigned id)
{
    if(s->stream_edge) return id==s->stream_edge_node;
    if(s->block_input) return id==s->block_input_node;
    return s->hash_input && s->graph_source_pending &&
        s->graph_source_kind==GRAPH_EMIT_FEED && id==s->hash_input_node;
}
int64_t next_row(struct execution *s,unsigned id,struct qs_cell *row)
{
    if(s->stream_edge && id==s->stream_edge_node) {
        struct projection_batch *edge=s->stream_edge;
        if(edge->position==edge->count) { s->scan_drained=true; return QP_WAIT; }
        for(unsigned col=0;col<edge->plan.nout;++col) {
            unsigned expr=edge->plan.outputs[col].index;
            if(edge->vectors[expr].errors[edge->position]) return -5;
            row[col]=qs_vector_cell(&edge->plan,edge->vectors,expr,edge->position);
        }
        ++edge->position;
        return 2;
    }
    if(graph_input_edge(s,id))
        return s->block_input?block_edge_row(s,id,row):hash_build_input(s,id,row);
    int64_t status=3;
    if(row && id<s->plan->count && s->nodes[id].project_retry && !s->nodes[id].project_retry->inline_only) status=resume_projection(s,id,row);
    if(status==3) status=next_row_impl(s,id,row);
    if(status<0 && !s->expression_wait && !s->control->error) { s->control->error=status; s->control->error_node=id; }
    return status;
}
#ifndef QP_SPLIT_RUNTIME
static int64_t merge_qual_next(struct execution *,unsigned,struct qs_cell *);
static void merge_qual_close(struct execution *,struct operator_state *);
static int key_compare(struct execution *,const struct qp_node *,const struct qs_cell *,const struct qs_cell *);
#endif
void init_eval(struct qs_eval *e,const struct qs_plan *plan,const struct qs_cell *row,const struct qs_cell *aggregates)
{
    e->plan=plan; e->tuple=row; e->aggregates=aggregates;
    memset(e->valid,0,plan->nexprs);
}
static int text_get(struct execution *,unsigned,const char **,unsigned *);
int compare_cell(struct execution *s,struct qs_cell a,struct qs_cell b,unsigned type)
{
    if(a.isnull || b.isnull) return (a.isnull!=0)-(b.isnull!=0);
    if(type==Q_STRING) {
        const char *x,*y; unsigned xn,yn;
        if(text_get(s,(unsigned)a.value,&x,&xn) || text_get(s,(unsigned)b.value,&y,&yn)) return INT_MIN;
        int cmp=memcmp(x,y,xn<yn?xn:yn); if(!cmp) cmp=(xn>yn)-(xn<yn);
        if(text_release(s)) return INT_MIN;
        return cmp;
    }
    int cmp=0; return qs_compare(a,b,&cmp)?INT_MIN:cmp;
}
int equal_cell(struct execution *s,struct qs_cell a,struct qs_cell b,unsigned type)
{
    if(a.isnull || b.isnull) return !!a.isnull==!!b.isnull;
    if(type==Q_STRING) return a.value==b.value;
    int cmp=compare_cell(s,a,b,type); return cmp==INT_MIN?-1:!cmp;
}
static inline struct qs_cell set_contains(const struct operator_state *set,struct qs_cell a)
{
    struct qs_cell v={0};
    if(a.isnull) v.isnull=set->set_count || set->set_null;
    else {
        if(a.value<=INT64_MAX && a.value>=INT64_MIN && set->set_capacity) {
            unsigned slot=mix_hash((uint64_t)a.value)&(set->set_capacity-1);
            while(set->set[slot].occupied) {
                if(set->set[slot].value==a.value) { v.value=1; break; }
                slot=(slot+1)&(set->set_capacity-1);
            }
        }
        if(!v.value) v.isnull=set->set_null;
    }
    return v;
}
int inset_vectors(struct qs_vector_input *input,unsigned index,const uint16_t *rows,unsigned count)
{
    if(!count) return 0;
    if(count>QS_VECTOR_ROWS) return -1;
    struct execution *s=input->set_state; const struct qs_expr *e=&input->plan->exprs[index];
    if(e->column<0 || (unsigned)e->column>=s->plan->count) return -1;
    const struct operator_state *set=graph_state_owner(s,e->column);
    if(!set->set_ready) return -1;
    if(set->lookup_spilled_set) {
        struct qs_cell keys[QS_VECTOR_ROWS],*values; unsigned selection[QS_VECTOR_ROWS];
        for(unsigned j=0;j<count;++j) { selection[j]=rows[j]; keys[j]=input->vectors[e->left].errors[rows[j]]?(struct qs_cell){.isnull=1}:qs_vector_cell(input->plan,input->vectors,e->left,rows[j]); }
        int64_t status=set->lookup_spilled_set(s,e->column,e,keys,selection,count,&values);
        if(status) { s->expression_wait=status>2; return -1; }
        struct qs_vector *out=&input->vectors[index]; out->kind=QS_NARROW;
        for(unsigned j=0;j<count;++j) { unsigned i=rows[j]; out->narrow[i]=values[j].value; out->nulls[i]=values[j].isnull;
            out->errors[i]=input->vectors[e->left].errors[i]; out->dscales[i]=0; out->all_valid&=!out->errors[i] && !values[j].isnull; }
        return 0;
    }
    const struct qs_vector *left=&input->vectors[e->left]; struct qs_vector *out=&input->vectors[index];
    out->kind=QS_NARROW;
    for(unsigned j=0;j<count;++j) {
        unsigned i=rows[j]; struct qs_cell v={0}; unsigned error=left->errors[i];
        if(!error) v=set_contains(set,qs_vector_cell(input->plan,input->vectors,e->left,i));
        out->narrow[i]=v.value; out->nulls[i]=v.isnull; out->errors[i]=error; out->dscales[i]=0;
        out->all_valid&=!error && !v.isnull;
    }
    return 0;
}
/* Both scalar expressions and final output release borrowed bytes after
 * completing their operation. A wait retains the same operands and memo. */
int text_status(struct execution *s,int64_t status)
{
    if(status>0) { s->text_wait=status; s->expression_wait=1; }
    return status?-3:0;
}
static const char *sort_text_get(struct execution *s,unsigned id,unsigned *length)
{
    for(unsigned i=0;s->plan && i<s->plan->count;++i) {
        const struct sorted_rows *sorts[]={&s->nodes[i].left_sort,&s->nodes[i].right_sort};
        for(unsigned j=0;j<2;++j) if(sorts[j]->lookup_text && sorts[j]->output && sorts[j]->output->rows) {
            const char *bytes=sorts[j]->lookup_text(sorts[j]->output,id,length);
            if(bytes) return bytes;
        }
    }
    return NULL;
}
static int text_get(struct execution *s,unsigned id,const char **bytes,unsigned *length)
{
    *bytes=sort_text_get(s,id,length); if(*bytes) return 0;
    if(!s->text) return qs_dict_get(s->dictionary,id,bytes,length);
    return text_status(s,qs_dictionary_text_get(s->text,id,bytes,length,s->io));
}
static int text_insert(struct execution *s,const char *bytes,unsigned length,unsigned *id)
{
    if(!s->text) {
        int value=qs_dict_insert(s->dictionary,bytes,length); if(value<0) return -3;
        *id=value; return 0;
    }
    return text_status(s,qs_dictionary_text_insert(s->text,bytes,length,id,s->io));
}
int text_release(struct execution *s)
{ return s->text?qs_dictionary_text_release(s->text):0; }
int evaluate(struct execution *s,struct qs_eval *e,int index,struct qs_cell *out)
{
    if(index<0 || (unsigned)index>=e->plan->nexprs) return -1;
    if(e->valid[index]) { *out=e->cells[index]; return 0; }
    const struct qs_expr *x=&e->plan->exprs[index]; struct qs_cell a={0},b={0},v={0};
    if(x->op==QP_PARAMETER) v=s->params[x->column];
    else if(x->op==QP_INSET) {
        if(x->column<0 || (unsigned)x->column>=s->plan->count) return -5;
        const struct operator_state *set=graph_state_owner(s,x->column);
        if(!set->set_ready || evaluate(s,e,x->left,&a)) return -5;
        if(set->lookup_spilled_set) {
            struct qs_cell *values; unsigned row=0;
            int64_t status=set->lookup_spilled_set(s,x->column,x,&a,&row,1,&values);
            if(status) { s->expression_wait=status>2; return -5; }
            v=values[0];
        } else v=set_contains(set,a);
    }
    else if(x->op==Q_CONST && x->type==Q_STRING && !x->isnull) {
        const struct qs_string *literal=&e->plan->strings[x->constant];
        uintptr_t at=(uintptr_t)e->plan,first=(uintptr_t)&s->plan->nodes[0].expr;
        unsigned node=at>=first?(at-first)/sizeof(struct qp_node):s->plan->count;
        unsigned id=s->literal_ids && node<s->plan->count && e->plan==&s->plan->nodes[node].expr?
            s->literal_ids[node*QS_STRINGS+(unsigned)x->constant]:0;
        if(!id && (text_insert(s,literal->bytes,literal->length,&id) || text_release(s))) return -3;
        v.value=id;
    } else if(x->op==Q_COLUMN || x->op==Q_CONST || x->op==Q_AGGREF) return qs_evaluate(e,index,out);
    else {
        if(evaluate(s,e,x->left,&a)) return -5;
        if(x->op==Q_CASE) {
            if(evaluate(s,e,!a.isnull && a.value?x->right:x->otherwise,&v)) return -5;
            /* Preserve the actual numeric scale of an intermediate value. */
        } else if(x->op==QP_COALESCE) {
            v=a; if(a.isnull && evaluate(s,e,x->right,&v)) return -5;
        } else if(x->op==Q_AND && !a.isnull && !a.value) v=a;
        else if(x->op==Q_OR && !a.isnull && a.value) v=a;
        else if(x->op==Q_NOT || x->op==Q_IS_NULL || x->op==Q_IS_NOT_NULL || x->op==Q_YEAR)
            return qs_evaluate(e,index,out);
        else {
            if(evaluate(s,e,x->right,&b)) return -5;
            v.isnull=a.isnull || b.isnull;
            if(x->op==QP_LIKE || x->op==QP_SUBSTRING ||
               (x->op>=Q_EQ && x->op<=Q_GE && e->plan->exprs[x->left].type==Q_STRING)) {
                if(!v.isnull) {
                    const char *left,*right; unsigned ln,rn;
                    if(x->op==QP_SUBSTRING) {
                        struct qs_cell length; if(evaluate(s,e,x->otherwise,&length)) return -5;
                        if(length.isnull) v.isnull=1;
                        else {
                            if(length.value<0 || b.value<INT_MIN || b.value>INT_MAX || length.value>INT_MAX) return -5;
                            if(text_get(s,(unsigned)a.value,&left,&ln)) return -3;
                            int64_t begin=b.value-1,end=begin+length.value;
                            if(begin<0) begin=0;
                            if(end<begin) end=begin;
                            unsigned first=0,last=0; int64_t character=0;
                            while(first<ln && character<begin) { first+=qs_text_character(left+first,ln-first); ++character; }
                            last=first;
                            while(last<ln && character<end) { last+=qs_text_character(left+last,ln-last); ++character; }
                            unsigned id; if(text_insert(s,left+first,last-first,&id)) return -3;
                            v.value=id;
                        }
                    } else {
                        if(text_get(s,(unsigned)a.value,&left,&ln) ||
                           text_get(s,(unsigned)b.value,&right,&rn)) return -3;
                        if(x->op==QP_LIKE) { int match=qs_text_like(right,rn,left,ln); if(match<0) return -5; v.value=match; }
                        else {
                            int cmp=memcmp(left,right,ln<rn?ln:rn); if(!cmp) cmp=(ln>rn)-(ln<rn);
                            v.value=x->op==Q_EQ?cmp==0:x->op==Q_NE?cmp!=0:x->op==Q_LT?cmp<0:x->op==Q_LE?cmp<=0:x->op==Q_GT?cmp>0:cmp>=0;
                        }
                    }
                    if(text_release(s)) return -3;
                }
            } else return qs_evaluate(e,index,out);
        }
    }
    e->valid[index]=1; e->cells[index]=v; *out=v; return 0;
}
/* Materialization retains its own position after expression evaluation has
 * completed. Do not rebuild vectors or advance a producer on an I/O wait. */
int materialize_vector(struct execution *s,struct qs_vector *v,unsigned count)
{
    if(!s->paged) return qs_vector_materialize_text(v,count,s->dictionary);
    return text_status(s,qs_vector_materialize_text_paged(v,count,s->paged,s->text,s->io,0));
}
int materialize_outputs(struct execution *s,struct projection_batch *b)
{
    b->materializing=1;
    for(;b->materialized_column<b->plan.nout;++b->materialized_column) {
        int index=b->plan.outputs[b->materialized_column].index;
        if(index>=0 && materialize_vector(s,&b->vectors[index],b->count)) return -3;
    }
    b->materializing=0; b->materialized_column=0; return 0;
}
int intern_view(struct execution *s,struct qs_text_view view,unsigned *id)
{
    if(!s->paged) {
        int value=qs_text_intern(s->dictionary,view); if(value<0) return -3;
        *id=value; return 0;
    }
    if(view.id && !view.offset && view.length==UINT_MAX) {
        if(!qs_dictionary_valid_id(s->paged,view.id)) return -3;
        *id=view.id; return 0;
    }
    const char *bytes=view.data; unsigned length=view.length;
    if(view.id) {
        if(text_get(s,view.id,&bytes,&length)) return -3;
        if(view.offset>length || (view.length!=UINT_MAX && view.length>length-view.offset)) return -3;
        bytes+=view.offset; length=view.length==UINT_MAX?length-view.offset:view.length;
    }
    if(text_insert(s,bytes,length,id)) return -3;
    return text_release(s);
}
/* A graph consumer borrows the producer's packet until it has drained it.
 * The ordinary executor continues pulling from the original child. */
int64_t hash_build_input(struct execution *s,unsigned child,struct qs_cell *row)
{
    const struct qs_result *in=s->hash_input;
    if(!in) return next_row(s,child,row);
    if(child!=s->hash_input_node) return QP_WAIT;
    if(s->hash_input_position==in->rows) return in->finished?0:QP_WAIT;
    unsigned cols=s->plan->nodes[child].expr.nout;
    unsigned at=in->begin+s->hash_input_position++;
    memcpy(row,(const struct qs_cell *)in->data+(size_t)at*cols,cols*sizeof(*row));
    return 2;
}
int64_t spool_append_context(struct execution *s,struct spool *p,const void *row,unsigned row_bytes,
    cs_work_context *context,size_t resident_reserve)
{
    unsigned block_bytes=spool_block_bytes(p),block_rows=block_bytes/row_bytes;
    if(!p->tail) {
        p->tail=cs_work_alloc(context,block_bytes); p->loaded=UINT_MAX;
        if(!p->tail) return -4;
    }
    if(p->used==block_rows) {
        if(p->blocks==p->capacity) {
            unsigned capacity=p->capacity?p->capacity*2:8;
            void *pages=cs_work_realloc(context,p->pages,(size_t)capacity*sizeof(*p->pages)); if(!pages) return -4;
            p->pages=pages; p->capacity=capacity;
        }
        unsigned char *tail=NULL;
        if(resident_reserve && cs_work_available(context)>resident_reserve+block_bytes)
            tail=cs_work_alloc(context,block_bytes);
        if(!tail) {
            memcpy(s->io,p->tail,block_bytes); s->pending_spool=p; s->pending_store=1;
            return csNeedStore(block_bytes);
        }
        p->pages[p->blocks++]=(struct spool_block){.data=p->tail};
        p->tail=tail; p->used=0;
    }
    memcpy(p->tail+(size_t)p->used*row_bytes,row,row_bytes); ++p->used; ++p->rows; return 0;
}
int64_t spool_append_bytes(struct execution *s,struct spool *p,const void *row,unsigned row_bytes)
{ return spool_append_context(s,p,row,row_bytes,s->context,0); }
int64_t spool_row_bytes(struct execution *s,struct spool *p,uint64_t index,unsigned row_bytes,void *row)
{
    unsigned block_bytes=spool_block_bytes(p),block_rows=block_bytes/row_bytes,block=index/block_rows;
    if(index>=p->rows) return -1;
    unsigned char *data=p->tail;
    if(block<p->blocks && p->pages[block].data) data=p->pages[block].data;
    else if(block<p->blocks) {
        if(!p->cache) { p->cache=cs_work_alloc(s->context,block_bytes); if(!p->cache) return -4; }
        if(p->loaded!=block) {
            s->pending_spool=p; s->pending_store=0; s->pending_block=block;
            return csNeedLoad(p->pages[block].token);
        }
        data=p->cache;
    }
    memcpy(row,data+(index%block_rows)*row_bytes,row_bytes); return 0;
}
int64_t spool_append(struct execution *s,struct spool *p,const struct qs_cell *row,unsigned cols)
{ return spool_append_bytes(s,p,row,cols*sizeof(*row)); }
int64_t spool_row(struct execution *s,struct spool *p,uint64_t index,unsigned cols,struct qs_cell *row)
{ return spool_row_bytes(s,p,index,cols*sizeof(*row),row); }
int64_t block_edge_row(struct execution *s,unsigned id,struct qs_cell *row)
{
    struct spool *in=s->block_input;
    if(s->block_edge_read==in->rows) return QP_WAIT;
    int64_t status=spool_row(s,in,s->block_edge_read,s->plan->nodes[id].expr.nout,row);
    if(status) return status;
    ++s->block_edge_read;
    return 2;
}
#ifndef QP_SPLIT_RUNTIME
static int key_compare(struct execution *s,const struct qp_node *n,const struct qs_cell *left,const struct qs_cell *right)
{
    const struct qs_plan *types=&s->plan->nodes[n->left].expr;
    struct operator_state *o=&s->nodes[n-s->plan->nodes];
    for(unsigned i=o->merge_key;i<n->nkeys;++i) {
        unsigned col=n->left_keys[i];
        unsigned type=types->exprs[types->outputs[col].index].type;
        struct qs_cell a=left[col],b=right[n->right_keys[i]];
        int cmp=compare_cell(s,a,b,type);
        if(cmp==INT_MIN) { o->merge_key=i; return cmp; }
        if(n->expr.norder) {
            if(a.isnull || b.isnull) { if(n->expr.order[i].nulls_first) cmp=-cmp; }
            else if(n->expr.order[i].descending) cmp=-cmp;
        }
        if(cmp) { o->merge_key=0; return cmp; }
    }
    o->merge_key=0; return 0;
}
#endif
#ifndef QP_SPLIT_RUNTIME
/* Keep duplicate-group and spill cursors here; projection consumes matching
 * pairs separately, either as a column batch or through the scalar adapter. */
int64_t merge_match_next(struct execution *s,unsigned id,struct qs_cell *values)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned ln=s->plan->nodes[n->left].expr.nout,rn=s->plan->nodes[n->right].expr.nout;
    struct qs_order left_order[QS_KEYS],right_order[QS_KEYS];
    for(unsigned i=0;i<n->nkeys;++i) {
        left_order[i]=n->expr.norder?n->expr.order[i]:(struct qs_order){.output=n->left_keys[i]};
        right_order[i]=left_order[i]; right_order[i].output=n->right_keys[i];
    }
    if(!o->merge_ordered) {
        o->merge_ordered=4;
        if(!o->left_sort.prefix && input_ordered(s->plan,n->left,n->nkeys,n->left_keys,left_order)) o->merge_ordered|=1;
        if(!o->right_sort.prefix && input_ordered(s->plan,n->right,n->nkeys,n->right_keys,right_order)) o->merge_ordered|=2;
    }
    for(;;) {
        int64_t status;
        if(o->phase==2) {
            status=o->merge_ordered&1?next_row(s,n->left,o->left):sorted_input(s,&o->left_sort,n->left,n->nkeys,n->left_keys,left_order,o->left);
            if(status!=2) { if(!status) merge_qual_close(s,o); return status; }
            o->have_left=1; o->position=0; o->matched=0; o->phase=0;
            const struct qs_plan *types=&s->plan->nodes[n->left].expr;
            int same=1;
            for(unsigned i=0;i<n->nkeys;++i) {
                unsigned col=n->left_keys[i];
                int equal=equal_cell(s,o->left[col],o->first[col],types->exprs[types->outputs[col].index].type);
                if(equal<0) return -5;
                if(!equal) same=0;
            }
            if(!same) { o->group_ready=0; o->group.rows=o->group.blocks=o->group.used=0; o->group.loaded=UINT_MAX; }
        }
        if(!o->have_left) {
            status=o->merge_ordered&1?next_row(s,n->left,o->left):sorted_input(s,&o->left_sort,n->left,n->nkeys,n->left_keys,left_order,o->left);
            if(status!=2) { if(!status) merge_qual_close(s,o); return status; }
            o->have_left=1; o->position=0; o->matched=0;
        }
        if(!o->group_ready) {
            if(!o->have_right && !o->input_done) {
                status=o->merge_ordered&2?next_row(s,n->right,o->right):
                    sorted_input(s,&o->right_sort,n->right,n->nkeys,n->right_keys,right_order,o->right);
                if(status==QP_WAIT || status<0 || status>2) return status;
                o->have_right=status==2; o->input_done=!status;
            }
            if(o->have_right && !keys_null(n,o->left)) {
                int cmp=key_compare(s,n,o->left,o->right); if(cmp==INT_MIN) return -5;
                if(cmp>0) { o->have_right=0; continue; }
                if(!cmp) {
                    status=spool_append(s,&o->group,o->right,rn); if(status) return status;
                    o->have_right=0; o->phase=1; continue;
                }
            }
            if(o->phase==1) { o->group_ready=1; o->phase=0; }
            else {
                o->have_left=0;
                if(n->join_type==QP_LEFT || n->join_type==QP_ANTI) {
                    memcpy(values,o->left,ln*sizeof(*values));
                    for(unsigned i=0;i<rn;++i) values[ln+i]=(struct qs_cell){.isnull=1};
                    return 2;
                }
                continue;
            }
        }
        if(o->position<o->group.rows || (o->merge_qual && o->merge_qual->expr.position<o->merge_qual->expr.count)) {
            int qualified=0;
            if(n->join_qual>=0 && !(o->join_condition && o->join_condition->active)) {
                status=merge_qual_next(s,id,values);
                if(!status) continue;
                if(status!=3) { if(status!=2) return status; qualified=1; }
            }
            if(!qualified) {
                if(o->join_condition && o->join_condition->active)
                    memcpy(values,o->join_condition->input,n->expr.nattrs*sizeof(*values));
                else {
                    memcpy(values,o->left,ln*sizeof(*values));
                    status=spool_row(s,&o->group,o->position,rn,values+ln); if(status) return status;
                }
                struct qs_cell match={.value=1};
                if(n->join_qual>=0 && join_condition(s,id,values,&match)) return -5;
                ++o->position;
                if(match.isnull || !match.value) continue;
            }
            o->matched=1;
            if((n->join_type==QP_SEMI || n->join_type==QP_ANTI) && o->merge_qual)
                o->merge_qual->expr.count=o->merge_qual->expr.position=0;
            if(n->join_type==QP_ANTI) { o->position=o->group.rows; continue; }
            if(n->join_type==QP_SEMI) o->position=o->group.rows;
            ++s->metrics.join_rows; return 2;
        }
        if(!o->matched && (n->join_type==QP_LEFT || n->join_type==QP_ANTI)) {
            memcpy(values,o->left,ln*sizeof(*values));
            for(unsigned i=0;i<rn;++i) values[ln+i]=(struct qs_cell){.isnull=1};
            o->matched=1;
            return 2;
        }
        /* Keep the right duplicate group for another left row with this key. */
        memcpy(o->first,o->left,ln*sizeof(*o->first)); o->have_left=0; o->phase=2;
    }
}
int64_t join_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    struct qs_cell values[QS_COLS];
    for(;;) {
        int64_t status=merge_match_next(s,id,values); if(status!=2) return status;
        int count=project(s,&s->plan->nodes[id],values,NULL,row);
        if(count<0) return count;
        if(count) return 2;
    }
}
#endif
/* Translate a projection chain into one expression DAG over its producer's
 * columns. This preserves PG's tree and avoids row materialization
 * between adjacent projections. Unsupported expressions keep the old path. */
/* Planner rewrites can append leaf operands to an existing expression.
 * Import still emits each dependency before its consumer. */
#ifndef QP_SPLIT_RUNTIME
static void merge_qual_close(struct execution *s,struct operator_state *o)
{
    if(!o->merge_qual) return;
    cs_work_free(s->context,o->merge_qual->expr.cells);
    cs_work_free(s->context,o->merge_qual->expr.vectors);
    cs_work_free(s->context,o->merge_qual); o->merge_qual=NULL;
}
/* Qualify consecutive candidates within one spool block. Retain the block
 * until its selected payloads drain; no speculative load crosses its end. */
static int64_t merge_qual_next(struct execution *s,unsigned id,struct qs_cell *values)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned ln=s->plan->nodes[n->left].expr.nout,rn=s->plan->nodes[n->right].expr.nout;
    struct merge_qual_batch *q=o->merge_qual;
    if(q && q->expr.retry) goto evaluate_merge;
    if(!q || q->expr.position==q->expr.count) {
        int semi=n->join_type==QP_SEMI || n->join_type==QP_ANTI;
        if((o->merge_qual_checked && !q) || (semi && !o->position)) return 3;
        unsigned block_rows=QS_SORT_IO_BYTES/(rn*sizeof(*values));
        uint64_t remaining=o->group.rows-o->position;
        unsigned count=semi?64:QS_VECTOR_ROWS;
        if(remaining<count) count=remaining;
        if(count>block_rows-o->position%block_rows) count=block_rows-o->position%block_rows;
        if(count<2) return 3;
        struct qs_cell right[QS_OUT];
        int64_t status=spool_row(s,&o->group,o->position,rn,right);
        if(status) {
            if(status==-4 && q) { merge_qual_close(s,o); return 3; }
            return status;
        }
        if(!q) {
            o->merge_qual_checked=1;
            q=cs_work_calloc(s->context,1,sizeof(*q)); if(!q) return 3;
            if(join_qual_init(s,id,&q->expr,NULL)) { cs_work_free(s->context,q); return 3; }
            o->merge_qual=q;
        }
        struct projection_batch *b=&q->expr; q->begin=o->position;
        for(unsigned r=0;r<count;++r) {
            if(r) { status=spool_row(s,&o->group,q->begin+r,rn,right); if(status) return status; }
            for(unsigned col=0;col<b->plan.nattrs;++col) {
                int slot=b->plan.cols[col].slot;
                if(slot>=0) b->cells[(size_t)slot*QS_VECTOR_ROWS+r]=col<ln?o->left[col]:right[col-ln];
            }
        }
        b->count=count; b->position=0;
    evaluate_merge:;
        struct qs_vector_input input={.cells=q->expr.cells,.dictionary=s->dictionary,
            .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
        if(projection_execute(&q->expr,NULL,NULL,0,input)) return -5;
        q->expr.position=0; o->position+=q->expr.count;
    }
    struct projection_batch *b=&q->expr;
    while(b->position<b->count) {
        unsigned r=b->position,index=b->plan.predicate;
        if(b->vectors[index].errors[r]) return -5;
        struct qs_cell keep=qs_vector_cell(&b->plan,b->vectors,index,r);
        if(!keep.isnull && keep.value) {
            int64_t status=spool_row(s,&o->group,q->begin+r,rn,values+ln); if(status) return status;
            memcpy(values,o->left,ln*sizeof(*values)); ++b->position; return 2;
        }
        ++b->position;
    }
    b->position=b->count=0; return 0;
}
#endif
void projection_append(struct projection_batch *b,const struct qs_cell *values)
{
    for(unsigned col=0;col<b->plan.nattrs;++col) {
        int slot=b->plan.cols[col].slot;
        if(slot>=0) b->cells[(size_t)slot*QS_VECTOR_ROWS+b->input_count]=values[col];
    }
    ++b->input_count;
}
/* Compact only requested results, after filters and output expressions have
 * run on surviving rows. Text stays a borrowed view for lazy Join payloads. */
static void projection_select(struct projection_batch *b,const uint16_t *rows,unsigned count)
{
    struct qs_cell values[QS_VECTOR_ROWS]; uint8_t errors[QS_VECTOR_ROWS];
    for(unsigned index=0;index<b->plan.nexprs;++index) if(b->needed[index]&2) {
        struct qs_vector *v=&b->vectors[index];
        if(v->kind==QS_CONSTANT) { v->done=count; continue; }
        if(v->kind==QS_TEXT) {
            v->all_valid=1;
            for(unsigned i=0;i<count;++i) {
                unsigned row=rows[i],error=v->errors[row],null=error || qs_vector_null(v,row);
                if(!null) v->strings[i]=v->strings[row];
                else { v->strings[i]=(struct qs_text_view){0}; }
                v->nulls[i]=null; v->errors[i]=error; v->all_valid&=!null;
            }
            v->done=count; continue;
        }
        int narrow=1;
        for(unsigned i=0;i<count;++i) {
            errors[i]=v->errors[rows[i]];
            values[i]=errors[i]?(struct qs_cell){.isnull=1}:qs_vector_cell(&b->plan,b->vectors,index,rows[i]);
            if(!values[i].isnull && (values[i].scale!=b->plan.exprs[index].scale ||
                values[i].value<INT64_MIN || values[i].value>INT64_MAX)) narrow=0;
        }
        v->kind=narrow?QS_NARROW:QS_DYNAMIC; v->all_valid=1; v->done=count;
        for(unsigned i=0;i<count;++i) {
            if(narrow) v->narrow[i]=values[i].isnull?0:(int64_t)values[i].value;
            else v->values[i]=values[i].value;
            v->nulls[i]=values[i].isnull; v->errors[i]=errors[i];
            v->scales[i]=values[i].scale; v->dscales[i]=values[i].dscale;
            v->all_valid&=!errors[i] && !values[i].isnull;
        }
    }
}
static int projection_execute_impl(struct projection_batch *b,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,struct qs_vector_input input)
{
    unsigned count=b->count;
    if(!b->nfilters) return qs_vector_execute_input(&b->plan,chunk,selection,first,count,b->needed,b->vectors,input)?-5:0;
    uint8_t dependencies[QS_EXPRS];
    for(unsigned i=0;i<b->plan.nexprs;++i) dependencies[i]=b->needed[i]&1;
    if(qs_vector_execute_input(&b->plan,chunk,selection,first,count,dependencies,b->vectors,input)) return -5;
    input.plan=&b->plan; input.vectors=b->vectors; input.count=count;
    uint16_t rows[QS_VECTOR_ROWS]; for(unsigned i=0;i<count;++i) rows[i]=i;
    unsigned kept=count,first_error=count;
    for(unsigned f=0;f<b->nfilters && kept;++f) {
        int index=b->filters[f].expression;
        if(qs_vector_evaluate(&input,index,rows,kept)) return -5;
        unsigned next=0;
        for(unsigned i=0;i<kept;++i) {
            unsigned row=rows[i];
            if(b->vectors[index].errors[row]) {
                if(row<first_error) { first_error=row; b->error=-5; b->error_node=b->filters[f].node; }
            } else if(row<first_error && !qs_vector_null(&b->vectors[index],row) && qs_vector_value(&b->vectors[index],row))
                rows[next++]=row;
        }
        kept=next;
    }
    for(unsigned i=0;i<b->plan.nexprs;++i) if((b->needed[i]&2) && kept && qs_vector_evaluate(&input,i,rows,kept)) return -5;
    if(kept!=count) projection_select(b,rows,kept);
    b->count=kept; return 0;
}
int projection_execute(struct projection_batch *b,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,struct qs_vector_input input)
{
    struct execution *s=input.set_state;
    if(s && s->paged) { input.text=s->text; input.text_io=s->io; input.resume=b->retry; }
    b->retry_input=input; b->retry_chunk=chunk; b->retry_selection=selection;
    b->retry_first=first; b->retry_count=b->count;
    int status=projection_execute_impl(b,chunk,selection,first,input);
    if(status && input.text) text_status(s,qs_dictionary_text_request(input.text));
    b->retry=status && s && s->expression_wait;
    if(b->input_cursor && !b->retry) {
        if(!status) *b->input_cursor+=b->retry_count;
        b->input_cursor=NULL;
    }
    return status;
}
/* A Sort Source already produces evaluated cells in complete batches. Keep
 * its buffer alive until the dependent expression vectors have been consumed. */
/* Consumers claim disjoint spool ranges with private read caches. The
 * materialized relation remains available for every CTE Scan stage. */
void projection_clear(struct execution *s,struct projection_batch **slot)
{
    struct projection_batch *b=*slot; if(!b) return;
    cs_work_free(s->context,b->cells); cs_work_free(s->context,b->vectors);
    cs_work_free(s->context,b->group_input); cs_work_free(s->context,b); *slot=NULL;
}
/* Return 3 only for an internal scalar fallback, before consuming input. */
int64_t projection_fill(struct execution *s,unsigned id,int source)
{
    struct projection_batch **slot=source?&s->nodes[id].source_projection:&s->nodes[id].projection;
    struct projection_batch *b=*slot;
    if(b->materializing) return materialize_outputs(s,b)?-3:2;
    if(b->retry) {
        b->count=b->retry_count;
        if(projection_execute(b,b->retry_chunk,b->retry_selection,b->retry_first,b->retry_input)) return -5;
        b->position=0;
        if(b->finish_vectors && b->finish_vectors(s,b)) return -5;
        if(!b->lazy_text && materialize_outputs(s,b)) return -3;
    }
    while(b->position==b->count) {
        if(b->error) { s->control->error=b->error; s->control->error_node=b->error_node; return b->error; }
        int64_t status=b->read_input(s,b);
        if(status!=2) {
            if((!status && !(s->block_graph && b->joined)) || (b->joined && status==3)) {
                projection_clear(s,slot);
            }
            return status;
        }
        if(b->filtered_scan && !s->nodes[b->scan].scan_filter) {
            projection_clear(s,slot); return 3;
        }
        /* A retained output must outlive this vector's byte views. Predicates
         * do not intern their temporary slices; projected text does. */
        if(b->finish_vectors && b->finish_vectors(s,b)) return -5;
        if(!b->lazy_text && materialize_outputs(s,b)) return -3;
    }
    return 2;
}
/* Feed one producer's evaluated columns straight into the existing run
 * storage. Keep the batch and cursor fixed until its spill request completes. */
int64_t projection_next(struct execution *s,unsigned id,struct qs_cell *row,int source)
{
    int64_t status=projection_fill(s,id,source);
    if(status==3) return next_row_impl(s,id,row);
    if(status!=2) return status;
    struct projection_batch *b=source?s->nodes[id].source_projection:s->nodes[id].projection;
    for(unsigned i=0;i<b->plan.nout;++i) {
        unsigned index=b->plan.outputs[i].index;
        if(b->vectors[index].errors[b->position]) return -5;
        row[i]=qs_vector_cell(&b->plan,b->vectors,index,b->position);
    }
    ++b->position; ++s->metrics.matched; return 2;
}
/* Reference the producer's expression columns, hash only its keys, then
 * scatter directly into the compact build rows. No intermediate AFDM rows.
 * A full table leaves the vector cursor for the existing growth/spill path. */
/* Retained packed rows are immutable until all producers finish. Claim
 * small ranges once and preserve the cursor across STORE reentry. */
/* External Hash consumes unordered build rows. Each CU owns its partition
 * tails and STORE continuation; no producer packet needs a host round trip. */
/* Full blocks can be adopted without reading them: order within a Hash
 * partition is irrelevant. Only bounded partial tails need row copying. */

/* Selective integer joins can discard bucket collisions in the probe
 * batch without unpacking the build row. Leave the first exact match
 * and the rest of its chain to the regular output/residual machinery. */
/* The common signed integer key needs no scale normalization or payload
 * cell. Gather projected-expression errors by column so those checks stay
 * outside the hot hash loop and retain their original row order. */
/* Hash packed keys in a window, then load the buckets separately so the
 * hardware can overlap independent lookups as in the native vector probe. */
/* Probe the bucket array as a batch, keeping column payloads borrowed until
 * a candidate needs them. Match-chain state remains in the join operator, so
 * one input can produce arbitrarily many output batches without rereading it. */
/* A payload text view becomes a canonical ID only for a matching row.
 * Cache it with the other retained cells for duplicate build-side matches. */
/* Retain the compact integer-key path. Other keys add only their actual cell
 * count and grow from a small table; spill value/count states, never raw rows. */
/* Import the indexed side's expressions once, with columns referencing the
 * decoded match chunk (its first two slots are lookup key and source row). */
/* Decode one candidate in the original consumption order. A zero result is
 * rejected by the indexed side; negative results remain deferred per row. */
/* index_scan.c calls this consumer with a batch of decoded heap matches.
 * Only the lookup key is narrowed to int64; left payloads keep 128-bit cells. */
#ifdef QP_SPLIT_RUNTIME
static int64_t next_row_impl(struct execution *s,unsigned id,struct qs_cell *row)
{
    if(id>=s->plan->count) return -1;
    struct operator_state *node=&s->nodes[id];
    if(s->worker && (int)id==s->source_node) {
        const struct operator_state *source=graph_state_owner(s,id);
        return source->read_row?source->read_row(s,id,row):-1;
    }
    if(node->finished && !projection_pending(node->projection) && !projection_pending(node->source_projection)) {
        projection_clear(s,&node->projection);
        projection_clear(s,&node->source_projection);
        return 0;
    }
    if(!s->hash_input && !s->worker && s->plan->workers>1 && s->plan->nodes[id].parallel)
        return parallel_next(s,id,row);
    if(node->projection && !(id==s->plan->root && s->raw_projection))
        return projection_next(s,id,row,0);
    return node->read_row?node->read_row(s,id,row):-1;
}
#else
static int64_t next_row_impl(struct execution *s,unsigned id,struct qs_cell *row)
{
    if(id>=s->plan->count) return -1;
    struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    if(s->worker && (int)id==s->source_node) {
        if(n->kind==QP_AGGREGATE && s->owner->partition_join<0)
            return aggregate_worker_source(s,id,row);
        unsigned bytes=hash_row_bytes(&n->expr),block_rows=QS_SORT_IO_BYTES/bytes;
        if(s->source_at==s->source_end) {
            s->source_at=atomic_fetch_add(&s->owner->next_offset,block_rows);
            if(s->source_at>=s->source.rows) return 0;
            s->source_end=s->source_at+block_rows;
            if(s->source_end>s->source.rows) s->source_end=s->source.rows;
        }
        unsigned char packed[QS_OUT*19];
        int64_t status=spool_row_bytes(s,&s->source,s->source_at,bytes,packed); if(status) return status;
        hash_load(row,packed,&n->expr); ++s->source_at; return 2;
    }
    if(o->finished && !projection_pending(o->projection) && !projection_pending(o->source_projection)) {
        projection_clear(s,&o->projection); projection_clear(s,&o->source_projection); return 0;
    }
    if(!s->hash_input && !s->worker && s->plan->workers>1 && n->parallel) return parallel_next(s,id,row);
    if(id==s->plan->root && s->raw_projection) {
        return scan_next(s,id,row);
    }
    if(n->kind==QP_SCAN || n->kind==QP_SORT || (n->kind==QP_PROJECT && n->left>=0) || n->kind==QP_JOIN || n->kind==QP_INDEX_JOIN) {
        int status=prepare_projection(s,id,0); if(status) return status;
        if(o->projection) return projection_next(s,id,row,0);
    }
    if(n->kind==QP_SCAN) return scan_next(s,id,row);
    if(n->kind==QP_JOIN) return n->hash?hash_next(s,id,row):join_next(s,id,row);
    if(n->kind==QP_INDEX_JOIN) return index_next(s,id,row,NULL);
    if(n->kind==QP_AGGREGATE) {
        int status=prepare_projection(s,id,1); if(status) return status;
        if(o->source_projection) return projection_next(s,id,row,1);
        return aggregate_next_impl(s,id,row,0);
    }
    if(n->kind==QP_PROJECT && n->left<0) {
        if(o->emitted++) return 0;
        int count=project(s,n,NULL,NULL,row);
        return count<0?count:count?2:0;
    }
    if(n->kind==QP_MATERIALIZED) {
        int64_t status=materialized_row(s,id,o->position,row); if(status==2) ++o->position; return status;
    }
    for(;;) {
        int64_t status;
        if(n->kind==QP_SORT)
            status=sorted_input(s,&o->left_sort,n->left,n->expr.norder,NULL,n->expr.order,o->left);
        else
        if(n->kind==QP_CTE_SCAN) {
            status=materialized_row(s,n->left,o->position,o->left); if(status==2) ++o->position;
        } else {
            if(n->kind==QP_LIMIT && n->expr.limit>=0 && o->emitted>=(uint64_t)n->expr.limit) return 0;
            status=next_row(s,n->left,o->left);
        }
        if(status!=2) return status;
        if(n->kind==QP_LIMIT && o->skipped<(uint64_t)n->expr.offset) { ++o->skipped; continue; }
        int count=project(s,n,o->left,NULL,row); if(count<0) return count;
        if(count) { ++o->emitted; return 2; }
    }
}
#endif
/* Build the read-only side once, then give each CU its own probe cursor,
 * index cache and AFDM output. The host schedules these ordinary CSF calls. */
static void close_node(struct operator_state *node)
{
    graph_close_sort(&node->left_sort);
    graph_close_sort(&node->right_sort);
    if(node->close) node->close(node);
}

static void close_workers(struct execution *s)
{
    for(unsigned w=0;w<s->plan->workers;++w) if(s->workers[w]) {
        struct execution *e=s->workers[w];
        for(unsigned i=0;i<s->plan->count;++i) close_node(&e->nodes[i]);
        e->control->state=0; cs_work_end(e->context); s->workers[w]=NULL;
    }
}
/* Like DuckDB's LocalSortState: local sinks own runs, while the coordinator
 * retains their handles for one merge Source. Allocate the first runs before
 * launching CUs, so every started sink can evict one of its own full runs. */
#ifndef QP_SPLIT_RUNTIME
static bool block_numeric_output(const struct qp_node *n)
{
    for(unsigned i=0;i<n->expr.nout;++i)
        if(n->expr.outputs[i].index<0 || n->expr.exprs[n->expr.outputs[i].index].type==Q_STRING) return false;
    return true;
}
/* Join and projection can carry a canonical string ID without opening
 * its dictionary page. Keep operators that evaluate string expressions
 * on the ordinary resumable path. */
static bool block_passes_text(const struct qp_node *n)
{
    if(n->kind!=QP_JOIN && n->kind!=QP_PROJECT) return block_numeric_output(n);
    for(unsigned i=0;i<n->expr.nexprs;++i) {
        const struct qs_expr *e=&n->expr.exprs[i];
        if(e->type==Q_STRING && e->op!=Q_COLUMN) return false;
        if(e->op==Q_LIKE || e->op==Q_SUBSTRING || (e->op>=Q_EQ && e->op<=Q_GE &&
           (n->expr.exprs[e->left].type==Q_STRING || n->expr.exprs[e->right].type==Q_STRING))) return false;
    }
    return true;
}
static bool block_group_supported(const struct qp_node *n)
{
    for(unsigned i=0;i<n->expr.naggs;++i) {
        const struct qs_agg *a=&n->expr.aggs[i];
        if((a->kind==Q_MIN || a->kind==Q_MAX) && n->expr.exprs[a->expr].type==Q_STRING) return false;
    }
    return true;
}
static bool block_chain_supported(const struct execution *s,unsigned root,int source)
{
    const struct qp_plan *p=s->plan;
    unsigned count=0;
    for(int at=root;at!=source;at=p->nodes[at].left) {
        if(at<0 || ++count>QP_GRAPH_BLOCK_MAX_CHAIN) return false;
        const struct qp_node *n=&p->nodes[at];
        if((!s->stream_waves && !s->resident_dictionary && !block_passes_text(n)) || !n->expr.nout) return false;
        if(n->kind==QP_PROJECT) continue;
        if(n->kind==QP_JOIN && n->hash && s->nodes[at].hash_phase && !s->nodes[at].external &&
           !s->nodes[at].hash_fallback) continue;
        if(n->kind==QP_INDEX_JOIN && n->right>=0 && p->files[n->right].count &&
           p->indexes[n->right].count && (s->stream_waves || s->resident_dictionary || block_numeric_output(&p->nodes[n->right]))) continue;
        return false;
    }
    return count!=0;
}
#endif
/* An explicit graph names the destination operator. Its blocking inputs
 * have already completed, so a device call cannot recursively build them. */
int64_t parallel_wave_next(struct execution *s,unsigned root,struct qs_cell *row,
    const struct graph_wave_sink *sink)
{
    bool explicit_sink=sink!=NULL;
    struct qp_plan *p=s->plan;
#ifndef QP_SPLIT_RUNTIME
restart:
#endif
    if(!s->parallel_started || s->parallel_node!=root) {
#ifdef QP_SPLIT_RUNTIME
        unsigned route=sink?(uint32_t)((uint64_t)s->args->cparam1>>QP_GRAPH_PIPELINE_ROUTE_SHIFT):0;
        int source=(int)(route&255)-1;
        if(source<0 || (unsigned)source>=p->count) {
            p->nodes[root].parallel=0;
            return row && !explicit_sink?next_row(s,root,row):3;
        }
        const struct operator_state *input=&s->nodes[source];
        if(input->admit_source) {
            int status=input->admit_source(s,source,source==(int)root);
            if(status) return status;
        }
#else
        int source=qp_pipeline_source(p,root);
        if(source<0) { p->nodes[root].parallel=0; return row && !explicit_sink?next_row(s,root,row):3; }
        int builds[QP_NODES]; unsigned count=0;
        for(int id=root;id>=0 && id!=source;id=p->nodes[id].left)
            if(p->nodes[id].kind==QP_JOIN) builds[count++]=id;
        /* Finish deeper build dependencies first. If a parent falls back to
         * Merge Join, its left input must not start another parallel phase
         * while the parent's unfinished right input still owns the buffers. */
        for(unsigned i=count;i;--i) {
            unsigned id=builds[i-1];
            if(explicit_sink && (!s->nodes[id].hash_phase || s->nodes[id].external)) return -1;
            int64_t status=explicit_sink?0:hash_build(s,id); if(status) return status;
            if(s->nodes[id].external && s->nodes[id].finished) {
                for(unsigned j=0;j<count;++j) {
                    struct operator_state *o=&s->nodes[builds[j]];
                    hash_release(s->context,o);
                }
                s->control->parallel=0; s->nodes[root].finished=1; return 0;
            }
            if(s->nodes[id].hash_fallback==1) {
                p->nodes[root].parallel=0;
                return row && !explicit_sink?next_row(s,root,row):3;
            }
        }
        /* Preparing an aggregate Source can resume its child wave. Keep
         * that wave's spill cursor until this pipeline is ready to start. */
        int partition_join=-1;
        for(unsigned i=0;i<count;++i)
            if(s->nodes[builds[i]].external && !s->nodes[builds[i]].external->direct) { partition_join=builds[i]; break; }
        if(p->nodes[source].kind==QP_AGGREGATE && partition_join<0) {
            if(explicit_sink) {
                if(s->nodes[source].group_phase<2) return -1;
            } else {
                struct blocking_timer timer={0}; if(s->profiling) timer=blocking_begin(s);
                int64_t status=hash_aggregate_next_impl(s,source,NULL,GROUP_PREPARE,0);
                if(s->profiling) blocking_end(s,QS_AGG_SINK,timer);
                if(status!=3) return status;
            }
            if(!s->nodes[source].group_radix && (s->nodes[source].group_phase!=3 || !s->nodes[source].groups)) {
                p->nodes[root].parallel=0; return row && !explicit_sink?next_row(s,root,row):3;
            }
            /* A direct Source has no expensive probe chain to distribute.
             * Base its task count on real groups, not a large PG estimate. */
            if(!s->nodes[source].group_radix && source==(int)root && atomic_load(s->nodes[source].group_count)<QP_VECTOR_ROWS) {
                p->nodes[root].parallel=0; return row && !explicit_sink?next_row(s,root,row):3;
            }
        }
        if(p->nodes[source].kind==QP_CTE_SCAN && !explicit_sink) return -1;
        s->partition_join=partition_join;
#endif
        s->wave_source=source;
        unsigned hash_sink=0,group_sink=0,aggregate_sink=0,partition_sink=0,set_sink=0;
        if(sink) {
            hash_sink=sink->hash;
            group_sink=sink->group;
            aggregate_sink=sink->aggregate;
            partition_sink=sink->partition;
            set_sink=sink->set;
        }
#ifndef QP_SPLIT_RUNTIME
        else {
            if(!s->graph_source_pending) for(unsigned i=0;i<p->count;++i) {
                const struct qp_node *parent=&p->nodes[i];
                if(parent->kind==QP_AGGREGATE && parent->left==(int)root &&
                   !parent->nkeys && !parent->distinct && !parent->distinct_aggs)
                    aggregate_sink=i+1;
            }
            if(!s->graph_source_pending) for(unsigned i=0;i<p->count;++i)
                if(p->nodes[i].kind==QP_AGGREGATE && p->nodes[i].left==(int)root &&
                   p->nodes[i].hash && s->nodes[i].groups && !s->nodes[i].group_phase)
                    group_sink=i+1;
            if(!s->graph_source_pending) for(unsigned i=0;i<p->count;++i)
                if(p->nodes[i].kind==QP_JOIN && p->nodes[i].right==(int)root &&
                   s->nodes[i].hash_data && !s->nodes[i].hash_phase && !s->nodes[i].hash_fallback)
                    hash_sink=i+1;
            if(!s->graph_source_pending && !hash_sink) for(unsigned i=0;i<p->count;++i)
                if(p->nodes[i].kind==QP_JOIN && s->nodes[i].external &&
                   ((!s->nodes[i].external->phase && p->nodes[i].right==(int)root) ||
                    (s->nodes[i].external->phase==1 && p->nodes[i].left==(int)root)))
                    partition_sink=i+1;
        }
#endif
        if(sink && sink->prepare) {
            int status=sink->prepare(s,sink->node,root,source);
            if(status) return status;
        }
#ifndef QP_SPLIT_RUNTIME
        else if(!sink && hash_sink) {
            int status=prepare_hash_wave(s,hash_sink,root,source,false);
            if(status) return status;
        }
#endif
        for(unsigned w=0;w<p->workers;++w) {
            struct cs_work_context_stats stats;
            if(cs_work_get_context_stats(s->context,&stats)) return -4;
            const struct qp_node *leaf=&p->nodes[source];
            size_t private_bytes=sizeof(*s)+p->count*sizeof(*s->nodes)+sizeof(struct qs_plan)+256;
            if(leaf->ordered_index) private_bytes+=QS_INDEX_WORK_BYTES+sizeof(struct qs_plan);
            if(partition_sink) private_bytes+=HASH_PARTITIONS*(QS_SORT_IO_BYTES+sizeof(struct spool));
#ifdef QP_SPLIT_RUNTIME
            size_t spill_reserve=sink?sink->reserve_bytes:0;
#else
            size_t spill_reserve=0;
            for(unsigned i=0;i<p->count;++i)
                if(p->nodes[i].kind==QP_AGGREGATE && p->nodes[i].left==(int)root && s->nodes[i].group_capacity)
                    spill_reserve=sort_spill_reserve(s);
#endif
            /* Cursor/filter state is allocated lazily by the worker. Keep
             * that space and the existing spill reserve before launching;
             * otherwise a successful launch can fail on its first page. */
            if(cs_work_available(s->context)<(p->workers-w)*private_bytes+spill_reserve) {
                close_workers(s); p->workers=1;
                return row && !explicit_sink?next_row(s,root,row):3;
            }
            cs_work_context *context=cs_work_begin_child(s->context,stats.limit_bytes);
            struct execution *e=context?cs_work_calloc(context,1,sizeof(*e)):NULL;
            if(e) e->nodes=cs_work_calloc(context,p->count,sizeof(*e->nodes));
            if(!e || !e->nodes) {
                if(context) cs_work_end(context);
                close_workers(s); p->nodes[root].parallel=0;
                return row && !explicit_sink?next_row(s,root,row):3;
            }
            e->projection_ops=s->projection_ops; e->context=context; e->plan=p; e->owner=s; e->raw_projection=s->raw_projection; e->worker=w+1; e->root=root;
            e->hash_sink=hash_sink; e->group_sink=group_sink; e->aggregate_sink=aggregate_sink;
            e->partition_sink=partition_sink; e->set_sink=set_sink;
            if(partition_sink) {
                e->partitions=cs_work_calloc(context,HASH_PARTITIONS,sizeof(*e->partitions));
                if(!e->partitions) { cs_work_end(context); close_workers(s); return -4; }
            }
            e->control=s->control+w+1; e->dictionary=s->dictionary; e->arena=s->arena;
            e->paged=s->paged; e->literal_ids=s->literal_ids; e->dictionary_ready=1;
            if(e->paged) {
                e->text=qs_dictionary_text_create(e->paged);
                if(!e->text) { cs_work_end(context); close_workers(s); p->nodes[root].parallel=0; return row && !explicit_sink?next_row(s,root,row):3; }
            }
            e->io_bytes=QS_SORT_IO_BYTES;
            e->sort_limit=s->sort_limit; e->profiling=s->profiling; e->initializer=p->ninit;
            e->wave_source=source;
            e->source_node=-1;
#ifdef QP_SPLIT_RUNTIME
            if(input->admit_source) e->source_node=source;
#else
            if(p->nodes[source].kind==QP_AGGREGATE) e->source_node=source;
            if(s->partition_join>=0) {
                const struct external_hash *h=s->nodes[s->partition_join].external;
                e->source_node=p->nodes[s->partition_join].left;
                e->source=h->probe[h->partition]; e->source.cache=NULL; e->source.loaded=UINT_MAX;
            }
#endif
            memcpy(e->params,s->params,sizeof(e->params));
            *e->control=(struct qp_control){.state=(uintptr_t)e}; s->workers[w]=e;
#ifndef QP_SPLIT_RUNTIME
            if(leaf->kind==QP_SCAN) *(struct qs_chunk *)(s->arena+vector_offset(e,leaf))=
                (struct qs_chunk){.capacity=leaf->ordered_index?256:qp_scan_capacity(p,leaf),.ncols=leaf->expr.ncols};
#endif
        }
#ifndef QP_SPLIT_RUNTIME
        if(partition_sink) {
            int supported=1,source=p->nodes[root].kind==QP_AGGREGATE;
            for(unsigned w=0;w<p->workers;++w) {
                struct execution *e=s->workers[w];
                int status=prepare_projection(e,root,source); if(status<0) return status;
                if(!(source?e->nodes[root].source_projection:e->nodes[root].projection)) supported=0;
            }
            if(!supported) {
                partition_sink=0;
                for(unsigned w=0;w<p->workers;++w) s->workers[w]->partition_sink=0;
            }
        }
#endif
        if(sink && sink->start) {
            int status=sink->start(s,sink->node,root); if(status) return status;
        }
#ifdef QP_SPLIT_RUNTIME
        if(input->start_source) input->start_source(s,source);
#endif
#ifndef QP_SPLIT_RUNTIME
        else if(!sink) {
            if(group_sink) { int status=parallel_group_begin(s,group_sink-1); if(status) return status; }
            if(!s->graph_source_pending && !hash_sink && !group_sink && !aggregate_sink && !partition_sink && !set_sink)
                parallel_sort_begin(s,root);
        }
        bool sort_sink=s->workers[0]->sort_sink && (s->stream_waves || block_numeric_output(&p->nodes[root]));
        unsigned block_flags=0;
        const struct qp_node *producer=&p->nodes[root],*leaf=&p->nodes[source];
        bool hash_probe=producer->kind==QP_JOIN && producer->left==source && !s->nodes[root].external;
        bool index_probe=producer->kind==QP_INDEX_JOIN && producer->left==source && producer->right>=0 &&
            p->files[producer->right].count && p->indexes[producer->right].count &&
            (s->stream_waves || s->resident_dictionary || block_numeric_output(&p->nodes[producer->right]));
        if(s->block_graph && (hash_sink || group_sink || aggregate_sink || sort_sink || partition_sink || set_sink) &&
           leaf->kind==QP_SCAN &&
           !leaf->ordered_index &&
           (s->stream_waves || s->resident_dictionary || (block_numeric_output(leaf) && block_passes_text(producer) &&
            (!group_sink || block_group_supported(&p->nodes[group_sink-1])) &&
            (!aggregate_sink || block_group_supported(&p->nodes[aggregate_sink-1]))))) {
            bool direct=(hash_probe || index_probe) && !sort_sink && !partition_sink;
            bool chain=root!=(unsigned)source && !direct && block_chain_supported(s,root,source);
            if(root==(unsigned)source || direct || chain)
                block_flags=QP_GRAPH_BLOCK_SCAN | (hash_sink?QP_GRAPH_BLOCK_BUILD:0) | (set_sink?QP_GRAPH_BLOCK_SET:0) |
                    (direct && hash_probe?QP_GRAPH_BLOCK_PROBE:0) | (direct && index_probe?QP_GRAPH_BLOCK_INDEX:0) |
                    (chain?QP_GRAPH_BLOCK_CHAIN:0) | (sort_sink?QP_GRAPH_BLOCK_SORT:0) |
                    (partition_sink?QP_GRAPH_BLOCK_PARTITION:0);
            if(s->stream_waves) {
                if(block_flags && !partition_sink)
                    block_flags|=QP_GRAPH_BLOCK_STREAM;
                else block_flags=0;
            }
        }
        if(explicit_sink && s->stream_waves && (leaf->kind==QP_AGGREGATE || leaf->kind==QP_CTE_SCAN || (leaf->ordered_index&8)) &&
           (hash_sink || group_sink || aggregate_sink || (sort_sink && explicit_sink && p->nodes[root].kind!=QP_AGGREGATE)) &&
           (root==(unsigned)source || block_chain_supported(s,root,source))) {
            block_flags=QP_GRAPH_BLOCK_SCAN|QP_GRAPH_BLOCK_STREAM|QP_GRAPH_BLOCK_DEVICE_SOURCE|
                (hash_sink?QP_GRAPH_BLOCK_BUILD:0)|(sort_sink?QP_GRAPH_BLOCK_SORT:0)|
                (root==(unsigned)source?0:QP_GRAPH_BLOCK_CHAIN);
        }
        if(block_flags) {
            for(unsigned w=0;w<p->workers;++w) {
                struct execution *e=s->workers[w];
                e->block_graph=block_flags!=0;
                e->stream_block=(block_flags&QP_GRAPH_BLOCK_STREAM)!=0;
                e->block_chain=(block_flags&QP_GRAPH_BLOCK_CHAIN)!=0;
                if(!block_flags || e->stream_block) continue;
                struct qs_chunk *chunk=(void *)(s->arena+vector_offset(e,&p->nodes[source]));
                chunk->capacity=qp_scan_buffer_bytes(p,&p->nodes[source])/24;
            }
        }
#else
        for(unsigned w=0;w<p->workers;++w) {
            struct execution *e=s->workers[w];
            e->block_graph=e->stream_block=(route&QP_GRAPH_BLOCK_STREAM)!=0;
            e->block_chain=(route&QP_GRAPH_BLOCK_CHAIN)!=0;
        }
#endif
        /* An explicit graph consumer takes the packet in a later E9
         * submission. Workers still split and decode the scan morsels. */
        s->parallel_node=root; s->parallel_done=0;
        memset(s->parallel_position,0,sizeof(s->parallel_position)); atomic_store(&s->next_offset,0);
        s->control->parallel=hash_sink || group_sink || partition_sink || set_sink || (explicit_sink && aggregate_sink)?2:1; ++s->control->epoch; s->control->available=s->control->drained=0;
#ifdef QP_SPLIT_RUNTIME
        s->control->scan_node=(route&QP_GRAPH_BLOCK_SCAN)?route:0;
#else
        s->control->scan_node=0;
        int scan=qp_probe_scan(p,root);
        bool scheduled=s->partition_join<0 && scan>=0 && !p->nodes[scan].ordered_index;
        /* Index probes retain their keys/payloads across namespace waits.
         * They can consume the same independently scheduled scan morsels as
         * hash probes; only the retained Heap window ends at a morsel edge. */
        if(scheduled) s->control->scan_node=(scan+1)|block_flags|
            ((block_flags&QP_GRAPH_BLOCK_CHAIN)?(root+1)<<QP_GRAPH_BLOCK_ROOT_SHIFT:0);
        if(block_flags&QP_GRAPH_BLOCK_DEVICE_SOURCE)
            s->control->scan_node=(source+1)|block_flags|((root+1)<<QP_GRAPH_BLOCK_ROOT_SHIFT);
#endif
        s->parallel_started=1; s->used_workers=p->workers;
        if(s->profiling) { s->pipeline_start=now_ns(CLOCK_MONOTONIC); s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID); }
    }
    for(unsigned w=0;w<p->workers;++w) if(s->control->available&(UINT64_C(1)<<w)) {
        struct qs_result *out=&s->outputs[w+1];
        /* Direct HOST delivery has already consumed the CU's final packet.
         * The coordinator only collects EOF/metrics, never its typed rows. */
        if(p->packed_result==QP_DIRECT_RESULT) s->parallel_position[w]=out->rows;
        if(s->parallel_position[w]<out->rows) {
            if(!row) return 2; /* Borrow this batch without advancing it. */
            unsigned cols=p->nodes[root].expr.nout;
            memcpy(row,(struct qs_cell *)out->data+(size_t)s->parallel_position[w]++*cols,cols*sizeof(*row));
            return 2;
        }
        if(out->finished) s->parallel_done|=UINT64_C(1)<<w;
        s->parallel_position[w]=0; s->control->available&=~(UINT64_C(1)<<w); s->control->drained|=UINT64_C(1)<<w;
    }
    if(s->parallel_done==qp_worker_mask(p->workers)) {
        for(unsigned w=s->parallel_combine_worker;w<p->workers;++w) {
            struct execution *e=s->workers[w];
            struct spool *overflow=&e->block_overflow;
            if(e->block_overflow_read<overflow->rows) {
                if(!row) return 2;
                int64_t status=spool_row(s,overflow,e->block_overflow_read,p->nodes[root].expr.nout,row);
                if(status) return status;
                ++e->block_overflow_read;
                return 2;
            }
            while(overflow->blocks) {
                struct spool_block *block=&overflow->pages[--overflow->blocks];
                if(block->token) return csNeedFree(block->token);
            }
            if(overflow->cache) {
                cs_work_free(s->context,overflow->cache); overflow->cache=NULL;
            }
        }
        struct qp_pipeline_profile *phase=&p->pipelines[s->group_combine?s->group_combine-1:root];
        if(!s->parallel_combining && s->profiling) {
            phase->wall_ns+=now_ns(CLOCK_MONOTONIC)-s->pipeline_start;
            phase->coordinator_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-s->pipeline_tick; ++phase->rounds;
        }
        s->parallel_combining=1;
        for(unsigned w=s->parallel_combine_worker;w<p->workers;++w,s->parallel_combine_worker=w) {
            const struct qs_result *v=&s->outputs[w+1];
#ifndef QP_SPLIT_RUNTIME
            if(s->workers[w]->partition_sink) {
                int64_t status=external_partition_combine(s,s->workers[w]); if(status) return status;
            }
#endif
            if(sink && sink->merge) {
                int64_t status=sink->merge(s,sink->node,s->workers[w]); if(status) return status;
            }
#ifndef QP_SPLIT_RUNTIME
            else if(!sink && s->workers[w]->aggregate_sink) {
                unsigned id=s->workers[w]->aggregate_sink-1;
                int status=aggregate_combine(s,id,&s->workers[w]->nodes[id]); if(status) return status;
            }
#endif
            if(s->profiling) {
                phase->worker_ns+=v->profile.operators[QS_JOIN_PROBE].cpu_ns;
                uint64_t work=v->profile.operators[QS_JOIN_PROBE].cpu_ns;
                phase->worker_wall_ns+=v->profile.operators[QS_JOIN_PROBE].wall_ns;
                if(!phase->worker_cpu_min || work<phase->worker_cpu_min) phase->worker_cpu_min=work;
                const struct execution *task=s->workers[w];
                if(work>phase->worker_cpu_max) {
                    phase->worker_cpu_max=work;
                    phase->slow_worker=w+1;
                    phase->slow_worker_input_rows=v->metrics.scanned;
                    phase->slow_worker_sink_rows=task->sink_rows;
                    phase->slow_worker_returned_rows=task->sink_returned_rows;
                }
                phase->input_rows+=v->metrics.scanned;
                phase->sink_returned_rows+=task->sink_returned_rows;
                if(task->hash_sink || task->group_sink)
                    phase->sink_node=task->hash_sink?task->hash_sink:task->group_sink;
                if(s->workers[w]->finished_ns) phase->finished_idle_ns+=now_ns(CLOCK_MONOTONIC)-s->workers[w]->finished_ns;
                phase->sink_ns+=s->workers[w]->sink_ns; phase->sink_rows+=s->workers[w]->sink_rows;
                for(unsigned reason=0;reason<QP_WAITS;++reason) {
                    phase->wait_ns[reason]+=s->workers[w]->wait_ns[reason];
                    phase->wait_calls[reason]+=s->workers[w]->wait_calls[reason];
                }
            }
            s->metrics.build_rows+=v->metrics.build_rows;
            s->metrics.scanned+=v->metrics.scanned; s->metrics.matched+=v->metrics.matched;
            s->metrics.input_bytes+=v->metrics.input_bytes-v->metrics.index_pages*QS_PAGE;
            s->metrics.index_pages+=v->metrics.index_pages; s->metrics.index_tuples+=v->metrics.index_tuples;
            for(unsigned kind=0;kind<2;++kind) {
                s->metrics.index_cache_hits[kind]+=v->metrics.index_cache_hits[kind];
                s->metrics.index_cache_misses[kind]+=v->metrics.index_cache_misses[kind];
            }
            s->metrics.spill_read_bytes+=v->metrics.spill_read_bytes;
            s->metrics.spill_write_bytes+=v->metrics.spill_write_bytes;
            s->metrics.runs+=v->metrics.runs;
            s->metrics.spilled_runs+=v->metrics.spilled_runs;
            for(unsigned op=0;op<QS_OPERATORS;++op) {
                struct qs_timing *t=&s->profile.operators[op]; const struct qs_timing *x=&v->profile.operators[op];
                t->calls+=x->calls; t->wall_ns+=x->wall_ns; t->cpu_ns+=x->cpu_ns;
            }
        }
        s->parallel_combine_worker=0; s->parallel_combining=0;
        if(sink && sink->finish) {
            int status=sink->finish(s,sink->node,root); if(status) return status;
        }
#ifndef QP_SPLIT_RUNTIME
        else if(!sink && s->workers[0]->group_sink) {
            int status=finish_group_workers(s,s->workers[0]->group_sink-1,root);
            if(status) return status;
        }
#endif
        close_workers(s);
        int source=s->wave_source;
        if(source>=0 && s->partition_join<0 && s->nodes[source].finish_source)
            s->nodes[source].finish_source(s,source);
        s->parallel_started=0; s->control->parallel=0;
#ifdef QP_SPLIT_RUNTIME
        for(int id=root;id>=0 && id!=source;id=p->nodes[id].left)
            if(s->nodes[id].finish_wave) s->nodes[id].finish_wave(s,id);
#else
        if(s->partition_join>=0) {
            external_advance(s,s->partition_join); s->partition_join=-1;
            goto restart;
        }
        for(int id=root;id>=0 && id!=source;id=p->nodes[id].left) if(p->nodes[id].kind==QP_JOIN) {
            hash_release(s->context,&s->nodes[id]);
        }
#endif
        s->nodes[root].finished=1; return 0;
    }
    return QP_WAIT;
}
int64_t parallel_next(struct execution *s,unsigned root,struct qs_cell *row)
{ return parallel_wave_next(s,root,row,NULL); }

static void close_operators(struct execution *s)
{
    if(s->operators_closed) return;
    s->operators_closed=1;
    for(unsigned i=0;i<s->plan->count;++i) close_node(&s->nodes[i]);
    close_workers(s);
}
void close_execution(struct execution *s)
{
    close_operators(s);
    if(s->paged && !s->owner) qs_dictionary_abort(s->paged);
    cs_work_context *context=s->context; s->control->state=0; cs_work_end(context);
}
/* BUSY completes this graph call without publishing a partial result.
 * Keep its row count private until the same batch can continue. */
static int64_t dictionary_request(struct execution *s,struct qs_result *out,int64_t status)
{
    wait_io(s,status);
    if(status!=QS_DICTIONARY_BUSY) return status;
    s->buffered_rows=out->rows; s->buffered_finished=out->finished; s->buffered=1;
    out->rows=out->finished=0;
    s->control->output_rows=s->control->finished=0;
    return s->worker?QP_RESOURCE_WAIT:0;
}
static int64_t finish_dictionary(struct execution *s,struct femu_csd_args *a,struct qs_result *out)
{
    close_operators(s);
    int64_t status=qs_dictionary_close(s->paged,s->initial_access,s->io,s->closing>1?csReentryValue(a):0);
    if(status) {
        s->closing=status==QS_DICTIONARY_BUSY?1:2;
        out->finished=0; s->control->output_rows=s->control->finished=0;
        return dictionary_request(s,out,status);
    }
    s->paged=NULL; out->finished=s->control->finished=1; s->control->output_rows=out->rows;
    close_execution(s); return 0;
}
/* Resource discovery reuses this CSF: selector 0 reports workmem bytes,
 * selector 1 the launcher's effective per-CU CPU fraction in ppm. No query
 * allocations or extra CSF entry are needed. */
int64_t pg_sina_workmem_capacity(struct femu_csd_args *a)
{
    struct cs_work_stats stats;
    if(!a || a->numr || a->cparam2) return -1;
    if(a->cparam1==QP_RESOURCE_CPU_RATIO) {
        const char *value=getenv("CSD_CU_CPU_RATIO"); char *end=NULL;
        double ratio=value?strtod(value,&end):1;
        if(value && (!*value || *end || !(ratio>=0 && ratio<=1))) return -1;
        if(ratio==0) ratio=1; /* 0 and 1 both mean unrestricted. */
        if(ratio<0.1) return -1;
        return (int64_t)(ratio*QP_CPU_RATIO_SCALE+0.5);
    }
    if(a->cparam1!=QP_RESOURCE_WORKMEM || cs_work_get_stats(&stats) || stats.capacity_bytes>INT64_MAX) return -1;
    return stats.capacity_bytes;
}
/* plan, lifecycle/page requests, result, input/dictionary arena, spill I/O.
 * A successful call either returns result rows or asks PG to LOAD pages.
 * NEED_STORE/LOAD is handled entirely by the existing SNIA batch scheduler. */
int64_t initialize_dictionary(struct execution *s,struct femu_csd_args *a)
{
    if(s->dictionary_ready) return 0;
    const struct qs_dict *config=(const void *)s->arena;
    uint64_t completion=s->initial_wait>QS_DICTIONARY_BUSY?csReentryValue(a):0;
    for(;s->imported<=config->count;++s->imported) {
        const char *bytes; unsigned length; uint32_t id=0;
        if(qs_dict_get(config,s->imported,&bytes,&length)) return -3;
        int64_t status=qs_dictionary_insert(s->paged,s->initial_access,bytes,length,&id,s->io,completion);
        s->initial_wait=status; if(status) return status;
        completion=0; if(id!=s->imported) return -3;
    }
    for(;s->literal_node<s->plan->count;++s->literal_node,s->literal_string=0) {
        const struct qs_plan *p=&s->plan->nodes[s->literal_node].expr;
        if(p->nstrings>QS_STRINGS) return -1;
        for(;s->literal_string<p->nstrings;++s->literal_string) {
            const struct qs_string *v=&p->strings[s->literal_string]; uint32_t id=0;
            if(v->length>sizeof(v->bytes)) return -1;
            int64_t status=qs_dictionary_insert(s->paged,s->initial_access,v->bytes,v->length,&id,s->io,completion);
            s->initial_wait=status; if(status) return status;
            completion=0; s->literal_ids[s->literal_node*QS_STRINGS+s->literal_string]=id;
        }
    }
    s->dictionary_ready=1; return 0;
}
/* Translate this SQL program's immutable file mapping into generic device
 * reads. Neither FEMU nor SNIA sees the file/extent interpretation. */
static int namespace_ranges(struct execution *s,const struct qp_file_mapping *map,
    uint64_t at,uint64_t bytes,uint64_t target,uint32_t pool_offset,unsigned *count)
{
    if(!map->count) return 0;
    if(!map->nsid || !map->lba_bytes || map->reserved || map->offset<sizeof(*s->plan) ||
       map->offset>(uint64_t)s->args->mr_len[0] ||
       map->count>((uint64_t)s->args->mr_len[0]-map->offset)/sizeof(struct qs_extent) ||
       bytes>UINT64_MAX-at) return -1;
    const struct qs_extent *extents=(void *)((unsigned char *)s->plan+map->offset);
    struct femu_graph_read *reads=s->io;
    unsigned lo=0,hi=map->count;
    while(lo<hi) {
        unsigned mid=lo+(hi-lo)/2;
        if(extents[mid].logical+extents[mid].bytes<=at) lo=mid+1; else hi=mid;
    }
    uint64_t end=at+bytes;
    for(unsigned i=lo;i<map->count && at<end;++i) {
        const struct qs_extent *e=&extents[i];
        if(e->logical>at || e->bytes>UINT64_MAX-e->logical) return -1;
        uint64_t n=end<e->logical+e->bytes?end-at:e->logical+e->bytes-at;
        if(!n || (at-e->logical)%map->lba_bytes || n%map->lba_bytes || n/map->lba_bytes>UINT32_MAX) return -1;
        uint64_t lba=e->lba+(at-e->logical)/map->lba_bytes;
        struct femu_graph_read *last=*count?&reads[*count-1]:NULL;
        if(last && last->pool_offset==htole32(pool_offset) && le32toh(last->nsid)==map->nsid &&
            le64toh(last->lba)+le32toh(last->blocks)==lba &&
            le64toh(last->offset)+(uint64_t)le32toh(last->blocks)*map->lba_bytes==target &&
            n/map->lba_bytes<=UINT32_MAX-le32toh(last->blocks))
            last->blocks=htole32(le32toh(last->blocks)+n/map->lba_bytes);
        else {
            if(*count==FEMU_CSD_GRAPH_MAX_NODES) return 0;
            reads[(*count)++]=(struct femu_graph_read){.nsid=htole32(map->nsid),.blocks=htole32(n/map->lba_bytes),
                .afdm_index=htole32(3),.pool_offset=htole32(pool_offset),
                .lba=htole64(lba),.offset=htole64(target)};
        }
        at+=n; target+=n;
    }
    return at==end?1:-1;
}
int64_t namespace_wait(struct execution *s)
{
    struct qp_control *c=s->control;
    if(c->ready || (!c->bytes && !c->index_nreads)) return 0;
    if(c->node>=s->plan->count) return -1;
    const struct qp_node *n=&s->plan->nodes[c->node];
    unsigned count=0; int status;
    if(c->index_nreads) {
        if(c->index_nreads>QS_INDEX_READS || (n->kind!=QP_INDEX_JOIN && !n->ordered_index)) return -1;
        unsigned leaf=n->ordered_index?c->node:(unsigned)n->right;
        if(leaf>=s->plan->count) return -1;
        size_t input=input_offset(s,n),pool_offset=input+QS_INDEX_POOL_OFFSET;
        if(pool_offset>UINT32_MAX || input>(uint64_t)s->args->mr_len[3] ||
           QS_INDEX_IO_BYTES>(uint64_t)s->args->mr_len[3]-input) return -1;
        struct femu_graph_read_pool *pool=(void *)(s->arena+pool_offset);
        pool->slot_bytes=htole32(QS_PAGE);
        pool->slots=htole32(QS_INDEX_SLOTS);
        pool->pin_limit=htole32(QS_INDEX_READS);
        for(unsigned i=0;i<c->index_nreads;++i) {
            const struct qs_index_read *r=&c->index_reads[i];
            size_t target;
            if(r->heap>1 || !qp_index_page_offset(input,r->slot,&target)) return -1;
            const struct qp_file_mapping *map=r->heap?&s->plan->files[leaf]:&s->plan->indexes[leaf];
            status=namespace_ranges(s,map,(uint64_t)r->block*QS_PAGE,QS_PAGE,target,pool_offset,&count);
            if(status<=0) return status;
        }
    } else {
        status=namespace_ranges(s,&s->plan->files[c->node],c->offset,c->bytes,input_offset(s,n),0,&count);
        if(status<=0) return status;
    }
    s->pending_nvm=count;
    return csNeedNvmRead(count);
}

/* Resolve immutable Heap references only at the HOST return boundary.
 * Use the existing internal READ protocol and private I/O AFDM; no index
 * lookup, process, host round trip or retained LOAD buffer is required. */
static int output_text(struct execution *s,struct qs_cell value,const char **bytes,unsigned *length)
{
    if(!qs_cell_late_text(&value)) return text_get(s,(unsigned)value.value,bytes,length);
    unsigned __int128 ref=(unsigned __int128)value.value;
    unsigned node=(ref>>80)&127,n=(ref>>64)&65535;
    uint64_t offset=(uint64_t)ref,block=offset/QS_PAGE;
    if(node>=s->plan->count || n>QS_PAGE-offset%QS_PAGE || offset>s->plan->nodes[node].bytes ||
       n>s->plan->nodes[node].bytes-offset) return -3;
    if(!s->late_page) s->late_page=cs_work_alloc(s->context,QS_PAGE);
    if(!s->late_page) return -4;
    if(!s->late_valid || s->late_node!=node || s->late_block!=block) {
        unsigned count=0;
        int status=namespace_ranges(s,&s->plan->files[node],block*QS_PAGE,QS_PAGE,QS_PAGE,0,&count);
        if(status!=1 || !count || s->io_bytes<2*QS_PAGE) return -3;
        struct femu_graph_read *reads=s->io;
        for(unsigned i=0;i<count;++i) reads[i].afdm_index=htole32(s->args->numr-1);
        s->late_node=node; s->late_block=block; s->late_valid=0;
        s->late_wait=csNeedNvmRead(count); return -3;
    }
    *bytes=(const char *)s->late_page+offset%QS_PAGE; *length=n; return 0;
}

#ifndef QP_SPLIT_RUNTIME
/* A partially exported row remains in the existing result AFDM. Only
 * completed columns contain text; the remaining ones still contain IDs. */
static int export_row(struct execution *s,const struct qs_plan *root,struct qs_cell *row,struct qs_result *out)
{
    for(unsigned col=s->output_column;col<root->nout;++col) {
        if(!row[col].isnull && !qs_cell_string(&row[col]) && !qs_cell_is_string_ref(&row[col]) &&
           root->exprs[root->outputs[col].index].type==Q_STRING) {
            const char *bytes; unsigned length;
            if(output_text(s,row[col],&bytes,&length)) {
                s->output_column=col; s->output_pending=1; return -3;
            }
            if(length>255 || out->str_bytes+length>QS_RESULT_STRING_BYTES) { text_release(s); return -3; }
            if(length && length<=QS_STRING_MAX) qs_cell_set_string(&row[col],bytes,length);
            else {
                memcpy(out->str_data+out->str_bytes,bytes,length);
                qs_cell_set_string_ref(&row[col],out->str_bytes,length); out->str_bytes+=length;
            }
            if(text_release(s)) return -3;
        }
    }
    s->output_column=0; s->output_pending=0; return 0;
}

static int output_string_order(const void *a,const void *b)
{
    const struct output_string *x=a,*y=b;
    return x->id<y->id?-1:x->id>y->id;
}
/* Consume Sort's run-owned payload before its next Source batch can replace
 * the buffer. Expressions continue to use canonical IDs; only final output
 * cells become inline strings or references into the result AFDM. */
static void export_sort_text(struct execution *s,const struct qs_plan *root,
    struct qs_cell *row,struct qs_result *out)
{
    for(unsigned col=0;col<root->nout;++col) {
        struct qs_cell *v=&row[col];
        if(v->isnull || qs_cell_late_text(v) || root->exprs[root->outputs[col].index].type!=Q_STRING || qs_cell_string(v) || qs_cell_is_string_ref(v)) continue;
        unsigned n; const char *bytes=sort_text_get(s,(uint32_t)v->value,&n);
        if(!bytes || n>255 || out->str_bytes+n>QS_RESULT_STRING_BYTES) continue;
        if(n && n<=QS_STRING_MAX) qs_cell_set_string(v,bytes,n);
        else {
            memcpy(out->str_data+out->str_bytes,bytes,n);
            qs_cell_set_string_ref(v,out->str_bytes,n); out->str_bytes+=n;
        }
    }
}
/* IDs and body locations are appended together. Resolve this fixed-size
 * batch in ID order, then write each value back to its original row/column.
 * This groups directory/body reads without changing the SQL result order. */
static int export_batch(struct execution *s,const struct qs_plan *root,struct qs_result *out)
{
    if(!s->output_active) {
        s->output_count=s->output_position=0;
        for(unsigned row=s->output_exported;row<out->rows;++row)
            for(unsigned col=0;col<root->nout;++col) {
                const struct qs_cell *v=qp_result_row(out,row,root->nout,s->plan->packed_result)+col;
                if(!v->isnull && !qs_cell_late_text(v) && !qs_cell_string(v) && !qs_cell_is_string_ref(v) && root->exprs[root->outputs[col].index].type==Q_STRING)
                    s->output_order[s->output_count++]=(struct output_string){(uint32_t)v->value,row,col};
            }
        qsort(s->output_order,s->output_count,sizeof(*s->output_order),output_string_order);
        s->output_active=1;
    }
    for(;s->output_position<s->output_count;++s->output_position) {
        const struct output_string *at=&s->output_order[s->output_position];
        const char *bytes; unsigned length;
        if(text_get(s,at->id,&bytes,&length)) return -3;
        if(length>255 || out->str_bytes+length>QS_RESULT_STRING_BYTES) { text_release(s); return -3; }
        struct qs_cell *v=qp_result_row(out,at->row,root->nout,s->plan->packed_result)+at->column;
        if(length && length<=QS_STRING_MAX) qs_cell_set_string(v,bytes,length);
        else {
            memcpy(out->str_data+out->str_bytes,bytes,length);
            qs_cell_set_string_ref(v,out->str_bytes,length); out->str_bytes+=length;
        }
        if(text_release(s)) return -3;
    }
    for(;s->output_exported<out->rows;++s->output_exported)
        if(export_row(s,root,qp_result_row(out,s->output_exported,root->nout,s->plan->packed_result),out)) return -3;
    s->output_active=0; return 0;
}

#endif

/* Encode one boundary row before its input or Sort source buffer is reused.
 * A dictionary wait retains this row and completed columns. If the variable
 * arena is full, publish the current packet and retry this already encoded
 * row against the next empty packet. */
int host_append(struct execution *s,const struct qs_plan *p,struct qs_result *out)
{
    for(;s->host_column<p->nout;++s->host_column) {
        unsigned col=s->host_column,oid=p->outputs[col].type_oid;
        const struct qs_cell *v=&s->host_row[col]; s->host_values[col]=0;
        if(v->isnull) continue;
        if(!qp_host_variable(oid)) {
            if(oid==20 && (v->value>INT64_MAX || v->value<INT64_MIN)) return -3;
            if(oid==701) { double value=(double)v->value; memcpy(&s->host_values[col],&value,8); }
            else s->host_values[col]=(uint64_t)v->value;
            continue;
        }
        unsigned offset=(s->host_var_bytes+3)&~3U,bytes=0;
        unsigned char *to=s->host_var+offset;
        if(oid==1700) {
            if(s->raw_projection) {
                unsigned body=(unsigned)((unsigned __int128)v->value>>64);
                if(body>sizeof(s->host_var)-offset-4) return -3;
                bytes=body+4; uint32_t header=bytes<<2; memcpy(to,&header,4);
                memcpy(to+4,(const void *)(uintptr_t)v->value,body);
            } else bytes=qp_host_numeric(to,*v);
        } else {
            const char *text; unsigned length,kind=p->exprs[p->outputs[col].index].type;
            char single=(char)v->value;
            if(kind==Q_CHAR) { text=&single; length=1; }
            else if(s->raw_projection) { text=(const void *)(uintptr_t)v->value; length=(unsigned)((unsigned __int128)v->value>>64); }
            else if(qs_cell_string(v)) { text=qs_cell_string_bytes(v); length=qs_cell_string(v); }
            else if(output_text(s,*v,&text,&length)) return -3;
            unsigned padded=oid==1042 && p->outputs[col].typmod>4?(unsigned)p->outputs[col].typmod-4:length;
            if(padded<length) padded=length;
            if(padded>255 || padded+4>sizeof(s->host_var)-offset) { text_release(s); return -3; }
            bytes=padded+4; uint32_t header=bytes<<2; memcpy(to,&header,4);
            memcpy(to+4,text,length); memset(to+4+length,' ',padded-length);
            if(text_release(s)) return -3;
        }
        s->host_values[col]=offset; s->host_var_bytes=offset+bytes;
    }
    unsigned base=(out->str_bytes+3)&~3U;
    if(s->host_var_bytes && s->host_var_bytes>sizeof(out->str_data)-base) return 1;
    if(s->host_var_bytes) memcpy(out->str_data+base,s->host_var,s->host_var_bytes);
    for(unsigned col=0;col<p->nout;++col) {
        unsigned width=s->host_widths[col];
        uint64_t value=s->host_values[col];
        if(qp_host_variable(p->outputs[col].type_oid)) value+=base;
        memcpy(s->host_columns[col]+(size_t)out->rows*width,&value,width);
        if(s->host_row[col].isnull) s->host_nulls[col][out->rows/8]|=1U<<(out->rows%8);
    }
    out->str_bytes=base+s->host_var_bytes; ++out->rows;
    s->host_pending=s->host_column=s->host_var_bytes=0;
    return 0;
}

void host_begin(struct execution *s,const struct qs_plan *p,struct qs_result *out,unsigned capacity)
{
    for(unsigned col=0;col<p->nout;++col) {
        s->host_widths[col]=qp_host_width(p->outputs[col].type_oid);
        s->host_columns[col]=qp_host_values(out,p,capacity,col);
        s->host_nulls[col]=s->host_columns[col]+(size_t)capacity*s->host_widths[col];
        memset(s->host_nulls[col],0,(capacity+7)/8);
    }
}
void plan_output_metrics(struct execution *s,struct qs_result *out,unsigned worker)
{
    const struct qp_plan *plan=s->plan;
    out->metrics=s->metrics; out->metrics.workers=s->used_workers?s->used_workers:1; out->profile=s->profile;
    struct qs_dictionary_stats dictionary;
    qs_dictionary_get_stats(s->paged,&dictionary);
    out->metrics.dictionary_entries=dictionary.strings; out->metrics.dictionary_bytes=dictionary.bytes;
    if(!worker) {
        out->metrics.spill_read_bytes+=dictionary.loads*QS_DICTIONARY_PAGE_BYTES;
        out->metrics.spill_write_bytes+=dictionary.stores*QS_DICTIONARY_PAGE_BYTES;
    }
    for(unsigned i=0;i<plan->count;++i) if(s->nodes[i].metrics)
        s->nodes[i].metrics(&s->nodes[i],&out->metrics,&out->profile);
    out->metrics.input_bytes+=out->metrics.index_pages*QS_PAGE;
    struct cs_work_context_stats memory;
    if(!cs_work_get_context_stats(s->context,&memory)) out->metrics.work_peak=memory.peak_used_bytes;
}
int open_execution(struct femu_csd_args *a,struct qp_plan *plan,
    struct qp_control *control,const struct graph_projection_ops *ops,
    graph_query_initializer initialize,struct execution **opened)
{
    if(!plan->count || plan->count>QP_NODES || plan->root>=plan->count) return -1;
    struct cs_work_stats stats; if(cs_work_get_stats(&stats)) return -4;
    size_t budget=plan->nodes[plan->root].expr.work_mem_bytes;
    if(!budget || budget>stats.capacity_bytes) budget=stats.capacity_bytes;
    cs_work_context *context=cs_work_begin_limit(budget); if(!context) return -4;
    struct execution *s=cs_work_calloc(context,1,sizeof(*s)); if(!s) { cs_work_end(context); return -4; }
    s->projection_ops=ops; s->context=context; s->plan=plan; s->control=control; s->raw_projection=qp_host_raw_projection(plan);
    if(qp_host_parameter_bytes(plan)) {
        const char *values=(const char *)plan+sizeof(*plan);
        for(unsigned i=0;i<plan->nparams;++i)
            if(plan->param_nodes[i]==QP_HOST_PARAMETER)
                memcpy(&s->params[i],values+(size_t)i*sizeof(struct qs_cell),sizeof(struct qs_cell));
    }
    s->partition_join=s->source_node=-1;
    s->arena=a->mr_addr[3]; s->io=a->mr_addr[a->numr-1]; s->io_bytes=a->mr_len[a->numr-1];
    s->nodes=cs_work_calloc(context,plan->count,sizeof(*s->nodes));
    if(!s->nodes) { cs_work_end(context); return -4; }
#ifdef QP_SPLIT_RUNTIME
    unsigned divisor=((uint64_t)a->cparam1>>QP_GRAPH_PIPELINE_BUDGET_SHIFT)&255;
    if(divisor<2) { cs_work_end(context); return -1; }
#else
    unsigned divisor=qp_sort_depth(plan,plan->root)+2;
#endif
    s->sort_limit=budget/divisor; if(s->sort_limit<512*1024) { cs_work_end(context); return -4; }
    s->literal_ids=cs_work_calloc(context,plan->count*QS_STRINGS,sizeof(*s->literal_ids));
    unsigned text_columns=0;
    const struct qs_plan *output=&plan->nodes[plan->root].expr;
    for(unsigned col=0;col<output->nout;++col)
        text_columns+=output->exprs[output->outputs[col].index].type==Q_STRING;
    if(text_columns && plan->packed_result<QP_TYPED_RESULT) {
        s->output_order=cs_work_calloc(context,(size_t)qp_result_capacity(output,plan->packed_result)*text_columns,sizeof(*s->output_order));
        if(!s->output_order) { cs_work_end(context); return -4; }
    }
    s->stream_waves=((uint64_t)a->cparam1&QP_GRAPH_STREAM_EXEC)!=0;
    s->resident_dictionary=((uint64_t)a->cparam1&QP_GRAPH_RESIDENT_TEXT)!=0;
    s->paged=s->resident_dictionary?qs_dictionary_create_resident(context):qs_dictionary_create(context);
    if(s->paged) { s->text=qs_dictionary_text_create(s->paged); s->initial_access=qs_dictionary_access_create(s->paged); }
    if(!s->literal_ids || !s->paged || !s->text || !s->initial_access) {
        if(s->paged) qs_dictionary_abort(s->paged);
        cs_work_end(context); return -4;
    }
    s->profiling=a->cparam1&1;
#ifndef QP_SPLIT_RUNTIME
    if(!initialize) initialize=graph_index_init;
#endif
    if(initialize) initialize(s,budget);
    s->imported=1; s->outputs=a->mr_addr[2]; control->state=(uintptr_t)s;
    *opened=s;
    return 0;
}

int resume_page_io(struct execution *s,struct femu_csd_args *a)
{
    if(s->late_wait) {
        if(!csIsReentry(a) || csReentryValue(a)!=((uint64_t)s->late_wait&CS_IO_VALUE_MASK)) return -1;
        memcpy(s->late_page,(unsigned char *)s->io+QS_PAGE,QS_PAGE);
        s->late_valid=1; s->late_wait=0;
    }
    if(s->pending_nvm) {
        if(!csIsReentry(a) || csReentryValue(a)!=s->pending_nvm) return -1;
        s->pending_nvm=0; s->control->ready=1;
    }
    return 0;
}

int resume_spool(struct execution *s,struct femu_csd_args *a)
{
    if(s->pending_spool) {
        if(!csIsReentry(a)) return -1;
        struct spool *p=s->pending_spool;
        unsigned bytes=spool_block_bytes(p);
        if(s->pending_store) {
            p->pages[p->blocks++]=(struct spool_block){.token=csReentryValue(a)};
            p->used=0; s->metrics.spill_write_bytes+=bytes;
        }
        else { memcpy(p->cache,s->io,bytes); p->loaded=s->pending_block; s->metrics.spill_read_bytes+=bytes; }
        s->pending_spool=NULL;
    }
    return 0;
}

/* Independent SNIA HashBuild, consuming a packed packet already present in
 * AFDM. The caller serializes uses of the shared coordinator context. */

int64_t graph_finish(struct execution *s,struct femu_csd_args *a,struct qs_result *out)
{
    const struct qp_control *control=s->control;
    if(s->buffered) { out->rows=s->buffered_rows; out->finished=s->buffered_finished; s->buffered=0; }
    int64_t status=finish_dictionary(s,a,out);
    if(status) return status;
    if(control->state) return QP_RESOURCE_WAIT;
    return QP_WORKER_FINISHED | QP_INPUT_DRAINED | (out->rows?QP_OUTPUT_READY:0);
}

/* Borrow a worker's packed Scan output in contiguous spans. The worker's
 * slot remains live until parallel_next acknowledges its final row. */
#ifndef QP_SPLIT_RUNTIME
static int64_t graph_parallel_scan(struct execution *s,unsigned id,struct qs_result *out,unsigned capacity)
{
    unsigned cols=s->plan->nodes[id].expr.nout;
    while(out->rows<capacity) {
        int64_t status=parallel_next(s,id,NULL);
        if(status!=2) return status;
        unsigned w=0;
        while(w<s->plan->workers && (!(s->control->available&(UINT64_C(1)<<w)) ||
              s->parallel_position[w]>=s->outputs[w+1].rows)) ++w;
        if(w==s->plan->workers) return -1;
        const struct qs_result *input=&s->outputs[w+1];
        unsigned at=s->parallel_position[w],rows=input->rows-at;
        if(rows>capacity-out->rows) rows=capacity-out->rows;
        memcpy(qp_result_row(out,out->rows,cols,1),(const struct qs_cell *)input->data+(size_t)at*cols,
            (size_t)rows*cols*sizeof(struct qs_cell));
        out->rows+=rows; s->parallel_position[w]+=rows;
    }
    return 2;
}

#endif

/* Internal edges carry packed cells. The root uses the existing typed
 * PostgreSQL boundary encoder and closes the same workmem context. */
int64_t graph_emit(struct femu_csd_args *a,enum graph_emit_kind kind,const struct graph_result_source *source)
{
#ifdef QP_SPLIT_RUNTIME
    if(!source) return -1;
    const bool probe=false, fed=false;
#else
    bool probe=kind==GRAPH_EMIT_PROBE, fed=probe || kind==GRAPH_EMIT_FEED;
#endif
    if(!a || (a->numr!=6 && (fed || a->numr!=5)) || a->mr_len[0]<(long long)sizeof(struct qp_plan) ||
       a->mr_len[1]<(long long)sizeof(struct qp_control) ||
       a->mr_len[2]<(long long)sizeof(struct qs_result) ||
       a->mr_len[3]<(long long)sizeof(struct qs_dict) || a->mr_len[a->numr-1]<QS_SORT_IO_BYTES ||
       (a->numr==6 && a->mr_len[4]<(long long)sizeof(struct qs_result))) return -1;
    struct qp_plan *plan=a->mr_addr[0];
    struct qp_control *control=a->mr_addr[1];
    struct execution *s=(void *)(uintptr_t)control->state;
    if(control->close) { if(s) close_execution(s); return 0; }
    uint64_t parameter=(uint64_t)a->cparam1;
    bool typed=(parameter&QP_GRAPH_HOST_OUTPUT)!=0, init=(parameter&QP_GRAPH_INIT_OUTPUT)!=0;
    uint64_t id=(parameter&~(QP_GRAPH_HOST_OUTPUT|QP_GRAPH_INIT_OUTPUT))>>QP_GRAPH_SOURCE_NODE_SHIFT;
    if(plan->count>QP_NODES || id>=plan->count || plan->nparams>QP_PARAMS || plan->ninit>QP_PARAMS ||
       (uint64_t)a->mr_len[0]<sizeof(*plan)+qp_host_parameter_bytes(plan) ||
       plan->workers>QS_WORKERS || a->mr_len[1]<(long long)((plan->workers+1)*sizeof(*control)) ||
       (!s && a->mr_len[2]<(long long)((plan->workers+1)*sizeof(struct qs_result)))) return -1;
    unsigned cols=plan->nodes[id].expr.nout;
    if(!cols || cols>QS_OUT || (typed && (init || id!=plan->root || plan->packed_result!=QP_TYPED_RESULT ||
       (!fed && a->numr==6)))) return -1;
    const struct qs_plan *output=&plan->nodes[id].expr;
    struct qs_result *out=!fed && a->numr==6?a->mr_addr[4]:a->mr_addr[2];
    if(!s) {
#ifdef QP_SPLIT_RUNTIME
        return -1;
#else
        if(fed || csIsReentry(a)) return -1;
        int status=open_execution(a,plan,control,NULL,NULL,&s); if(status) return status;
#endif
    }
    if(s->plan!=plan || s->owner ||
       (s->graph_source_pending && (s->graph_source_node!=id || s->graph_source_kind!=kind))) return -1;
    s->args=a; s->io=a->mr_addr[a->numr-1]; s->io_bytes=a->mr_len[a->numr-1];
    if(s->closing) return typed?graph_finish(s,a,out):-1;
    const struct qp_node *n=&plan->nodes[id];
    struct operator_state *o=&s->nodes[id];
    if(source && !source->ready(s,id)) return -1;
    if(typed && s->initializer!=plan->ninit) return -1;
    int init_param=-1;
    if(init) {
        if(s->initializer>=plan->ninit || (unsigned)plan->init_nodes[s->initializer]!=id) return -1;
        init_param=plan->init_params[s->initializer];
        if((init_param<0)!=(n->kind==QP_SET) || init_param>=QP_PARAMS) return -1;
    }
    if(!fed && n->kind==QP_CTE_SCAN && (n->left<0 || (unsigned)n->left>=plan->count ||
       !s->nodes[n->left].group_ready)) return -1;
    const struct qs_result *in=fed?a->mr_addr[4]:NULL;
    if(fed) {
        if(n->left<0 || (unsigned)n->left>=plan->count) return -1;
        if(probe && (n->kind!=QP_JOIN || !n->hash || o->hash_fallback==1 ||
           (!o->hash_phase && !o->finished && (!o->external || o->external->phase<2)))) return -1;
        if(!probe && n->kind!=QP_AGGREGATE && n->kind!=QP_PROJECT && n->kind!=QP_SORT &&
           n->kind!=QP_LIMIT && n->kind!=QP_INDEX_JOIN && n->kind!=QP_MATERIALIZED &&
           (n->kind!=QP_SET || !init)) return -1;
        if(n->kind==QP_INDEX_JOIN && (n->right<0 || (unsigned)n->right>=plan->count)) return -1;
        unsigned input_cols=plan->nodes[n->left].expr.nout;
        if(!input_cols || input_cols>QS_OUT || in->status || in->finished>1 ||
           (uint64_t)in->begin+in->rows>QS_RESULT_ROWS*QS_OUT/input_cols ||
           (probe && o->external && !o->external->direct && (!in->finished || in->rows)) ||
           (o->graph_input_held && o->graph_input_position>in->rows) ||
           (!o->graph_input_held && csIsReentry(a))) return -1;
        if(!o->graph_input_held) {
            o->graph_input_position=o->graph_probe_count=0; o->graph_input_held=true;
        }
    }
    s->raw_projection=0;
    if(!csIsReentry(a)) {
        control->drained=0;
        if(parameter&QP_GRAPH_SOURCE_PAGES) control->ready=1;
    }
    wait_resume(s);
    unsigned capacity=typed?qp_host_capacity(output):QS_RESULT_ROWS*QS_OUT/cols;
    if(!s->graph_source_pending) {
        out->rows=out->begin=out->finished=out->str_bytes=0; out->status=0;
        s->graph_source_node=id; s->graph_source_kind=kind; s->graph_source_pending=true;
        if(typed) host_begin(s,output,out,capacity);
    }
    int64_t status=initialize_dictionary(s,a);
    if(status) goto wait;
    if(s->text_wait) {
        status=qs_dictionary_text_resume(s->text,s->io,s->text_wait>QS_DICTIONARY_BUSY?csReentryValue(a):0);
        s->text_wait=status; if(status) goto wait;
        s->expression_wait=0;
    }
    status=resume_page_io(s,a); if(status) return status;
    status=resume_spool(s,a); if(status) return status;
    if(s->profiling) s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID);
    if(fed) {
        s->hash_input=in; s->hash_input_node=n->left;
        s->hash_input_position=o->graph_input_position;
    }
#ifndef QP_SPLIT_RUNTIME
    if(n->kind==QP_SET) status=build_set(s,id);
    else if(n->kind==QP_MATERIALIZED) status=materialize(s,id);
    else
#endif
    {
        status=2;
#ifndef QP_SPLIT_RUNTIME
        if(!fed && !init && !typed && n->kind==QP_SCAN && n->parallel && plan->workers>1)
            status=graph_parallel_scan(s,id,out,capacity);
#endif
        if(status==3) status=2; /* Existing serial fallback. */
        while(status==2 && out->rows<capacity) {
            struct qs_cell *row=typed?s->host_row:qp_result_row(out,out->rows,cols,1);
            if(typed && s->host_pending) status=2;
#ifdef QP_SPLIT_RUNTIME
            else status=source->next(s,id,row);
#else
            else status=source?source->next(s,id,row):next_row(s,id,row);
#endif
            if(status!=2) break;
            if(init) {
                if(s->initializer_phase) { status=-8; break; }
                s->params[init_param]=row[0]; s->initializer_phase=1;
                continue;
            }
            if(!typed) { ++out->rows; continue; }
            s->host_pending=1;
            int appended=host_append(s,output,out);
            if(appended<0) status=appended;
            if(appended) break;
        }
    }
    if(fed) { o->graph_input_position=s->hash_input_position; s->hash_input=NULL; }
    if(s->late_wait) status=s->late_wait;
    if(s->text_wait) status=s->text_wait;
    if(s->dictionary_wait) status=s->dictionary_wait;
    if(s->expression_wait && s->pending_spool) {
        s->expression_wait=0;
        status=csNeedLoad(s->pending_spool->pages[s->pending_block].token);
    }
    if(s->profiling && s->parallel_started)
        plan->pipelines[s->group_combine?s->group_combine-1:s->parallel_node].coordinator_ns+=
            now_ns(CLOCK_THREAD_CPUTIME_ID)-s->pipeline_tick;
    if(status==QP_WAIT && !control->ready && (control->bytes || control->index_nreads)) {
        status=namespace_wait(s);
        /* Without a device file mapping PG supplies LOAD -> EXEC. Keep the
         * active edge private until that page request has completed. */
        if(!status) return QP_CONTROL_REQUIRED;
    }
    if(status==QP_WAIT && !fed && !typed && n->kind==QP_SCAN && s->parallel_started)
        return QP_CONTROL_REQUIRED; /* Continue filling this packet from the next ready CU. */
    bool drained=fed && (!status || (status==QP_WAIT && (!probe || !o->have_left) &&
        o->graph_input_position==in->rows));
    if(status<0 || status>2) goto wait;
    if(!status) {
        if(init) {
            if(init_param>=0 && !s->initializer_phase) s->params[init_param]=(struct qs_cell){.isnull=1};
            s->initializer_phase=0; ++s->initializer;
        }
        out->finished=1;
    }
    s->graph_source_pending=false;
    if(drained) o->graph_input_held=false;
    if(typed) {
        qp_host_compact(out,output,capacity);
        control->output_rows=out->rows; control->output_strings=out->str_bytes;
        control->finished=out->finished;
        plan_output_metrics(s,out,0);
        if(out->finished) { s->closing=1; return graph_finish(s,a,out); }
    }
    return (out->rows?QP_OUTPUT_READY:0) | (out->finished?QP_WORKER_FINISHED:0) |
        (drained?QP_INPUT_DRAINED:status==QP_WAIT?QP_CONTROL_REQUIRED:0);
wait:
    wait_io(s,status);
    return status==QS_DICTIONARY_BUSY?QP_RESOURCE_WAIT:status;
}

#ifndef QP_SPLIT_RUNTIME
int64_t pg_sina_graph_source(struct femu_csd_args *a)
{ return graph_emit(a,GRAPH_EMIT_SOURCE,NULL); }
/* Probe and consumer use distinct AFDMs. A full output packet can leave a
 * duplicate-match chain open, so only INPUT_DRAINED releases the left edge. */
int64_t pg_sina_graph_feed(struct femu_csd_args *a)
{ return graph_emit(a,GRAPH_EMIT_FEED,NULL); }

#endif

#ifndef QP_SPLIT_RUNTIME
/* Compatibility for the original packet-based block path. */
static unsigned block_columns(const struct execution *s,unsigned id,const struct projection_batch *b,int *indices)
{
    if(s->hash_sink || s->aggregate_sink) {
        for(unsigned col=0;col<b->plan.nout;++col) indices[col]=b->plan.outputs[col].index;
        return b->plan.nout;
    }
    const struct qp_node *n=&s->plan->nodes[id];
    unsigned count=0;
    for(unsigned k=0;k<n->nkeys;++k) indices[count++]=b->group_keys[k];
    for(unsigned a=0;a<n->expr.naggs;++a)
        if(b->group_values[a]>=0) indices[count++]=b->group_values[a];
    const struct operator_state *local=s->nodes[id].group_local;
    if(!local || !local->group_radix)
        for(unsigned col=0;col<b->plan.nout;++col) indices[count++]=b->plan.outputs[col].index;
    return count;
}
static int64_t block_probe(struct execution *s,unsigned id)
{
    int status=prepare_projection(s,id,0); if(status) return status;
    struct projection_batch *b=s->nodes[id].projection;
    if(!b || !b->joined) return -4;
    int columns[QS_KEYS+QS_AGGS+QS_OUT];
    unsigned count=block_columns(s,id,b,columns);
    const struct operator_state *local=s->nodes[id].group_local;
    size_t reserve=local && local->group_radix?local->group_radix->resident_reserve:2*QS_SORT_IO_BYTES;
    for(;;) {
        int64_t ready=projection_fill(s,id,0);
        if(ready==QP_WAIT || !ready) {
            if(ready==QP_WAIT && s->control->index_nreads && !s->control->ready) {
                int64_t request=namespace_wait(s);
                return request?request:-1;
            }
            if(!s->scan_drained) return -1;
            b->count=b->position=0;
            return 0;
        }
        if(ready!=2) return ready==3?-4:ready;
        while(b->position<b->count) {
            struct qs_cell cells[QS_KEYS+QS_AGGS+QS_OUT];
            for(unsigned col=0;col<count;++col) {
                int index=columns[col];
                if(index<0) { cells[col]=(struct qs_cell){0}; continue; }
                if(b->vectors[index].errors[b->position]) return -5;
                cells[col]=qs_vector_cell(&b->plan,b->vectors,index,b->position);
            }
            int64_t stored=spool_append_context(s,&s->block_rows,cells,count*sizeof(*cells),
                s->context,reserve);
            if(stored) return stored;
            ++b->position;
        }
    }
}
static int64_t block_apply(struct execution *s,unsigned id,struct projection_batch *b,struct qs_result *out)
{
    if(s->hash_sink) return block_hash_vectors(s,id,b);
    if(s->partition_sink) return partition_sink_vectors(s,b);
    if(s->sort_sink) return sort_sink_vectors(s,b);
    if(s->aggregate_sink) return aggregate_accept_vectors(s,id,NULL,0);
    unsigned capacity=QS_RESULT_ROWS*QS_OUT/b->plan.nout;
    if(out->rows==out->begin) {
        out->rows=out->begin=0;
        int64_t status=aggregate_accept_vectors(s,id,out,capacity);
        if(status) return status;
    }
    return graph_spool_result(s,out,b->plan.nout);
}
#endif

int64_t graph_spool_result(struct execution *s,struct qs_result *out,unsigned cols)
{
    while(out->begin<out->rows) {
        const struct qs_cell *row=qp_result_row(out,out->begin,cols,1);
        int64_t status=spool_append(s,&s->block_overflow,row,cols);
        if(status) return status;
        ++out->begin;
    }
    out->rows=out->begin=0;
    return 0;
}
static int64_t block_sink_scan(struct execution *s,unsigned id,struct qs_result *out,
    graph_sink_batch consume)
{
    int status=prepare_projection(s,id,0); if(status) return status;
    if(!s->nodes[id].projection) return -4;
    for(;;) {
        struct projection_batch *b=s->nodes[id].projection;
        if(out->begin<out->rows) {
            int64_t applied=consume(s,id,b,out);
            if(applied) return applied;
        }
        int64_t ready=projection_fill(s,id,0);
        if(ready==QP_WAIT || !ready) return s->scan_drained?0:-1;
        if(ready!=2) return ready==3?-4:ready;
        b=s->nodes[id].projection;
        do {
            int64_t applied=consume(s,id,b,out);
            if(applied) return applied;
        } while(b->position<b->count);
    }
}
#ifndef QP_SPLIT_RUNTIME
static int64_t block_sink(struct execution *s,unsigned id,struct qs_result *out)
{
    struct spool *rows=&s->block_rows;
    if(rows->rows) {
        int status=prepare_projection(s,id,0); if(status) return status;
        struct projection_batch *b=s->nodes[id].projection;
        if(!b) return -4;
        int columns[QS_KEYS+QS_AGGS+QS_OUT];
        unsigned count=block_columns(s,id,b,columns);
        if(!s->block_cells) s->block_cells=cs_work_alloc(s->context,QS_VECTOR_ROWS*count*sizeof(*s->block_cells));
        if(!s->block_cells) return -4;
        for(;;) {
            if(b->position==b->count && out->begin==out->rows) {
                b->position=b->count=0;
                while(b->count<QS_VECTOR_ROWS && s->block_read<rows->rows) {
                    int64_t loaded=spool_row(s,rows,s->block_read,count,s->block_cells+(size_t)b->count*count);
                    if(loaded) return loaded;
                    ++b->count; ++s->block_read;
                }
                if(!b->count) break;
            }
            for(unsigned col=0;col<count;++col) {
                if(columns[col]<0) continue;
                struct qs_vector *v=&b->vectors[columns[col]];
                v->kind=QS_CELLS; v->cells=s->block_cells+col; v->selection=NULL;
                v->first=0; v->cell_stride=count; v->all_valid=0;
                memset(v->errors,0,b->count);
            }
            unsigned before=b->position;
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            int64_t applied=block_apply(s,id,b,out);
            if(s->profiling) { s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick; s->sink_rows+=b->position-before; }
            if(applied) return applied;
        }
        while(rows->blocks) {
            struct spool_block *block=&rows->pages[--rows->blocks];
            cs_work_free(s->context,block->data);
            if(block->token) return csNeedFree(block->token);
        }
        rows->used=0; rows->rows=0; rows->loaded=UINT_MAX;
    }
    return 0;
}
/* Alternating spools are shared only by adjacent EXECs in the same slot.
 * Borrow the virtual input projection for the lifetime of this EXEC. */
static void block_use_input(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    s->block_input=&s->block_edges[s->block_write^1];
    s->block_input_node=id; s->block_edge_read=0;
    s->block_native_projection=o->projection;
    s->block_native_checked=o->projection_checked;
    s->block_native_deferred=o->projection_deferred;
    o->projection=o->block_input_projection;
    o->projection_checked=o->projection_deferred=0;
}
static int64_t block_finish_input(struct execution *s)
{
    struct spool *in=s->block_input;
    if(!in) return 0;
    while(in->blocks) {
        struct spool_block *block=&in->pages[--in->blocks];
        cs_work_free(s->context,block->data);
        if(block->token) return csNeedFree(block->token);
    }
    in->used=0; in->rows=0; in->loaded=UINT_MAX;
    struct operator_state *o=&s->nodes[s->block_input_node];
    o->block_input_projection=o->projection;
    o->projection=s->block_native_projection;
    o->projection_checked=s->block_native_checked;
    o->projection_deferred=s->block_native_deferred;
    s->block_input=NULL;
    return 0;
}
static int64_t block_transform(struct execution *s,unsigned id,unsigned scan)
{
    const struct qp_node *n=&s->plan->nodes[id];
    if(!csIsReentry(s->args)) {
        s->block_done=s->block_row_pending=false;
        if(n->left!=(int)scan) block_use_input(s,n->left);
    }
    struct spool *out=&s->block_edges[s->block_write];
    while(!s->block_done) {
        if(!s->block_row_pending) {
            int64_t status=next_row(s,id,s->block_row);
            if(status==QP_WAIT && s->control->index_nreads && !s->control->ready) {
                int64_t request=namespace_wait(s);
                return request?request:-1;
            }
            if(status==QP_WAIT && s->scan_drained) status=0;
            if(!status) { s->block_done=true; break; }
            if(status!=2) return status;
            s->block_row_pending=true;
        }
        int64_t stored=spool_append_context(s,out,s->block_row,n->expr.nout*sizeof(struct qs_cell),
            s->context,4*QS_SORT_IO_BYTES);
        if(stored) return stored;
        s->block_row_pending=false;
    }
    int64_t freed=block_finish_input(s); if(freed) return freed;
    s->block_write^=1;
    return 0;
}
static int64_t block_chain_sink(struct execution *s,unsigned sink,struct qs_result *out)
{
    if(!s->block_sink_started) {
        s->block_sink_started=true; s->block_done=false;
        block_use_input(s,s->root);
    }
    if(!s->block_done) {
        int64_t status=block_sink_scan(s,sink,out,block_apply); if(status) return status;
        s->block_done=true;
    }
    return block_finish_input(s);
}
#endif

/* A stream edge borrows the producer's vector until the consumer has
 * finished all its output. No whole-block intermediate is materialized. */
int graph_attach_stream_input(struct execution *s,unsigned scan,unsigned input)
{
    if(input>=s->plan->count) return -1;
    const struct operator_state *producer=&s->nodes[input];
    struct projection_batch *borrowed=producer->published_batch?*producer->published_batch:NULL;
    if(!borrowed && (input!=scan || !producer->decode_chunk)) return -4;
    s->stream_edge=borrowed; s->stream_edge_node=input;
    if(borrowed) s->scan_drained=borrowed->position==borrowed->count;
    return 0;
}

int graph_stream_node(const struct execution *s)
{
    uint64_t encoded=(uint64_t)s->args->cparam1>>QP_GRAPH_BLOCK_NODE_SHIFT;
    return encoded && encoded<=s->plan->count?(int)(encoded-1):-1;
}

static int64_t graph_sink_edge(struct execution *s,unsigned scan,unsigned id,struct qs_result *out,
    const struct graph_sink_ops *ops)
{
    unsigned input=ops->borrow_projection?s->root:(unsigned)s->plan->nodes[id].left;
    int64_t status=graph_attach_stream_input(s,scan,input);
    if(status) return status;
    if(ops->borrow_projection) {
        status=ops->consume(s,s->root,s->stream_edge,out);
        if(!status) status=graph_consume_input();
    } else {
        status=prepare_projection(s,id,0);
        if(status || !s->nodes[id].projection) { if(!status) status=-4; goto done; }
        for(;;) {
            status=projection_fill(s,id,0);
            if(status!=2) break;
            struct projection_batch *batch=s->nodes[id].projection;
            do {
                status=ops->consume(s,id,batch,out);
            } while(!status && batch->position<batch->count);
            if(status) goto done;
        }
        if(status==QP_WAIT && s->control->index_nreads && !s->control->ready) {
            int64_t request=namespace_wait(s);
            status=request?request:-1;
        } else if((status==QP_WAIT || !status) && s->scan_drained) {
            s->nodes[id].stream_inlet=false;
            status=graph_consume_input();
        } else if(status==3 || !status) status=-4;
    }
done:
    s->stream_edge=NULL;
    return status;
}

/* The LOAD region lives for the graph. The scan vector and probe projection
 * are separate, reusable buffers. Filter lends the scan vector downstream
 * and E9 returns its inlet credit after Probe or the sink has consumed it. */
int64_t graph_device_scan(struct execution *s,unsigned scan,
    int64_t (*read)(struct execution *,struct projection_batch *))
{
    const struct qp_node *leaf=&s->plan->nodes[scan];
    int64_t status=prepare_projection(s,scan,0); if(status) return status;
    struct projection_batch *batch=s->nodes[scan].projection;
    if(!batch) return -4;
    s->stream_source_window=false;
    if(leaf->expr.predicate<0) status=projection_fill(s,scan,0);
    else status=read(s,batch);
    if(status==QP_WAIT && s->control->index_nreads && !s->control->ready) {
        int64_t request=namespace_wait(s);
        return request?request:-1;
    }
    if(status==2) {
        s->nodes[scan].published_batch=&s->nodes[scan].projection;
        s->stream_source_window=leaf->expr.predicate>=0;
        s->scan_drained=false;
        return graph_publish_output();
    }
    if(!status) return graph_finish_stream();
    return status==3?-4:status;
}

int64_t graph_stream_sink(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out,
    const struct graph_sink_ops *ops)
{
    const struct qp_node *leaf=&s->plan->nodes[scan];
    bool device=!s->nodes[scan].decode_chunk;
    if(s->args->graph_input!=FEMU_GRAPH_INPUT_EOF) {
        if(device || s->block_chain) return graph_sink_edge(s,scan,sink,out,ops);
        struct projection_batch *batch=s->nodes[sink].projection;
        unsigned before=batch?batch->position:0;
        uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
        uint64_t built=s->metrics.build_rows;
        bool direct=s->root==scan;
        int64_t status;
        if(direct) status=block_sink_scan(s,sink,out,ops->consume);
        else {
            if(!batch) return -4;
            do {
                status=ops->consume(s,sink,batch,out);
            } while(!status && batch->position<batch->count);
        }
        if(s->profiling) {
            s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick;
            s->sink_rows+=s->hash_sink?s->metrics.build_rows-built:!direct?batch->position-before:0;
        }
        return status?status:graph_consume_input();
    }
    if(device || s->nodes[scan].position==leaf->bytes) {
        int64_t status=ops->finish?ops->finish(s,sink):0;
        if(status) return status;
        out->finished=s->control->finished=1;
    }
    if(device) return csGraphStep(FEMU_GRAPH_STEP_DONE,QP_WORKER_FINISHED);
    s->stream_scanning=false;
    return csGraphStep(FEMU_GRAPH_STEP_DONE,QP_INPUT_DRAINED|(out->finished?QP_WORKER_FINISHED:0));
}
enum graph_batch_status graph_batch_read(const struct femu_csd_args *a,struct graph_batch *batch)
{
    if(!batch || !graph_operator_args(a) || !a->graph_stream) return GRAPH_BATCH_ERROR;
    if(a->graph_input==FEMU_GRAPH_INPUT_EOF) return GRAPH_BATCH_EOF;
    if(a->graph_input!=FEMU_GRAPH_INPUT_BATCH) return GRAPH_BATCH_ERROR;
    const struct qp_plan *plan=a->mr_addr[0];
    unsigned worker=((uint64_t)a->cparam1>>1)&127;
    if(!worker || worker>plan->workers) return GRAPH_BATCH_ERROR;
    const struct qp_control *control=(const struct qp_control *)a->mr_addr[1]+worker;
    const struct execution *s=(const void *)(uintptr_t)control->state;
    if(!s || !s->stream_block || s->plan!=plan) return GRAPH_BATCH_ERROR;
    struct qs_chunk *chunk=s->published_chunk;
    if(!chunk) return GRAPH_BATCH_ERROR;
    unsigned column=(uint64_t)a->cparam1>>QP_GRAPH_BLOCK_NODE_SHIFT;
    if(!column || column>chunk->ncols) return GRAPH_BATCH_ERROR;
    *batch=(struct graph_batch){.chunk=chunk,.column=column-1};
    return GRAPH_BATCH_READY;
}

unsigned graph_batch_count(const struct graph_batch *batch)
{ return batch->chunk->selected; }

bool graph_batch_i64(const struct graph_batch *batch,unsigned row,int64_t *value)
{
    struct qs_chunk *chunk=batch->chunk;
    if(row<batch->next_row || row>=chunk->selected) return false;
    unsigned selected=qs_selection(chunk)[row];
    unsigned stride=(chunk->capacity+7)/8;
    if(qs_nulls(chunk)[batch->column*stride+selected/8]&(1U<<(selected%8))) return false;
    *value=qs_values(chunk)[(size_t)batch->column*chunk->capacity+selected];
    return true;
}

int graph_batch_keep(struct graph_batch *batch,unsigned row)
{
    if(row<batch->next_row || row>=batch->chunk->selected) return -1;
    uint32_t *selection=qs_selection(batch->chunk);
    selection[batch->retained++]=selection[row];
    batch->next_row=row+1;
    return 0;
}

int64_t graph_batch_borrow(struct graph_batch *batch)
{
    if(!batch->retained) return -1;
    batch->chunk->selected=batch->retained;
    return graph_borrow_input();
}

int64_t graph_publish_output(void)
{ return csGraphStep(FEMU_GRAPH_STEP_OUTPUT,0); }

int64_t graph_borrow_input(void)
{ return csGraphBorrowedOutput(); }

int64_t graph_consume_input(void)
{ return csGraphStep(FEMU_GRAPH_STEP_CONSUMED,0); }

int64_t graph_finish_stream(void)
{ return csGraphStep(FEMU_GRAPH_STEP_DONE,0); }

static int64_t stream_block_execute(struct execution *s,struct femu_csd_args *a,
    unsigned scan,unsigned sink,struct qs_result *out,enum qp_graph_block_step step,graph_stream_operator execute)
{
    int64_t status=0;
    if(s->text_wait) {
        status=qs_dictionary_text_resume(s->text,s->io,s->text_wait>QS_DICTIONARY_BUSY?csReentryValue(a):0);
        s->text_wait=status;
        if(!status) s->expression_wait=0;
    }
    if(!status) status=execute?execute(s,scan,sink,out):-1;
    if(s->late_wait) status=s->late_wait;
    if(s->text_wait) status=s->text_wait;
    if(s->dictionary_wait) status=s->dictionary_wait;
    if(s->expression_wait && s->pending_spool && status<0) {
        s->expression_wait=0;
        status=csNeedLoad(s->pending_spool->pages[s->pending_block].token);
    }
    wait_io(s,status);
    if(s->text_wait==QS_DICTIONARY_BUSY || s->dictionary_wait==QS_DICTIONARY_BUSY)
        return csGraphWait();
    if(status>0 && !((uint64_t)status>>60)) status=-4;
    if(status<0 && !s->control->error) {
        s->control->error=status;
        s->control->error_node=step<=BLOCK_FILTER?scan:sink;
    }
    return status;
}

static int64_t graph_block_execute(struct femu_csd_args *a,enum qp_graph_block_step step,graph_stream_operator execute)
{
    if(!a || a->numr!=5 || a->mr_len[0]<(long long)sizeof(struct qp_plan) || a->mr_len[4]<QS_SORT_IO_BYTES)
        return -1;
    unsigned worker=((uint64_t)a->cparam1>>1)&127;
    struct qp_plan *p=a->mr_addr[0];
    if(!worker || worker>p->workers || a->mr_len[1]<(long long)((worker+1)*sizeof(struct qp_control)) ||
       a->mr_len[2]<(long long)((worker+1)*sizeof(struct qs_result))) return -1;
    struct qp_control *control=(struct qp_control *)a->mr_addr[1]+worker;
    struct execution *s=(void *)(uintptr_t)control->state;
    if(!s || !s->block_graph || s->plan!=p || (!s->group_sink && !s->hash_sink && !s->aggregate_sink && !s->sort_sink && !s->partition_sink &&
       !s->set_sink && p->packed_result!=QP_DIRECT_RESULT)) return -1;
#ifdef QP_SPLIT_RUNTIME
    if(!s->stream_block) return -1;
    unsigned scan=s->wave_source;
#else
    unsigned scan=s->stream_block?qp_pipeline_source(p,s->root):qp_probe_scan(p,s->root);
#endif
    unsigned sink=s->group_sink?s->group_sink-1:s->aggregate_sink?s->aggregate_sink-1:s->root;
    if(scan>=p->count) return -1;
    struct qs_result *out=(struct qs_result *)a->mr_addr[2]+worker;
    s->args=a; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
    wait_resume(s);
    int64_t status=resume_page_io(s,a); if(status) return status;
    status=resume_spool(s,a); if(status) return status;
#ifndef QP_SPLIT_RUNTIME
    if(!s->stream_block && s->set_sink) return -1;
    if(s->stream_block) {
#else
    {
#endif
        uint64_t wall=s->profiling?now_ns(CLOCK_MONOTONIC):0;
        uint64_t cpu=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
        if(s->profiling) {
            struct execution *owner=s->owner;
            unsigned active=atomic_fetch_add(&owner->active_cus,1)+1,peak=atomic_load(&owner->peak_cus);
            while(active>peak && !atomic_compare_exchange_weak(&owner->peak_cus,&peak,active)) { }
        }
        status=stream_block_execute(s,a,scan,sink,out,step,execute);
        if(s->profiling) {
            struct qs_timing *timing=&s->profile.operators[QS_JOIN_PROBE];
            ++timing->calls; timing->cpu_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
            timing->wall_ns+=now_ns(CLOCK_MONOTONIC)-wall;
            atomic_fetch_sub(&s->owner->active_cus,1);
        }
        if(((uint64_t)status>>60)==FEMU_GRAPH_STEP_ACTION && (status&FEMU_GRAPH_STEP_DONE) &&
           (((uint64_t)status>>3)&(QP_INPUT_DRAINED|QP_WORKER_FINISHED))) plan_output_metrics(s,out,worker);
        return status;
    }
#ifndef QP_SPLIT_RUNTIME
    const struct qp_node *leaf=&p->nodes[scan];
    struct operator_state *o=&s->nodes[scan];
    if(s->text_wait) {
        status=qs_dictionary_text_resume(s->text,s->io,s->text_wait>QS_DICTIONARY_BUSY?csReentryValue(a):0);
        s->text_wait=status; if(status) return status==QS_DICTIONARY_BUSY?-4:status;
        s->expression_wait=0;
    }
    uint64_t wall=s->profiling?now_ns(CLOCK_MONOTONIC):0,cpu=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
    if(s->profiling) {
        struct execution *owner=s->owner;
        unsigned active=atomic_fetch_add(&owner->active_cus,1)+1,peak=atomic_load(&owner->peak_cus);
        while(active>peak && !atomic_compare_exchange_weak(&owner->peak_cus,&peak,active)) { }
    }
    switch(step) {
        case BLOCK_SCAN: {
            struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,leaf));
            uint64_t page=(uint64_t)a->cparam1>>QP_SCAN_PAGE_SHIFT;
            if(page>UINT64_MAX/QS_PAGE || page*QS_PAGE>leaf->bytes) { status=-1; break; }
            uint64_t offset=page*QS_PAGE;
            unsigned bytes=leaf->bytes-offset<p->scan_bytes?leaf->bytes-offset:p->scan_bytes;
            if(!csIsReentry(a)) {
                if((s->host_scan && !s->scan_drained) || s->block_rows.rows) { status=-1; break; }
                s->host_scan=true; s->scan_drained=false; s->block_read=0;
                s->block_sink_started=s->block_sink_done=false;
                o->scan_position=o->scan_input=o->scan_bytes=o->scan_raw_bytes=0;
                o->position=offset+bytes;
                chunk->rows=chunk->selected=0;
                out->rows=out->begin=out->finished=out->str_bytes=0;
                unsigned first=s->root;
                if(s->block_chain) {
                    s->block_write=0;
                    for(int id=s->root;id!=(int)scan;id=p->nodes[id].left) {
                        first=id;
                        if(p->nodes[id].kind==QP_INDEX_JOIN)
                            s->nodes[id].input_done=s->nodes[id].finished=0;
                    }
                } else if(p->nodes[s->root].kind==QP_INDEX_JOIN)
                    s->nodes[s->root].input_done=s->nodes[s->root].finished=0;
                if(p->nodes[first].kind==QP_JOIN) {
                    status=hash_probe_projection(s,first,0); if(status) break;
                }
            }
            if(bytes) status=scan_decode(s,scan,bytes,false);
            break;
        }
        case BLOCK_FILTER:
            if(o->scan_raw_bytes) {
                status=scan_filter_chunk(s,scan);
                if(status==-4) {
                    cs_work_free(s->context,o->scan_filter); o->scan_filter=NULL;
                    status=scan_decode(s,scan,o->scan_raw_bytes,true);
                }
            }
            break;
        case BLOCK_PROBE:
        case BLOCK_INDEX:
        case BLOCK_PROJECT:
            if(s->block_chain) {
                unsigned id=(uint64_t)a->cparam1>>QP_GRAPH_BLOCK_NODE_SHIFT;
                if(!id || id>p->count) { status=-1; break; }
                status=block_transform(s,id-1,scan);
            } else status=block_probe(s,sink);
            break;
        case BLOCK_GROUP:
            if(!s->block_sink_done) {
                status=s->block_chain?block_chain_sink(s,sink,out):
                    s->root==scan?block_sink_scan(s,sink,out,block_apply):block_sink(s,sink,out);
                s->block_sink_done=status==0;
            }
            /* Reentry during the final radix flush resumes the flush, even
             * after the scan consumed and freed the final projection. */
            if(!status && o->position==leaf->bytes) {
                struct operator_state *local=s->nodes[sink].group_local;
                if(local && local->group_radix) status=radix_flush(s,sink);
                if(!status) out->finished=control->finished=1;
            }
            break;
        case BLOCK_HASH: {
            uint64_t before=s->metrics.build_rows;
            status=s->block_chain?block_chain_sink(s,sink,out):
                s->root==scan?block_sink_scan(s,sink,out,block_apply):block_sink(s,sink,out);
            if(s->root==scan && s->profiling) s->sink_rows+=s->metrics.build_rows-before;
            if(!status && o->position==leaf->bytes) out->finished=control->finished=1;
            break;
        }
        case BLOCK_PARTITION:
            status=partition_sink_prefix(s);
            if(!status) status=s->block_chain?block_chain_sink(s,sink,out):block_sink_scan(s,sink,out,block_apply);
            if(!status && o->position==leaf->bytes) out->finished=control->finished=1;
            break;
        case BLOCK_SORT:
            if(!s->sort_sink->finishing_sink)
                status=s->block_chain?block_chain_sink(s,sink,out):block_sink_scan(s,sink,out,block_apply);
            if(!status && o->position==leaf->bytes) {
                s->sort_sink->finishing_sink=1;
                status=sort_call(s,s->sort_sink,0);
                if(!status) out->finished=control->finished=1;
            }
            break;
        default: status=-1; break;
    }
    if(s->profiling) {
        struct qs_timing *t=&s->profile.operators[QS_JOIN_PROBE];
        atomic_fetch_sub(&s->owner->active_cus,1);
        ++t->calls; t->cpu_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
        t->wall_ns+=now_ns(CLOCK_MONOTONIC)-wall;
    }
    if(s->late_wait) status=s->late_wait;
    if(s->text_wait) status=s->text_wait;
    if(s->dictionary_wait) status=s->dictionary_wait;
    wait_io(s,status);
    if(status) {
        if(status>0 && !((uint64_t)status>>60)) status=-4;
        if(status<0 && !control->error) {
            control->error=status;
            control->error_node=step<=BLOCK_FILTER?scan:sink;
        }
        return status;
    }
    if(step!=BLOCK_GROUP && step!=BLOCK_HASH && step!=BLOCK_SORT && step!=BLOCK_PARTITION) return 0;
    plan_output_metrics(s,out,worker);
    return QP_INPUT_DRAINED | (out->finished?QP_WORKER_FINISHED:0);
#endif
}

int64_t graph_block_call(struct femu_csd_args *a,enum qp_graph_block_step step,graph_stream_operator execute)
{
    qs_dictionary_graph_context(a);
    int64_t status=graph_block_execute(a,step,execute);
    qs_dictionary_graph_context(NULL);
    return status;
}

bool graph_operator_args(const struct femu_csd_args *a)
{
    if(!a || a->numr!=5 || a->mr_len[0]<(long long)sizeof(struct qp_plan) ||
       a->mr_len[3]<(long long)sizeof(struct qs_dict) || a->mr_len[4]<QS_SORT_IO_BYTES)
        return false;
    const struct qp_plan *p=a->mr_addr[0];
    return p->count && p->count<=QP_NODES && p->root<p->count && p->workers<=QS_WORKERS &&
        p->nparams<=QP_PARAMS && (uint64_t)a->mr_len[0]>=sizeof(*p)+qp_host_parameter_bytes(p) &&
        a->mr_len[1]>=(long long)((p->workers+1)*sizeof(struct qp_control)) &&
        a->mr_len[2]>=(long long)((p->workers+1)*sizeof(struct qs_result));
}

int graph_wave_node(const struct femu_csd_args *a)
{
    if(!graph_operator_args(a)) return -1;
    const struct qp_plan *p=a->mr_addr[0];
    unsigned id=((uint64_t)a->cparam1>>QP_GRAPH_PIPELINE_NODE_SHIFT)&QP_GRAPH_PIPELINE_NODE_MASK;
    return id<p->count?(int)id:-1;
}

/* Restore the common execution context before an operator advances a wave. */
int64_t graph_wave_begin(struct femu_csd_args *a,const struct graph_projection_ops *ops,
    graph_query_initializer initialize,struct execution **execution)
{
    struct qp_control *control=a->mr_addr[1];
    struct execution *s=(void *)(uintptr_t)control->state;
    if(!s) {
        int status=open_execution(a,a->mr_addr[0],control,ops,initialize,&s);
        if(status) return status;
    }
    s->args=a; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
    s->block_graph=true;
    if(!csIsReentry(a)) control->drained=0;
    wait_resume(s);
    int64_t status=initialize_dictionary(s,a);
    if(status) return status;
    status=resume_spool(s,a);
    if(status) return status;
    *execution=s;
    return 0;
}

int64_t pg_sina_graph_query_close(struct femu_csd_args *a)
{
    if(!graph_operator_args(a)) return -1;
    struct qp_control *control=a->mr_addr[1];
    struct execution *s=(void *)(uintptr_t)control->state;
    if(s && control->close) close_execution(s);
    return 0;
}


static int64_t graph_tail_step(struct femu_csd_args *a,graph_tail_operator execute)
{
    struct qp_plan *p=a->mr_addr[0];
    unsigned worker=((uint64_t)a->cparam1>>1)&127;
    if(worker>p->workers) return -1;
    struct qp_control *control=(struct qp_control *)a->mr_addr[1]+worker;
    struct execution *s=(void *)(uintptr_t)control->state;
    if(!s || s->plan!=p || p->packed_result!=QP_TYPED_RESULT) return -1;
    uint64_t id=(uint64_t)a->cparam1>>QP_GRAPH_PIPELINE_NODE_SHIFT;
    if(id>=p->count) return -1;
    s->args=a; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
    wait_resume(s);
    if(s->text_wait) {
        int64_t pending=qs_dictionary_text_resume(s->text,s->io,
            s->text_wait>QS_DICTIONARY_BUSY?csReentryValue(a):0);
        s->text_wait=pending;
        if(pending) return pending==QS_DICTIONARY_BUSY?csGraphWait():pending;
        s->expression_wait=0;
    }
    int64_t status=resume_page_io(s,a); if(status) return status;
    status=resume_spool(s,a); if(status) return status;
    return execute(s,a,worker,id);
}

int64_t graph_tail_call(struct femu_csd_args *a, graph_tail_operator execute,bool report_worker)
{
    if(!graph_operator_args(a)) return -1;
    const struct qp_plan *p=a->mr_addr[0];
    unsigned worker=((uint64_t)a->cparam1>>1)&127;
    if(worker>p->workers) return -1;
    const struct qp_control *control=(const struct qp_control *)a->mr_addr[1]+worker;
    struct execution *s=worker?(void *)(uintptr_t)control->state:NULL;
    uint64_t wall=0,cpu=0;
    if(s && s->profiling) {
        wall=now_ns(CLOCK_MONOTONIC); cpu=now_ns(CLOCK_THREAD_CPUTIME_ID);
        struct execution *owner=s->owner;
        unsigned active=atomic_fetch_add(&owner->active_cus,1)+1,peak=atomic_load(&owner->peak_cus);
        while(active>peak && !atomic_compare_exchange_weak(&owner->peak_cus,&peak,active)) { }
    }
    qs_dictionary_graph_context(a);
    int64_t status=graph_tail_step(a,execute);
    qs_dictionary_graph_context(NULL);
    if(s && s->profiling) {
        struct qs_timing *timing=&s->profile.operators[QS_JOIN_PROBE];
        ++timing->calls; timing->cpu_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
        timing->wall_ns+=now_ns(CLOCK_MONOTONIC)-wall;
        atomic_fetch_sub(&s->owner->active_cus,1);
    }
    if(s && report_worker && ((uint64_t)status>>60)==FEMU_GRAPH_STEP_ACTION &&
       (status&FEMU_GRAPH_STEP_DONE))
        plan_output_metrics(s,(struct qs_result *)a->mr_addr[2]+worker,worker);
    return status;
}


#ifndef QP_SPLIT_RUNTIME
enum pipeline_phase { PIPELINE_EXECUTE, PIPELINE_BUILD, PIPELINE_GROUP };
static int64_t plan_execute(struct femu_csd_args *a,enum pipeline_phase phase)
{
    if(!a || a->numr!=5 || a->mr_len[0]<(long long)sizeof(struct qp_plan) ||
        a->mr_len[1]<(long long)sizeof(struct qp_control) || a->mr_len[2]<(long long)sizeof(struct qs_result) ||
        a->mr_len[4]<QS_SORT_IO_BYTES) return -1;
    struct qp_plan *plan=a->mr_addr[0]; unsigned worker=((uint64_t)a->cparam1>>1)&127;
    if(plan->nparams>QP_PARAMS || (uint64_t)a->mr_len[0]<sizeof(*plan)+qp_host_parameter_bytes(plan)) return -1;
    if(plan->workers>QS_WORKERS || worker>plan->workers || a->mr_len[1]<(long long)((worker+1)*sizeof(struct qp_control)) ||
       a->mr_len[2]<(long long)((worker+1)*sizeof(struct qs_result))) return -1;
    unsigned target=0;
    if(phase!=PIPELINE_EXECUTE) {
        uint64_t id=((uint64_t)a->cparam1&~(QP_GRAPH_BLOCK_EXEC|QP_GRAPH_RESIDENT_TEXT|QP_GRAPH_STREAM_EXEC))>>QP_GRAPH_PIPELINE_NODE_SHIFT;
        if(worker || plan->ninit || id>=plan->count || id>=QP_NODES) return -1;
        const struct qp_node *n=&plan->nodes[id];
        if(n->kind!=QP_AGGREGATE || !n->nkeys || n->left<0 ||
           (unsigned)n->left>=plan->count || (unsigned)n->left>=QP_NODES) return -1;
        if(phase==PIPELINE_BUILD &&
           (plan->nodes[n->left].kind!=QP_JOIN || !plan->nodes[n->left].hash)) return -1;
        target=id;
    }
    struct qp_control *control=(struct qp_control *)a->mr_addr[1]+worker;
    struct qs_result *out=(struct qs_result *)a->mr_addr[2]+worker;
    struct execution *s=(void *)(uintptr_t)control->state;
    if(s) wait_resume(s);
    if(control->close) { if(s) close_execution(s); return 0; }
    if(!s) {
        if(worker) return -1;
        int status=open_execution(a,plan,control,NULL,NULL,&s); if(status) return status;
        out->rows=out->begin=out->finished=out->str_bytes=0; out->status=0;
    }
    s->args=a; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
    if(!worker && ((uint64_t)a->cparam1&QP_GRAPH_BLOCK_EXEC)) s->block_graph=true;
    /* A new submission must not acknowledge an earlier worker batch again,
     * including early returns while dictionary initialization/resume is BUSY.
     * Device-internal I/O reentry still accumulates acknowledgements until
     * the host observes this submission's single completion. */
    if(!csIsReentry(a)) control->drained=0;
    if(s->buffered) { out->rows=s->buffered_rows; out->finished=s->buffered_finished; s->buffered=0; }
    if(s->closing) return finish_dictionary(s,a,out);
    int typed=plan->packed_result==QP_DIRECT_RESULT || (!worker && plan->packed_result==QP_TYPED_RESULT);
    int continuing=s->late_wait || s->text_wait || s->dictionary_wait || s->initial_wait || s->pending_sort || (typed && s->host_building);
    int64_t imported=initialize_dictionary(s,a);
    if(imported) return dictionary_request(s,out,imported);
    if(s->text_wait) {
        int64_t resumed=qs_dictionary_text_resume(s->text,s->io,s->text_wait>QS_DICTIONARY_BUSY?csReentryValue(a):0);
        s->text_wait=resumed; if(resumed) return dictionary_request(s,out,resumed);
        s->expression_wait=0;
    }
    if(!continuing && !csIsReentry(a) && ((uint64_t)a->cparam1&QP_PAGES_INPUT) && !((uint64_t)a->cparam1&QP_SCAN_INPUT))
        control->ready=1;
    if(!continuing && ((uint64_t)a->cparam1&QP_SCAN_INPUT) && !csIsReentry(a)) {
        int id=qp_probe_scan(plan,s->root);
        uint64_t page=(uint64_t)a->cparam1>>QP_SCAN_PAGE_SHIFT;
        if(!worker || id<0 || plan->nodes[id].ordered_index || page>UINT64_MAX/QS_PAGE) return -1;
        struct qp_node *n=&plan->nodes[id]; struct operator_state *o=&s->nodes[id];
        uint64_t offset=page*QS_PAGE;
        if(offset>n->bytes || (s->host_scan && !s->scan_drained)) return -1;
        s->host_scan=true; s->scan_drained=false;
        o->position=offset; o->scan_position=o->scan_input=o->scan_bytes=0;
        struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n)); chunk->rows=chunk->selected=0;
        control->node=id; control->offset=offset;
        control->bytes=n->bytes-offset<plan->scan_bytes?n->bytes-offset:plan->scan_bytes;
        control->ready=control->bytes!=0;
    }
    /* The last AFDM is private to this CU, also for SNIA NEED_LOAD/STORE. */
    s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
    int resumed=resume_page_io(s,a); if(resumed) return resumed;
    resumed=resume_spool(s,a); if(resumed) return resumed;
    if(!csIsReentry(a) && !continuing) {
        out->rows=out->begin=out->finished=out->str_bytes=0; out->status=0; s->output_exported=0;
    }
    uint64_t start=s->profiling?now_ns(CLOCK_MONOTONIC):0,cpu=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
    if(s->profiling && !worker) s->pipeline_tick=cpu;
    struct execution *owner=worker?s->owner:s;
    if(s->profiling) {
        unsigned active=atomic_fetch_add(&owner->active_cus,1)+1,peak=atomic_load(&owner->peak_cus);
        while(active>peak && !atomic_compare_exchange_weak(&owner->peak_cus,&peak,active)) { }
    }
    unsigned columns=plan->nodes[worker?(int)s->root:(int)plan->root].expr.nout;
    /* Internal buffers pack only the pipeline's projected columns. A two-column
     * pipeline fits 2048 rows in the same AFDM as 256 final sixteen-cell rows. */
    unsigned capacity=worker && !typed?QS_RESULT_ROWS*QS_OUT/(columns?columns:1):
        qp_result_capacity(&plan->nodes[plan->root].expr,plan->packed_result);
    int64_t status=0; int host_full=0,host_publish=0;
    bool phase_done=false;
    if(typed && !s->host_building)
        host_begin(s,&plan->nodes[plan->root].expr,out,capacity);
    if(s->output_active) { status=s->output_status; goto export_results; }
    while(s->initializer<plan->ninit) {
        unsigned index=s->initializer,node=plan->init_nodes[index]; int param=plan->init_params[index];
        if(param<0) {
            status=build_set(s,node); if(status) break;
            ++s->initializer; continue;
        }
        struct qs_cell value[QS_OUT];
        status=next_row(s,node,value);
        if(status==QP_WAIT || status<0 || status>2) break;
        if(!s->initializer_phase) {
            if(!status) { s->params[param]=(struct qs_cell){.isnull=1}; ++s->initializer; }
            else { s->params[param]=value[0]; s->initializer_phase=1; }
        } else {
            if(status==2) { status=-8; break; }
            s->initializer_phase=0; ++s->initializer;
        }
    }
    if(phase!=PIPELINE_EXECUTE) {
        s->block_graph=((uint64_t)a->cparam1&QP_GRAPH_BLOCK_EXEC)!=0;
        if(phase==PIPELINE_BUILD) {
            /* Preserve the native allocation order. Before the child hash
             * tables exist, the aggregate reserves only its small initial table. */
            status=plan->nodes[target].hash?
                hash_aggregate_next_impl(s,target,NULL,GROUP_ALLOCATE,0):3;
            if(status==3) status=hash_build(s,plan->nodes[target].left);
            phase_done=status==0;
        } else {
            struct blocking_timer timer={0};
            if(s->profiling) timer=blocking_begin(s);
            status=plan->nodes[target].hash?
                hash_aggregate_next_impl(s,target,NULL,GROUP_PREPARE,0):3;
            if(s->profiling) blocking_end(s,QS_AGG_SINK,timer);
            /* Prepare stops before reading the first finalized group. */
            phase_done=status==3;
            if(phase_done) status=0;
        }
        goto combined;
    }
produce:
    if(worker && s->group_combine) {
        uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
        status=group_combine_local(s);
        if(s->profiling) s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick;
        if(!status) out->finished=control->finished=1;
        goto combined;
    }
    /* STORE reentry must finish consuming the retained output before the
     * producer overwrites it with another batch. */
    if(worker && s->sort_sink && s->pending_sort==s->sort_sink && !s->sort_sink->columns) goto produced;
    /* A spilled-partition Source retains its original row cursor. Resident
     * groups can now expose finalized columns without reopening their input. */
    if(worker && s->partition_sink) {
        uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
        status=partition_sink_prefix(s);
        if(s->profiling) s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick;
        if(status) goto produced;
        unsigned id=s->root; int source=plan->nodes[id].kind==QP_AGGREGATE;
        for(;;) {
            status=projection_fill(s,id,source);
            if(status!=2) {
                if(!status) out->finished=control->finished=1;
                goto produced;
            }
            struct projection_batch *b=source?s->nodes[id].source_projection:s->nodes[id].projection;
            unsigned before=b->position;
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            status=partition_sink_vectors(s,b);
            if(s->profiling) { s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick; s->sink_rows+=b->position-before; }
            if(status) goto produced;
        }
    }
    if(worker && s->hash_sink && (s->source_node!=(int)s->root || plan->nodes[s->root].kind==QP_AGGREGATE)) {
        unsigned id=s->root; int source=plan->nodes[id].kind==QP_AGGREGATE;
        status=prepare_projection(s,id,source); if(status<0) goto produced;
        while(source?s->nodes[id].source_projection:s->nodes[id].projection) {
            status=projection_fill(s,id,source);
            if(status!=2) {
                if(!status) out->finished=control->finished=1;
                if(status!=3) goto produced;
                break;
            }
            struct projection_batch *b=source?s->nodes[id].source_projection:s->nodes[id].projection;
            unsigned before=b->position;
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            status=hash_sink_vectors(s,id);
            if(s->profiling) { s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick; s->sink_rows+=b->position-before; }
            if(status<0) goto produced;
            /* The remaining rows must reach the coordinator before another
             * input overwrites the vectors; next_row retains their cursor. */
            if(b->position<b->count) break;
        }
        status=2;
    }
    if(worker && s->group_sink) {
        unsigned id=s->group_sink-1;
        status=prepare_projection(s,id,0); if(status<0) goto produced;
        while(s->nodes[id].projection) {
            status=projection_fill(s,id,0);
            if(status!=2) {
                if(!status) out->finished=control->finished=1;
                if(status!=3) goto produced;
                break;
            }
            struct projection_batch *b=s->nodes[id].projection;
            unsigned before=b->position,rows=out->rows;
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            status=aggregate_accept_vectors(s,id,out,capacity);
            if(s->profiling) {
                s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick;
                s->sink_rows+=b->position-before-(out->rows-rows);
            }
            if(status) goto produced;
            status=2; if(out->rows) goto produced;
        }
        status=2;
    }
    if(worker && s->aggregate_sink) {
        unsigned id=s->aggregate_sink-1;
        status=prepare_projection(s,id,0); if(status<0) goto produced;
        while(s->nodes[id].projection) {
            status=projection_fill(s,id,0);
            if(status!=2) {
                if(!status) out->finished=control->finished=1;
                if(status!=3) goto produced;
                break;
            }
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            unsigned rows=s->nodes[id].projection->count;
            status=aggregate_accept_vectors(s,id,NULL,0);
            if(s->profiling) { s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick; s->sink_rows+=rows; }
            if(status<0) goto produced;
        }
        status=2;
    }
    if(worker && s->sort_sink) {
        status=sort_sink_columns(s,s->sort_sink,s->root);
        if(status!=3) {
            if(!status) out->finished=control->finished=1;
            goto produced;
        }
        status=2;
    }
    while(status>=0 && status!=QP_WAIT && status<=2 && out->rows<capacity) {
        struct qs_cell *row=typed?s->host_row:worker?(struct qs_cell *)out->data+(size_t)out->rows*columns:
            qp_result_row(out,out->rows,columns,plan->packed_result);
        if(typed && !worker && !s->host_pending && plan->workers>1 && plan->nodes[plan->root].kind==QP_SCAN && plan->nodes[plan->root].parallel) {
            /* Workers already evaluated this Scan's projection. Borrow a
             * whole canonical batch, rather than running the generic row
             * dispatcher and searching the ready CU mask for every tuple.
             * Acknowledge it only on the next borrow, after all row payloads
             * (including deferred Numeric bodies) have been encoded. */
            if(!s->host_source_rows) {
                status=parallel_next(s,plan->root,NULL);
                if(status!=2) {
                    if(!status) out->finished=control->finished=1;
                    goto produced;
                }
                for(unsigned w=0;w<plan->workers;++w) if((s->control->available&(UINT64_C(1)<<w)) &&
                    s->parallel_position[w]<s->outputs[w+1].rows) {
                    s->host_source_worker=w;
                    s->host_source_rows=s->outputs[w+1].rows-s->parallel_position[w];
                    s->host_source=(const struct qs_cell *)s->outputs[w+1].data+(size_t)s->parallel_position[w]*columns;
                    break;
                }
            }
            memcpy(row,s->host_source,columns*sizeof(*row));
            s->host_source+=columns; --s->host_source_rows; ++s->parallel_position[s->host_source_worker];
            status=2;
        } else status=(typed?s->host_pending!=0:s->output_pending!=0)?2:next_row(s,worker?(int)s->root:(int)plan->root,row);
        if(status==QP_WAIT || status<0 || status>2) break;
        if(!status) { out->finished=control->finished=1; break; }
        const struct qs_plan *root=&plan->nodes[plan->root].expr;
        if(typed) {
            s->host_pending=1;
            int appended=host_append(s,root,out);
            if(appended<0) { status=appended; break; }
            if(appended) { host_full=1; break; }
            continue;
        }
        if(!worker) export_sort_text(s,root,row,out);
        if(!worker && !s->output_order && export_row(s,root,row,out)) { status=-3; break; }
        ++out->rows;
    }
produced:
    if(worker && s->sort_sink && !s->sort_sink->columns && status>=0 && status<=2 &&
       (out->rows || out->finished || s->pending_sort==s->sort_sink)) {
        struct sorted_rows *local=s->sort_sink;
        if(s->pending_sort!=local) local->producer_status=status;
        local->batch=out; out->begin=0;
        unsigned rows=out->rows;
        uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
        status=sort_call(s,local,0);
        if(!status) {
            if(out->finished && !local->sink_finished) {
                local->finishing_sink=1;
                status=sort_call(s,local,0);
            }
        }
        if(!status) {
            local->batch=NULL; out->rows=0;
            status=out->finished?0:local->producer_status;
            if(s->profiling) s->sink_rows+=rows;
        }
        if(s->profiling) s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick;
        if(s->host_scan && !out->rows && status==2) goto produce;
    }
    if(worker && (s->hash_sink || s->group_sink || s->aggregate_sink) && status>=0 && status<=2) {
        uint64_t cpu=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
        unsigned rows=out->rows;
        if(s->hash_sink) { int added=hash_sink_batch(s,out); if(added<0) status=added; }
        if(s->group_sink || s->aggregate_sink) {
            int64_t added=aggregate_accept_result(s,out);
            if(s->group_sink?added!=0:added<0) status=added;
        }
        if(s->profiling) { s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu; s->sink_rows+=rows-out->rows; }
        /* A direct sink can consume another output batch from the same input
         * without asking PG to submit another compute request. */
        if(s->host_scan && !out->rows && status==2) goto produce;
    }
    if(worker && s->group_sink && out->finished && status>=0 && status<=2) {
        unsigned id=s->group_sink-1;
        struct operator_state *local=s->nodes[id].group_local;
        if(local && local->group_radix) {
            int64_t flush=radix_flush(s,id); if(flush) status=flush;
        }
    }
export_results:
    if(!typed && !worker && s->output_order && out->rows && status>=0 && status<=2 &&
       !s->text_wait && !s->dictionary_wait) {
        if(!s->output_active) s->output_status=status;
        if(export_batch(s,&plan->nodes[plan->root].expr,out)) status=-3;
        else status=s->output_status;
    }
combined:
    if(typed) {
        host_publish=!s->late_wait && !s->text_wait && !s->dictionary_wait && status>=0 &&
            (out->finished || host_full || out->rows==capacity || (worker && s->host_scan && s->scan_drained));
        if(host_publish) { qp_host_compact(out,&plan->nodes[plan->root].expr,capacity); s->host_building=0; }
        else s->host_building=out->rows!=0;
    }
    if(s->late_wait) status=s->late_wait;
    if(s->text_wait) status=s->text_wait;
    if(s->dictionary_wait) status=s->dictionary_wait;
    if(s->expression_wait && s->pending_spool) {
        s->expression_wait=0;
        status=csNeedLoad(s->pending_spool->pages[s->pending_block].token);
    }
    if(status==QP_WAIT) {
        int64_t request=namespace_wait(s); if(request) status=request;
    }
    if(s->profiling) {
        struct qs_timing *t=&s->profile.operators[QS_JOIN_PROBE];
        if(!worker && s->parallel_started)
            plan->pipelines[s->group_combine?s->group_combine-1:s->parallel_node].coordinator_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-s->pipeline_tick;
        ++t->calls; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-start; t->cpu_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
        atomic_fetch_sub(&owner->active_cus,1); control->peak_cus=atomic_load(&owner->peak_cus);
    }
    control->output_rows=(s->late_wait || s->text_wait || s->dictionary_wait || (typed && !host_publish))?0:out->rows;
    control->output_strings=out->str_bytes;
    plan_output_metrics(s,out,worker);
    if(status<0) {
        struct cs_work_stats pool={0};
        struct cs_work_context_stats memory={0};
        cs_work_get_stats(&pool);
        cs_work_get_context_stats(s->context,&memory);
        const struct operator_state *failed=&s->nodes[control->error_node<plan->count?control->error_node:plan->root];
        fprintf(stderr,"CSD plan error=%lld worker=%u node=%u root=%u phase=%u groups=%u capacity=%u radix=%u context_used=%zu context_limit=%zu pool_used=%zu pool_free=%zu largest_free=%zu\n",
            (long long)status,worker,control->error_node,s->root,failed->group_phase,
            failed->group_count?atomic_load(failed->group_count):0,failed->group_capacity,
            failed->group_radix!=NULL,memory.used_bytes,memory.limit_bytes,
            pool.used_bytes,pool.free_bytes,pool.largest_free_payload);
        return status;
    }
    if(s->text_wait==QS_DICTIONARY_BUSY || s->dictionary_wait==QS_DICTIONARY_BUSY ||
       (s->pending_sort && status==QS_DICTIONARY_BUSY)) return dictionary_request(s,out,QS_DICTIONARY_BUSY);
    if(worker && status<=2) {
        if(s->profiling && (s->hash_sink || s->group_sink))
            s->sink_returned_rows+=control->output_rows;
        uint64_t metadata=typed?QP_TYPED_OUTPUT|((uint64_t)control->output_rows<<5)|((uint64_t)control->output_strings<<22):0;
        if(typed && host_publish) qp_host_pack(out,&plan->nodes[plan->root].expr,control->output_strings);
        if(s->profiling && out->finished && !s->finished_ns) s->finished_ns=now_ns(CLOCK_MONOTONIC);
        if(s->host_scan && s->scan_drained && !out->finished) wait_begin(s,QP_WAIT_INPUT);
        else if(!out->finished && control->output_rows) wait_begin(s,QP_WAIT_SINK);
        return metadata | (control->output_rows?QP_OUTPUT_READY:0) | (s->host_scan && s->scan_drained?QP_INPUT_DRAINED:0) |
            (out->finished?QP_WORKER_FINISHED:0) |
            (!control->ready && (control->bytes || control->index_nreads)?QP_CONTROL_REQUIRED:0);
    }
    if(out->finished && !worker && status<=2 && !s->late_wait) { s->closing=1; return finish_dictionary(s,a,out); }
    wait_io(s,status);
    if(phase_done && !status) return QP_GRAPH_PHASE_DONE;
    return status==QP_WAIT?0:status>2?status:0;
}

static int64_t plan_call(struct femu_csd_args *a,enum pipeline_phase phase)
{
    qs_dictionary_graph_context(a);
    int64_t status=plan_execute(a,phase);
    qs_dictionary_graph_context(NULL);
    return status;
}

int64_t pg_sina_plan(struct femu_csd_args *a)
{ return plan_call(a,PIPELINE_EXECUTE); }

/* Retain the legacy staged interface only in the native reference module,
 * whose external-hash regression also exercises that interface. */
#ifdef pg_sina_plan_EXPORTS
int64_t pg_sina_graph_build_pipeline(struct femu_csd_args *a)
{ return plan_call(a,PIPELINE_BUILD); }
int64_t pg_sina_graph_group_pipeline(struct femu_csd_args *a)
{ return plan_call(a,PIPELINE_GROUP); }
#endif
#endif
