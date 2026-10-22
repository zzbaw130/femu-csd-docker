/* Execute a PG physical tree. PG submits page LOADs and SNIA handles spill;
 * tuple bytes and intermediate rows never cross back to the PG executor. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "plan.h"
#include "query_eval.h"
#include "index_scan.h"
#include <limits.h>
#include <stdbool.h>
#include <time.h>

extern int64_t pg_sina_scan_impl(struct femu_csd_args *);
extern int64_t pg_sina_sort_sink_impl(struct femu_csd_args *);
extern int64_t pg_sina_sort_source_impl(struct femu_csd_args *);
extern void qs_sort_dictionary(uint64_t,struct qs_dict *);
#define QP_WAIT 1
struct sorted_rows {
    struct qs_plan plan;
    struct qs_task task;
    struct qs_group group;
    struct qs_result *input,*output;
    unsigned position;
    int initialized, complete, drained, input_pending;
    struct qs_cell *prefix;
    unsigned prefix_rows,prefix_position;
    struct qs_metrics metrics;
};
struct spool {
    unsigned char *tail,*cache;
    uint64_t *tokens;
    unsigned blocks, capacity, used, loaded;
    uint64_t rows;
};
struct accumulator { struct qs_cell sum,extreme,last; uint64_t count; int has_last; };
struct set_key { int64_t value; unsigned occupied; };
struct operator_state {
    struct sorted_rows left_sort,right_sort;
    struct qs_cell left[QS_OUT],right[QS_OUT],first[QS_OUT],keys[QS_KEYS];
    struct accumulator accum[QS_AGGS];
    struct spool group;
    uint64_t position, skipped, emitted;
    unsigned scan_position;
    int phase, have_left, have_right, input_done, group_ready, matched, finished;
    struct qs_plan *index_plan;
    struct qs_join_control index_control;
    struct qs_task index_task;
    struct qs_result *index_output;
    struct qs_cell *index_left;
    uint64_t index_matches;
    unsigned index_count, index_unmatched;
    unsigned index_position;
    int index_resume, index_flush;
    struct qs_cell *hash_data;
    unsigned hash_rows,hash_capacity,hash_match,hash_phase;
    int hash_matched,hash_fallback;
    struct set_key *set;
    unsigned set_capacity,set_count;
    int set_null,set_ready;
};
struct execution {
    cs_work_context *context;
    struct qp_plan *plan;
    struct qp_control *control;
    struct qs_dict *dictionary;
    struct operator_state *nodes;
    struct qs_cell params[QP_PARAMS];
    unsigned char *arena;
    void *io;
    size_t io_bytes, sort_limit;
    struct sorted_rows *pending_sort;
    struct spool *pending_spool;
    unsigned pending_block;
    int pending_store;
    struct qs_metrics metrics;
    struct qs_profile profile;
    bool profiling;
    struct femu_csd_args *args;
    unsigned initializer,initializer_phase;
};
static uint64_t now_ns(clockid_t clock)
{ struct timespec t; clock_gettime(clock,&t); return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }
static int64_t next_row_impl(struct execution *,unsigned,struct qs_cell *);
static inline int64_t next_row(struct execution *s,unsigned id,struct qs_cell *row)
{
    int64_t status=next_row_impl(s,id,row);
    if(status<0 && !s->control->error) { s->control->error=status; s->control->error_node=id; }
    return status;
}
static int64_t join_next(struct execution *,unsigned,struct qs_cell *);
static int key_compare(struct execution *,const struct qp_node *,const struct qs_cell *,const struct qs_cell *);
static int keys_null(const struct qp_node *,const struct qs_cell *);
static uint64_t mix_hash(uint64_t);
static int64_t build_set(struct execution *,unsigned);
static _Thread_local struct execution *index_execution;
static _Thread_local unsigned index_node;
#define QP_INDEX_YIELD 4
static void init_eval(struct qs_eval *e,const struct qs_plan *plan,const struct qs_cell *row,const struct qs_cell *aggregates)
{
    e->plan=plan; e->tuple=row; e->aggregates=aggregates;
    memset(e->valid,0,plan->nexprs);
}
static int compare_cell(struct execution *s,struct qs_cell a,struct qs_cell b,unsigned type)
{
    if(a.isnull || b.isnull) return (a.isnull!=0)-(b.isnull!=0);
    if(type==Q_STRING) {
        const char *x,*y; unsigned xn,yn;
        if(qs_dict_get(s->dictionary,(unsigned)a.value,&x,&xn) || qs_dict_get(s->dictionary,(unsigned)b.value,&y,&yn)) return INT_MIN;
        int cmp=memcmp(x,y,xn<yn?xn:yn); return cmp?cmp:(xn>yn)-(xn<yn);
    }
    int cmp=0; return qs_compare(a,b,&cmp)?INT_MIN:cmp;
}
static unsigned character_bytes(const char *text,unsigned available)
{
    if(!available) return 0;
    unsigned char c=*text; unsigned n=c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:4;
    return n<=available?n:available;
}
static int glob(const char *p,unsigned pn,const char *v,unsigned vn)
{
    if(!pn) return !vn;
    if(*p=='%') {
        while(pn && *p=='%') { ++p; --pn; }
        if(!pn) return 1;
        for(unsigned i=0;;) {
            if(glob(p,pn,v+i,vn-i)) return 1;
            if(i==vn) break;
            i+=character_bytes(v+i,vn-i);
        }
        return 0;
    }
    if(*p=='\\' && pn>1) { ++p; --pn; return vn && *p==*v && glob(p+1,pn-1,v+1,vn-1); }
    if(*p=='_') { unsigned n=character_bytes(v,vn); return n && glob(p+1,pn-1,v+n,vn-n); }
    return vn && *p==*v && glob(p+1,pn-1,v+1,vn-1);
}
static int evaluate(struct execution *s,struct qs_eval *e,int index,struct qs_cell *out)
{
    if(index<0 || (unsigned)index>=e->plan->nexprs) return -1;
    if(e->valid[index]) { *out=e->cells[index]; return 0; }
    const struct qs_expr *x=&e->plan->exprs[index]; struct qs_cell a={0},b={0},v={0};
    if(x->op==QP_PARAMETER) v=s->params[x->column];
    else if(x->op==QP_INSET) {
        if(evaluate(s,e,x->left,&a)) return -5;
        struct operator_state *set=&s->nodes[x->column];
        if(!set->set_ready) return -1;
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
    }
    else if(x->op==Q_CONST && x->type==Q_STRING && !x->isnull) {
        const struct qs_string *literal=&e->plan->strings[x->constant];
        int id=qs_dict_insert(s->dictionary,literal->bytes,literal->length); if(id<0) return -3;
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
                    if(qs_dict_get(s->dictionary,(unsigned)a.value,&left,&ln)) return -3;
                    if(x->op==QP_SUBSTRING) {
                        struct qs_cell length; if(evaluate(s,e,x->otherwise,&length)) return -5;
                        if(length.isnull) v.isnull=1;
                        else {
                            if(length.value<0 || b.value<INT_MIN || b.value>INT_MAX || length.value>INT_MAX) return -5;
                            int64_t begin=b.value-1,end=begin+length.value;
                            if(begin<0) begin=0;
                            if(end<begin) end=begin;
                            unsigned first=0,last=0; int64_t character=0;
                            while(first<ln && character<begin) { first+=character_bytes(left+first,ln-first); ++character; }
                            last=first;
                            while(last<ln && character<end) { last+=character_bytes(left+last,ln-last); ++character; }
                            int id=qs_dict_insert(s->dictionary,left+first,last-first); if(id<0) return -3;
                            v.value=id;
                        }
                    } else {
                        if(qs_dict_get(s->dictionary,(unsigned)b.value,&right,&rn)) return -3;
                        if(x->op==QP_LIKE) v.value=glob(right,rn,left,ln);
                        else {
                            int cmp=memcmp(left,right,ln<rn?ln:rn); if(!cmp) cmp=(ln>rn)-(ln<rn);
                            v.value=x->op==Q_EQ?cmp==0:x->op==Q_NE?cmp!=0:x->op==Q_LT?cmp<0:x->op==Q_LE?cmp<=0:x->op==Q_GT?cmp>0:cmp>=0;
                        }
                    }
                }
            } else return qs_evaluate(e,index,out);
        }
    }
    e->valid[index]=1; e->cells[index]=v; *out=v; return 0;
}
static int project(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row)
{
    struct qs_eval e; init_eval(&e,&n->expr,input,aggregates); struct qs_cell keep;
    if(n->expr.predicate>=0) {
        if(evaluate(s,&e,n->expr.predicate,&keep)) return -5;
        if(keep.isnull || !keep.value) return 0;
    }
    for(unsigned i=0;i<n->expr.nout;++i)
        if(evaluate(s,&e,n->expr.outputs[i].index,&row[i])) return -5;
    return 1;
}
static int64_t sort_call(struct execution *s,struct sorted_rows *sort,int source)
{
    void *mr[4]; long long len[4];
    struct femu_csd_args a={.numr=source?3:4,.mr_addr=mr,.mr_len=len};
    if(source) {
        mr[0]=&sort->group; len[0]=sizeof(sort->group);
        mr[1]=sort->output; len[1]=sizeof(*sort->output); mr[2]=s->io; len[2]=s->io_bytes;
    } else {
        mr[0]=sort->input; len[0]=sizeof(*sort->input); mr[1]=&sort->plan; len[1]=sizeof(sort->plan);
        mr[2]=&sort->task; len[2]=sizeof(sort->task); mr[3]=s->io; len[3]=s->io_bytes;
    }
    if(s->pending_sort==sort) { a.cparam2=s->args->cparam2; s->pending_sort=NULL; }
    int64_t status=source?pg_sina_sort_source_impl(&a):pg_sina_sort_sink_impl(&a);
    if(status>0) s->pending_sort=sort;
    if(source && !status) {
        const struct qs_metrics *m=&sort->output->metrics,*old=&sort->metrics;
        s->metrics.spill_read_bytes+=m->spill_read_bytes-old->spill_read_bytes;
        s->metrics.spill_write_bytes+=m->spill_write_bytes-old->spill_write_bytes;
        s->metrics.runs+=m->runs-old->runs;
        s->metrics.spilled_runs+=m->spilled_runs-old->spilled_runs;
        sort->metrics=*m;
    }
    return status;
}
static int64_t sort_init(struct execution *s,struct sorted_rows *sort,const struct qs_plan *input,
    unsigned keys,const int *columns,const struct qs_order *order)
{
    if(sort->initialized) return 0;
    sort->input=cs_work_calloc(s->context,1,sizeof(*sort->input));
    sort->output=cs_work_calloc(s->context,1,sizeof(*sort->output));
    if(!sort->input || !sort->output) return -4;
    sort->plan.nout=input->nout; sort->plan.ordered_rows=2; sort->plan.limit=-1;
    sort->plan.work_mem_bytes=s->sort_limit;
    sort->plan.work_mem_parent=(uintptr_t)s->context;
    memcpy(sort->plan.outputs,input->outputs,input->nout*sizeof(input->outputs[0]));
    for(unsigned i=0;i<keys;++i) sort->plan.order[sort->plan.norder++]=order?order[i]:(struct qs_order){.output=columns[i]};
    sort->task.workers=1;
    int64_t status=sort_call(s,sort,0); if(status) return status;
    qs_sort_dictionary(sort->task.state,s->dictionary);
    sort->initialized=1; return 0;
}
static int64_t sorted_input(struct execution *s,struct sorted_rows *sort,unsigned child,
    unsigned keys,const int *columns,const struct qs_order *order,struct qs_cell *row)
{
    const struct qp_node *input=&s->plan->nodes[child];
    int64_t status=sort_init(s,sort,&input->expr,keys,columns,order); if(status) return status;
    if(sort->drained && !sort->output) return 0;
    while(!sort->complete) {
        if(sort->input_pending) {
            status=sort_call(s,sort,0); if(status) return status;
            sort->input_pending=0; sort->input->rows=0;
        }
        if(sort->prefix_position<sort->prefix_rows) {
            memcpy(sort->input->data[sort->input->rows].cells,
                sort->prefix+(size_t)sort->prefix_position++*input->expr.nout,input->expr.nout*sizeof(struct qs_cell));
            status=2;
        } else {
            if(sort->prefix) { cs_work_free(s->context,sort->prefix); sort->prefix=NULL; }
            status=next_row(s,child,sort->input->data[sort->input->rows].cells);
        }
        if(status<0 || status>2) return status;
        if(status==QP_WAIT) return QP_WAIT;
        /* next_row returns 2 for a row and 0 for EOF. */
        if(!status) {
            if(sort->input->rows) { sort->input_pending=1; continue; }
            sort->complete=1; sort->group.count=1; sort->group.states[0]=sort->task.state;
            cs_work_free(s->context,sort->input); sort->input=NULL; break;
        }
        if(++sort->input->rows==QS_RESULT_ROWS) sort->input_pending=1;
    }
    if((s->pending_sort==sort || sort->position==sort->output->rows) && !sort->drained) {
        status=sort_call(s,sort,1); if(status) return status;
        sort->position=0; sort->drained=sort->output->finished;
    }
    if(sort->position<sort->output->rows) {
        memcpy(row,sort->output->data[sort->position++].cells,input->expr.nout*sizeof(*row)); return 2;
    }
    if(sort->drained) { cs_work_free(s->context,sort->output); sort->output=NULL; }
    return 0;
}
static uint64_t mix_hash(uint64_t x)
{ x^=x>>33; x*=UINT64_C(0xff51afd7ed558ccd); x^=x>>33; x*=UINT64_C(0xc4ceb9fe1a85ec53); return x^(x>>33); }
static uint64_t row_hash(const struct qp_node *n,const struct qs_cell *row,int right)
{
    uint64_t hash=0x9e3779b97f4a7c15ULL;
    for(unsigned i=0;i<n->nkeys;++i) {
        struct qs_cell v=row[right?n->right_keys[i]:n->left_keys[i]];
        while(v.scale && !(v.value%10)) { v.value/=10; --v.scale; }
        hash=mix_hash(hash^(uint64_t)v.value^(uint64_t)((unsigned __int128)v.value>>64)^((uint64_t)v.scale<<56));
    }
    return hash;
}
static int64_t hash_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned rn=s->plan->nodes[n->right].expr.nout,ln=s->plan->nodes[n->left].expr.nout;
    if(o->hash_fallback) return join_next(s,id,row);
    size_t stride=rn*sizeof(struct qs_cell)+3*sizeof(uint32_t);
    if(!o->hash_capacity) {
        uint64_t estimate=s->plan->nodes[n->right].estimated_rows;
        unsigned max=1; while((size_t)max*2*stride<=s->sort_limit && max<UINT_MAX/4) max*=2;
        unsigned capacity=128; while(capacity<estimate && capacity<max) capacity*=2;
        if(capacity>max) capacity=max;
        o->hash_data=cs_work_alloc(s->context,capacity*stride);
        if(!o->hash_data) { o->hash_fallback=1; return join_next(s,id,row); }
        o->hash_capacity=capacity;
    }
    while(!o->hash_phase) {
        if(o->hash_rows==o->hash_capacity) {
            size_t bytes=(size_t)o->hash_capacity*2*stride;
            void *data=bytes<=s->sort_limit?cs_work_realloc(s->context,o->hash_data,bytes):NULL;
            if(!data) {
                /* Preserve already-built projected rows. The fallback sorts
                 * this prefix plus the remaining child stream, never rereads
                 * or decodes the original heap for another build batch. */
                o->right_sort.prefix=o->hash_data; o->right_sort.prefix_rows=o->hash_rows;
                o->hash_data=NULL; o->hash_fallback=1; return join_next(s,id,row);
            }
            o->hash_data=data; o->hash_capacity*=2;
        }
        int64_t status=next_row(s,n->right,o->hash_data+(size_t)o->hash_rows*rn);
        if(status==QP_WAIT || status<0 || status>2) return status;
        if(status==2) { ++o->hash_rows; ++s->metrics.build_rows; continue; }
        uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*rn),*buckets=next+o->hash_capacity;
        memset(buckets,0xff,(size_t)o->hash_capacity*2*sizeof(*buckets));
        for(unsigned i=0;i<o->hash_rows;++i) {
            const struct qs_cell *v=o->hash_data+(size_t)i*rn;
            unsigned bucket=row_hash(n,v,1)&(2*o->hash_capacity-1);
            next[i]=buckets[bucket]; buckets[bucket]=i;
        }
        o->hash_phase=1;
    }
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*rn),*buckets=next+o->hash_capacity;
    for(;;) {
        if(!o->have_left) {
            int64_t status=next_row(s,n->left,o->left);
            if(status!=2) {
                if(!status) { cs_work_free(s->context,o->hash_data); o->hash_data=NULL; o->finished=1; }
                return status;
            }
            o->have_left=1; o->hash_matched=0;
            unsigned at=keys_null(n,o->left)?UINT_MAX:buckets[row_hash(n,o->left,0)&(2*o->hash_capacity-1)];
            o->hash_match=at==UINT_MAX?0:at+1;
        }
        while(o->hash_match) {
            unsigned at=o->hash_match-1; o->hash_match=next[at]==UINT_MAX?0:next[at]+1;
            const struct qs_cell *right=o->hash_data+(size_t)at*rn;
            if(key_compare(s,n,o->left,right)) continue;
            int null=0; for(unsigned i=0;i<n->nkeys;++i) null|=right[n->right_keys[i]].isnull;
            if(null) continue;
            struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values)); memcpy(values+ln,right,rn*sizeof(*values));
            struct qs_eval e; init_eval(&e,&n->expr,values,NULL); struct qs_cell match={.value=1};
            if(n->join_qual>=0 && evaluate(s,&e,n->join_qual,&match)) return -5;
            if(match.isnull || !match.value) continue;
            o->hash_matched=1; ++s->metrics.join_rows;
            if(n->join_type==QP_ANTI) { o->hash_match=0; break; }
            if(n->join_type==QP_SEMI) { o->hash_match=0; o->have_left=0; }
            int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
        }
        int unmatched=!o->hash_matched && (n->join_type==QP_LEFT || n->join_type==QP_ANTI);
        o->have_left=0;
        if(unmatched) {
            struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
            for(unsigned i=0;i<rn;++i) values[ln+i]=(struct qs_cell){.isnull=1};
            int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
        }
    }
}
static int64_t spool_append(struct execution *s,struct spool *p,const struct qs_cell *row,unsigned cols)
{
    unsigned row_bytes=cols*sizeof(*row),block_rows=QS_SORT_IO_BYTES/row_bytes;
    if(!p->tail) {
        p->tail=cs_work_alloc(s->context,QS_SORT_IO_BYTES); p->loaded=UINT_MAX;
        if(!p->tail) return -4;
    }
    if(p->used==block_rows) {
        if(p->blocks==p->capacity) {
            unsigned capacity=p->capacity?p->capacity*2:8;
            void *tokens=cs_work_realloc(s->context,p->tokens,capacity*sizeof(*p->tokens)); if(!tokens) return -4;
            p->tokens=tokens; p->capacity=capacity;
        }
        memcpy(s->io,p->tail,QS_SORT_IO_BYTES); s->pending_spool=p; s->pending_store=1;
        return csNeedStore(QS_SORT_IO_BYTES);
    }
    memcpy(p->tail+(size_t)p->used*row_bytes,row,row_bytes); ++p->used; ++p->rows; return 0;
}
static int64_t spool_row(struct execution *s,struct spool *p,uint64_t index,unsigned cols,struct qs_cell *row)
{
    unsigned row_bytes=cols*sizeof(*row),block_rows=QS_SORT_IO_BYTES/row_bytes,block=index/block_rows;
    if(index>=p->rows) return -1;
    unsigned char *data=p->tail;
    if(block<p->blocks) {
        if(!p->cache) { p->cache=cs_work_alloc(s->context,QS_SORT_IO_BYTES); if(!p->cache) return -4; }
        if(p->loaded!=block) {
            s->pending_spool=p; s->pending_store=0; s->pending_block=block;
            return csNeedLoad(p->tokens[block]);
        }
        data=p->cache;
    }
    memcpy(row,data+(index%block_rows)*row_bytes,row_bytes); return 0;
}
static int64_t materialized_row(struct execution *s,unsigned id,uint64_t position,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned cols=s->plan->nodes[n->left].expr.nout;
    while(!o->group_ready) {
        if(!o->have_left) {
            int64_t status=next_row(s,n->left,o->left);
            if(status==QP_WAIT || status<0 || status>2) return status;
            if(!status) { o->group_ready=1; break; }
            o->have_left=1;
        }
        int64_t status=spool_append(s,&o->group,o->left,cols); if(status) return status;
        o->have_left=0;
    }
    if(position==o->group.rows) return 0;
    int64_t status=spool_row(s,&o->group,position,cols,row); return status?status:2;
}
static int64_t build_set(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id]; const struct qp_node *n=&s->plan->nodes[id];
    while(!o->set_ready) {
        int64_t status=next_row(s,n->left,o->left);
        if(status==QP_WAIT || status<0 || status>2) return status;
        if(!status) { o->set_ready=1; return 0; }
        struct qs_cell v=o->left[0];
        if(v.isnull) { o->set_null=1; continue; }
        if(v.value>INT64_MAX || v.value<INT64_MIN) return -5;
        if(!o->set_capacity || o->set_count*2>=o->set_capacity) {
            unsigned capacity=o->set_capacity?o->set_capacity*2:64;
            struct set_key *next=cs_work_calloc(s->context,capacity,sizeof(*next)); if(!next) return -4;
            for(unsigned i=0;i<o->set_capacity;++i) if(o->set[i].occupied) {
                unsigned slot=mix_hash((uint64_t)o->set[i].value)&(capacity-1);
                while(next[slot].occupied) slot=(slot+1)&(capacity-1);
                next[slot]=o->set[i];
            }
            if(o->set) cs_work_free(s->context,o->set);
            o->set=next; o->set_capacity=capacity;
        }
        unsigned slot=mix_hash((uint64_t)v.value)&(o->set_capacity-1);
        while(o->set[slot].occupied && o->set[slot].value!=v.value) slot=(slot+1)&(o->set_capacity-1);
        if(!o->set[slot].occupied) { o->set[slot]=(struct set_key){.value=v.value,.occupied=1}; ++o->set_count; }
    }
    return 0;
}
static int key_compare(struct execution *s,const struct qp_node *n,const struct qs_cell *left,const struct qs_cell *right)
{
    const struct qs_plan *types=&s->plan->nodes[n->left].expr;
    for(unsigned i=0;i<n->nkeys;++i) {
        unsigned col=n->left_keys[i];
        unsigned type=types->exprs[types->outputs[col].index].type;
        struct qs_cell a=left[col],b=right[n->right_keys[i]];
        int cmp=compare_cell(s,a,b,type);
        if(cmp==INT_MIN) return cmp;
        if(n->expr.norder) {
            if(a.isnull || b.isnull) { if(n->expr.order[i].nulls_first) cmp=-cmp; }
            else if(n->expr.order[i].descending) cmp=-cmp;
        }
        if(cmp) return cmp;
    }
    return 0;
}
static int keys_null(const struct qp_node *n,const struct qs_cell *left)
{ for(unsigned i=0;i<n->nkeys;++i) if(left[n->left_keys[i]].isnull) return 1; return 0; }
static int64_t join_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned ln=s->plan->nodes[n->left].expr.nout,rn=s->plan->nodes[n->right].expr.nout;
    struct qs_order right_order[QS_KEYS];
    for(unsigned i=0;i<n->expr.norder;++i) { right_order[i]=n->expr.order[i]; right_order[i].output=n->right_keys[i]; }
    const struct qs_order *left_order=n->expr.norder?n->expr.order:NULL;
    for(;;) {
        int64_t status;
        if(o->phase==2) {
            status=sorted_input(s,&o->left_sort,n->left,n->nkeys,n->left_keys,left_order,o->left);
            if(status!=2) return status;
            o->have_left=1; o->position=0; o->matched=0; o->phase=0;
            const struct qs_plan *types=&s->plan->nodes[n->left].expr;
            int same=1;
            for(unsigned i=0;i<n->nkeys;++i) {
                unsigned col=n->left_keys[i];
                int cmp=compare_cell(s,o->left[col],o->first[col],types->exprs[types->outputs[col].index].type);
                if(cmp==INT_MIN) return -5;
                if(cmp) same=0;
            }
            if(!same) { o->group_ready=0; o->group.rows=o->group.blocks=o->group.used=0; o->group.loaded=UINT_MAX; }
        }
        if(!o->have_left) {
            status=sorted_input(s,&o->left_sort,n->left,n->nkeys,n->left_keys,left_order,o->left);
            if(status!=2) return status;
            o->have_left=1; o->position=0; o->matched=0;
        }
        if(!o->group_ready) {
            if(!o->have_right && !o->input_done) {
                status=sorted_input(s,&o->right_sort,n->right,n->nkeys,n->right_keys,n->expr.norder?right_order:NULL,o->right);
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
                    struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
                    for(unsigned i=0;i<rn;++i) values[ln+i]=(struct qs_cell){.isnull=1};
                    int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
                }
                continue;
            }
        }
        if(o->position<o->group.rows) {
            struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
            status=spool_row(s,&o->group,o->position,rn,values+ln); if(status) return status;
            ++o->position;
            struct qs_eval e; init_eval(&e,&n->expr,values,NULL); struct qs_cell match={.value=1};
            if(n->join_qual>=0 && evaluate(s,&e,n->join_qual,&match)) return -5;
            if(match.isnull || !match.value) continue;
            o->matched=1;
            if(n->join_type==QP_ANTI) { o->position=o->group.rows; continue; }
            if(n->join_type==QP_SEMI) o->position=o->group.rows;
            int count=project(s,n,values,NULL,row); if(count<0) return count;
            if(count) { ++s->metrics.join_rows; return 2; }
            continue;
        }
        if(!o->matched && (n->join_type==QP_LEFT || n->join_type==QP_ANTI)) {
            struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
            for(unsigned i=0;i<rn;++i) values[ln+i]=(struct qs_cell){.isnull=1};
            o->matched=1;
            int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
        }
        /* Keep the right duplicate group for another left row with this key. */
        memcpy(o->first,o->left,ln*sizeof(*o->first)); o->have_left=0; o->phase=2;
    }
}
static int64_t scan_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; const struct qs_plan *p=&n->expr; struct operator_state *o=&s->nodes[id];
    struct qs_chunk *chunk=(void *)(s->arena+n->vector_offset);
    for(;;) {
        if(o->scan_position<chunk->rows) {
            unsigned at=o->scan_position++; struct qs_cell values[QS_COLS];
            for(unsigned i=0;i<p->nattrs;++i) if(p->cols[i].slot>=0) {
                unsigned slot=p->cols[i].slot;
                values[i]=(struct qs_cell){.value=qs_values(chunk)[(size_t)slot*chunk->capacity+at],
                    .scale=p->cols[i].scale,.dscale=qs_dscales(chunk)[(size_t)slot*chunk->capacity+at],
                    .isnull=(qs_nulls(chunk)[(size_t)slot*((chunk->capacity+7)/8)+at/8]>>(at%8))&1};
            }
            int count=project(s,n,values,NULL,row); if(count<0) return count;
            if(count) { ++s->metrics.matched; return 2; }
            continue;
        }
        if(s->control->ready && s->control->node==id && !s->control->index_nreads) {
            struct qs_task task={.input_bytes=s->control->bytes};
            void *mr[]={s->arena+n->input_offset,(void *)p,chunk,&task,s->dictionary};
            long long len[]={QP_SCAN_BYTES,sizeof(*p),qs_chunk_bytes(chunk->capacity,chunk->ncols),sizeof(task),0};
            struct femu_csd_args a={.numr=5,.mr_addr=mr,.mr_len=len};
            int64_t status=pg_sina_scan_impl(&a); if(status) return status;
            s->metrics.scanned+=chunk->rows; s->metrics.input_bytes+=task.input_bytes; ++s->metrics.chunks;
            o->scan_position=0; o->position+=task.input_bytes; s->control->ready=0; s->control->bytes=0; continue;
        }
        if(o->position==n->bytes) return 0;
        s->control->node=id; s->control->offset=o->position;
        s->control->bytes=n->bytes-o->position<QP_SCAN_BYTES?n->bytes-o->position:QP_SCAN_BYTES;
        return QP_WAIT;
    }
}
static int group_same(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    struct qs_eval e; init_eval(&e,&n->expr,o->left,NULL);
    for(unsigned k=0;k<n->expr.nkeys;++k) {
        struct qs_cell key; if(evaluate(s,&e,n->expr.keys[k],&key)) return -1;
        int cmp=compare_cell(s,key,o->keys[k],n->expr.exprs[n->expr.keys[k]].type);
        if(cmp==INT_MIN) return -1;
        if(cmp) return 0;
    }
    return 1;
}
static int accumulate(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    struct qs_eval e; init_eval(&e,&n->expr,o->left,NULL);
    for(unsigned i=0;i<n->expr.naggs;++i) {
        const struct qs_agg *agg=&n->expr.aggs[i]; struct accumulator *a=&o->accum[i]; struct qs_cell v={.value=1};
        if(agg->kind!=Q_COUNT_STAR && evaluate(s,&e,agg->expr,&v)) return -5;
        if(v.isnull) continue;
        if(n->distinct_aggs&(1U<<i)) {
            int cmp=1;
            if(a->has_last) cmp=compare_cell(s,a->last,v,n->expr.exprs[agg->expr].type);
            if(cmp==INT_MIN) return -5;
            if(!cmp) continue;
            a->last=v; a->has_last=1;
        }
        if(agg->kind==Q_MIN || agg->kind==Q_MAX) {
            int cmp=a->count?compare_cell(s,v,a->extreme,n->expr.exprs[agg->expr].type):0;
            if(cmp==INT_MIN) return -5;
            if(!a->count || (agg->kind==Q_MIN?cmp<0:cmp>0)) a->extreme=v;
        } else if(agg->kind==Q_SUM || agg->kind==Q_AVG) {
            if(!a->count) a->sum=v;
            else {
                unsigned scale=v.scale>a->sum.scale?v.scale:a->sum.scale; __int128 left,right;
                if(qs_scale(a->sum.value,scale-a->sum.scale,&left) || qs_scale(v.value,scale-v.scale,&right) ||
                    __builtin_add_overflow(left,right,&a->sum.value)) return -5;
                a->sum.scale=scale; if(v.dscale>a->sum.dscale) a->sum.dscale=v.dscale;
            }
        }
        ++a->count;
    }
    return 0;
}
static int64_t aggregate_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    for(;;) {
        if(!o->have_left && !o->input_done) {
            int64_t status;
            if(n->nkeys && (!n->sorted_input || n->distinct)) {
                int keys[QS_KEYS+1]; memcpy(keys,n->left_keys,n->nkeys*sizeof(*keys)); unsigned count=n->nkeys;
                if(n->distinct) keys[count++]=n->distinct-1;
                status=sorted_input(s,&o->left_sort,n->left,count,keys,NULL,o->left);
            } else status=next_row(s,n->left,o->left);
            if(status==QP_WAIT || status<0 || status>2) return status;
            o->have_left=status==2; o->input_done=!status;
        }
        if(!o->phase) {
            if(!o->have_left && (n->nkeys || o->emitted)) return 0;
            memset(o->accum,0,sizeof(o->accum));
            if(o->have_left) {
                memcpy(o->first,o->left,sizeof(o->first));
                struct qs_eval e; init_eval(&e,&n->expr,o->left,NULL);
                for(unsigned k=0;k<n->expr.nkeys;++k) if(evaluate(s,&e,n->expr.keys[k],&o->keys[k])) return -5;
            }
            o->phase=1;
        }
        if(o->have_left) {
            int same=group_same(s,n,o); if(same<0) return -5;
            if(same) {
                int status=accumulate(s,n,o); if(status) return status;
                o->have_left=0; continue;
            }
        }
        struct qs_cell values[QS_AGGS];
        for(unsigned i=0;i<n->expr.naggs;++i) {
            const struct qs_agg *agg=&n->expr.aggs[i]; const struct accumulator *a=&o->accum[i];
            struct qs_cell v={.isnull=!a->count};
            if(agg->kind==Q_COUNT || agg->kind==Q_COUNT_STAR) v=(struct qs_cell){.value=a->count};
            else if(a->count) {
                v=agg->kind==Q_MIN || agg->kind==Q_MAX?a->extreme:a->sum;
                if(agg->kind==Q_AVG && qs_average(v,a->count,&v)) return -5;
            }
            values[i]=v;
        }
        o->phase=0; ++o->emitted;
        int count=project(s,n,o->first,values,row); if(count<0) return count;
        if(count) return 2;
    }
}

/* index_scan.c calls this consumer with a batch of decoded heap matches.
 * Only the lookup key is narrowed to int64; left payloads keep 128-bit cells. */
int64_t qs_join_consume(unsigned stage,struct qs_chunk *chunk,const struct qs_plan *unused,
    struct qs_task *task,struct qs_join_control *control,struct femu_csd_args *args,int resume)
{
    (void)stage; (void)unused; (void)task; (void)control; (void)args; (void)resume;
    struct execution *s=index_execution; unsigned id=index_node;
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    struct operator_state *o=&s->nodes[id]; unsigned ln=s->plan->nodes[n->left].expr.nout;
    if(o->index_flush) { o->index_flush=0; return 0; }
    o->index_output->rows=0; o->index_position=0;
    for(unsigned r=0;r<chunk->selected;++r) {
        unsigned at=qs_selection(chunk)[r]; struct qs_cell raw[QS_COLS],values[QS_COLS];
        unsigned source=qs_values(chunk)[chunk->capacity+at];
        if(source>=o->index_count) return -1;
        memcpy(values,o->index_left+(size_t)source*ln,ln*sizeof(*values));
        for(unsigned col=0;col<right->expr.nattrs;++col) if(right->expr.cols[col].slot>=0) {
            unsigned slot=2+right->expr.cols[col].slot;
            raw[col]=(struct qs_cell){.value=qs_values(chunk)[(size_t)slot*chunk->capacity+at],
                .scale=right->expr.cols[col].scale,.dscale=qs_dscales(chunk)[(size_t)slot*chunk->capacity+at],
                .isnull=(qs_nulls(chunk)[(size_t)slot*((chunk->capacity+7)/8)+at/8]>>(at%8))&1};
        }
        int status=project(s,right,raw,NULL,values+ln); if(status<0) return status; if(!status) continue;
        struct qs_eval e; init_eval(&e,&n->expr,values,NULL); struct qs_cell match={.value=1};
        if(n->join_qual>=0 && evaluate(s,&e,n->join_qual,&match)) return -5;
        if(match.isnull || !match.value) continue;
        uint64_t bit=UINT64_C(1)<<source;
        if(n->join_type==QP_SEMI && (o->index_matches&bit)) continue;
        o->index_matches|=bit; ++s->metrics.join_rows;
        if(n->join_type==QP_ANTI) continue;
        status=project(s,n,values,NULL,o->index_output->data[o->index_output->rows].cells);
        if(status<0) return status;
        if(status) ++o->index_output->rows;
    }
    if(o->index_output->rows) { o->index_flush=1; return QP_INDEX_YIELD; }
    return 0;
}
static int64_t index_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    struct operator_state *o=&s->nodes[id]; unsigned ln=s->plan->nodes[n->left].expr.nout;
    if(!o->index_plan) {
        struct qs_plan *p=cs_work_calloc(s->context,1,sizeof(*p));
        o->index_output=cs_work_calloc(s->context,1,sizeof(*o->index_output));
        o->index_left=cs_work_alloc(s->context,64*ln*sizeof(*o->index_left));
        if(!p || !o->index_output || !o->index_left) return -4;
        o->index_plan=p; p->nattrs=right->expr.nattrs+2; p->ncols=right->expr.ncols+2; p->njoins=1;
        p->work_mem_parent=(uintptr_t)s->context;
        p->join=right->expr.join; p->join.relid=right->expr.relid; p->join.relfilenode=right->expr.relfilenode;
        p->join.nattrs=right->expr.nattrs; p->join.probe_key=0; p->join.predicate=-1; p->join.work_mem_bytes=QS_INDEX_WORK_BYTES;
        ++p->join.build_key;
        p->cols[0]=(struct qs_column){.slot=0,.type=Q_INTEGER,.length=8,.align=8};
        p->cols[1]=p->cols[0]; p->cols[1].slot=1;
        for(unsigned i=0;i<right->expr.nattrs;++i) {
            p->cols[i+2]=right->expr.cols[i];
            if(p->cols[i+2].slot>=0) p->cols[i+2].slot+=2;
        }
        int status=qs_index_init(p,&o->index_control); if(status) return status;
        qs_index_dictionary(&o->index_control,s->dictionary);
        struct qs_chunk *input=(void *)(s->arena+n->vector_offset),*output=(void *)(s->arena+n->vector_offset+4096);
        *input=(struct qs_chunk){.capacity=64,.ncols=2};
        *output=(struct qs_chunk){.capacity=256,.ncols=p->ncols};
    }
    for(;;) {
        if(o->index_position<o->index_output->rows) {
            memcpy(row,o->index_output->data[o->index_position++].cells,n->expr.nout*sizeof(*row)); return 2;
        }
        o->index_output->rows=0; o->index_position=0;
        if(o->phase==2) {
            while(o->index_unmatched<o->index_count) {
                unsigned at=o->index_unmatched++;
                if((n->join_type!=QP_LEFT && n->join_type!=QP_ANTI) || (o->index_matches&(UINT64_C(1)<<at))) continue;
                struct qs_cell values[QS_COLS]; memcpy(values,o->index_left+(size_t)at*ln,ln*sizeof(*values));
                for(unsigned i=0;i<right->expr.nout;++i) values[ln+i]=(struct qs_cell){.isnull=1};
                int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
            }
            o->index_count=0; o->phase=0;
        }
        if(!o->phase) {
            unsigned batch=n->sorted_input?1:64;
            while(o->index_count<batch && !o->input_done) {
                int64_t status=next_row(s,n->left,o->index_left+(size_t)o->index_count*ln);
                if(status==QP_WAIT || status<0 || status>2) return status;
                if(!status) o->input_done=1; else ++o->index_count;
            }
            if(!o->index_count) return 0;
            o->index_matches=0; o->index_resume=0; o->index_flush=0; o->index_unmatched=0; o->phase=1;
            struct qs_chunk *input=(void *)(s->arena+n->vector_offset);
            input->rows=input->selected=o->index_count; memset(qs_nulls(input),0,16);
            for(unsigned i=0;i<o->index_count;++i) {
                struct qs_cell key=o->index_left[(size_t)i*ln+n->left_keys[0]];
                if(key.value>INT64_MAX || key.value<INT64_MIN) return -5;
                qs_values(input)[i]=key.value; qs_values(input)[64+i]=i;
                if(key.isnull) qs_nulls(input)[i/8]|=1U<<(i%8);
                qs_selection(input)[i]=i;
            }
        }
        if(s->control->ready && s->control->node==id && s->control->index_nreads) {
            s->control->ready=s->control->index_nreads=0;
        }
        struct qs_plan *p=o->index_plan;
        void *mr[]={s->arena+n->vector_offset,p,&o->index_task,&o->index_control,
            s->arena+n->vector_offset+4096,s->arena+n->input_offset};
        long long len[]={qs_chunk_bytes(64,2),sizeof(*p),sizeof(o->index_task),sizeof(o->index_control),
            qs_chunk_bytes(256,p->ncols),QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE};
        struct femu_csd_args a={.numr=6,.mr_addr=mr,.mr_len=len};
        index_execution=s; index_node=id;
        int64_t status=qs_index_probe(0,mr[0],p,&o->index_task,&o->index_control,&a,o->index_resume);
        o->index_resume=1;
        if(status==QP_INDEX_YIELD) continue;
        if(status==QS_INDEX_WAIT && o->index_task.index_pending) {
            s->control->node=id; s->control->index_nreads=o->index_task.index_nreads;
            memcpy(s->control->index_reads,o->index_task.index_reads,s->control->index_nreads*sizeof(s->control->index_reads[0]));
            return QP_WAIT;
        }
        if(status) return status;
        o->phase=2;
    }
}
static int64_t next_row_impl(struct execution *s,unsigned id,struct qs_cell *row)
{
    if(id>=s->plan->count) return -1;
    struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    if(o->finished) return 0;
    if(n->kind==QP_SCAN) return scan_next(s,id,row);
    if(n->kind==QP_JOIN) return n->hash?hash_next(s,id,row):join_next(s,id,row);
    if(n->kind==QP_INDEX_JOIN) return index_next(s,id,row);
    if(n->kind==QP_AGGREGATE) return aggregate_next(s,id,row);
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
        if(n->kind==QP_SORT) status=sorted_input(s,&o->left_sort,n->left,n->expr.norder,NULL,n->expr.order,o->left);
        else if(n->kind==QP_CTE_SCAN) {
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
static unsigned sort_depth(const struct qp_plan *plan,unsigned id)
{
    const struct qp_node *n=&plan->nodes[id]; unsigned left=n->left<0?0:sort_depth(plan,n->left),right=n->right<0?0:sort_depth(plan,n->right);
    if(n->kind==QP_JOIN) return (left>right?left:right)+2;
    return left+(n->kind==QP_SORT || (n->kind==QP_AGGREGATE && n->nkeys && (!n->sorted_input || n->distinct)));
}
static void drop_sort(struct sorted_rows *sort)
{
    if(!sort->initialized || !sort->task.state || sort->drained) return;
    struct qs_group group={.count=1,.states={sort->task.state}};
    void *mr[]={&group}; long long len[]={sizeof(group)};
    struct femu_csd_args a={.numr=1,.mr_addr=mr,.mr_len=len};
    pg_sina_sort_source_impl(&a); sort->task.state=0;
}
static void close_execution(struct execution *s)
{
    for(unsigned i=0;i<s->plan->count;++i) { drop_sort(&s->nodes[i].left_sort); drop_sort(&s->nodes[i].right_sort); }
    for(unsigned i=0;i<s->plan->count;++i) if(s->nodes[i].index_control.state) qs_index_close(&s->nodes[i].index_control);
    cs_work_context *context=s->context; s->control->state=0; cs_work_end(context);
}
/* plan, lifecycle/page requests, result, input/dictionary arena, spill I/O.
 * A successful call either returns result rows or asks PG to LOAD pages.
 * NEED_STORE/LOAD is handled entirely by the existing SNIA batch scheduler. */
int64_t pg_sina_plan(struct femu_csd_args *a)
{
    if(!a || a->numr!=5 || a->mr_len[0]<(long long)sizeof(struct qp_plan) ||
        a->mr_len[1]<(long long)sizeof(struct qp_control) || a->mr_len[2]<(long long)sizeof(struct qs_result) ||
        a->mr_len[4]<QS_SORT_IO_BYTES) return -1;
    struct qp_plan *plan=a->mr_addr[0]; struct qp_control *control=a->mr_addr[1]; struct qs_result *out=a->mr_addr[2];
    struct execution *s=(void *)(uintptr_t)control->state;
    if(control->close) { if(s) close_execution(s); return 0; }
    if(!s) {
        if(!plan->count || plan->count>QP_NODES || plan->root>=plan->count) return -1;
        struct cs_work_stats stats; if(cs_work_get_stats(&stats)) return -4;
        size_t budget=plan->nodes[plan->root].expr.work_mem_bytes;
        if(!budget || budget>stats.capacity_bytes) budget=stats.capacity_bytes;
        cs_work_context *context=cs_work_begin_limit(budget); if(!context) return -4;
        s=cs_work_calloc(context,1,sizeof(*s)); if(!s) { cs_work_end(context); return -4; }
        s->context=context; s->plan=plan; s->control=control; s->dictionary=a->mr_addr[3];
        s->arena=a->mr_addr[3]; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
        s->nodes=cs_work_calloc(context,plan->count,sizeof(*s->nodes));
        if(!s->nodes) { cs_work_end(context); return -4; }
        unsigned depth=sort_depth(plan,plan->root);
        s->sort_limit=budget/(depth+2); if(s->sort_limit<512*1024) { cs_work_end(context); return -4; }
        s->profiling=a->cparam1!=0; control->state=(uintptr_t)s;
    }
    s->args=a;
    if(s->pending_spool) {
        if(!csIsReentry(a)) return -1;
        struct spool *p=s->pending_spool;
        if(s->pending_store) { p->tokens[p->blocks++]=csReentryValue(a); p->used=0; s->metrics.spill_write_bytes+=QS_SORT_IO_BYTES; }
        else { memcpy(p->cache,s->io,QS_SORT_IO_BYTES); p->loaded=s->pending_block; s->metrics.spill_read_bytes+=QS_SORT_IO_BYTES; }
        s->pending_spool=NULL;
    }
    if(!csIsReentry(a)) { out->rows=out->begin=out->finished=out->str_bytes=0; out->status=0; }
    uint64_t start=s->profiling?now_ns(CLOCK_MONOTONIC):0,cpu=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
    int64_t status=0;
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
    while(status>=0 && status!=QP_WAIT && status<=2 && out->rows<QS_RESULT_ROWS) {
        struct qs_cell *row=out->data[out->rows].cells;
        status=next_row(s,plan->root,row);
        if(status==QP_WAIT || status<0 || status>2) break;
        if(!status) { out->finished=control->finished=1; break; }
        const struct qs_plan *root=&plan->nodes[plan->root].expr;
        for(unsigned col=0;col<root->nout;++col) if(!row[col].isnull && root->exprs[root->outputs[col].index].type==Q_STRING) {
            const char *bytes; unsigned length;
            if(qs_dict_get(s->dictionary,(unsigned)row[col].value,&bytes,&length) || length>255 || out->str_bytes+length>QS_RESULT_STRING_BYTES) return -3;
            if(length<=QS_STRING_MAX) qs_cell_set_string(&row[col],bytes,length);
            else {
                memcpy(out->str_data+out->str_bytes,bytes,length); qs_cell_set_string_ref(&row[col],out->str_bytes,length); out->str_bytes+=length;
            }
        }
        ++out->rows;
    }
    if(s->profiling) {
        struct qs_timing *t=&s->profile.operators[QS_JOIN_PROBE];
        ++t->calls; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-start; t->cpu_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
    }
    out->metrics=s->metrics; out->metrics.workers=1; out->profile=s->profile;
    for(unsigned i=0;i<plan->count;++i) if(plan->nodes[i].kind==QP_INDEX_JOIN) {
        out->metrics.index_pages+=s->nodes[i].index_task.index_pages;
        out->metrics.index_tuples+=s->nodes[i].index_task.index_tuples;
    }
    out->metrics.input_bytes+=out->metrics.index_pages*QS_PAGE;
    struct cs_work_context_stats memory;
    if(!cs_work_get_context_stats(s->context,&memory)) out->metrics.work_peak=memory.peak_used_bytes;
    if(status<0) return status;
    if(out->finished) close_execution(s);
    return status==QP_WAIT?0:status>2?status:0;
}
