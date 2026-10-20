#include "femu-csd-kernel.h"
#include "query_eval.h"
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
/* Like DuckDB's typed SelectExpression loops: normalize a conjunction once,
 * then compare column vectors and emit only a selection vector. Other SQL
 * expressions retain the exact evaluator (including three-valued logic). */
struct comparison { const int64_t *values; const unsigned char *nulls; int64_t constant; unsigned op; };
static int comparisons(const struct qs_plan *p,struct qs_chunk *c,int index,
                       struct comparison *out,unsigned *count)
{
    if(index<0 || (unsigned)index>=p->nexprs) return 0;
    const struct qs_expr *e=&p->exprs[index];
    if(e->op==Q_AND)
        return comparisons(p,c,e->left,out,count) && comparisons(p,c,e->right,out,count);
    if(e->op<Q_EQ || e->op>Q_GE || e->left<0 || e->right<0 ||
        (unsigned)e->left>=p->nexprs || (unsigned)e->right>=p->nexprs) return 0;
    const struct qs_expr *column=&p->exprs[e->left],*constant=&p->exprs[e->right];
    unsigned op=e->op;
    if(column->op==Q_CONST && constant->op==Q_COLUMN) {
        const struct qs_expr *swap=column; column=constant; constant=swap;
        static const unsigned reversed[]={Q_EQ,Q_NE,Q_GT,Q_GE,Q_LT,Q_LE}; op=reversed[op-Q_EQ];
    }
    if(column->op!=Q_COLUMN || constant->op!=Q_CONST || constant->isnull ||
        column->column<0 || (unsigned)column->column>=p->nattrs || *count>=QS_EXPRS) return 0;
    int slot=p->cols[column->column].slot;
    if(slot<0 || (unsigned)slot>=c->ncols) return 0;
    __int128 value=constant->constant;
    if(column->scale>=constant->scale) {
        if(qs_scale(value,column->scale-constant->scale,&value)) return 0;
    } else {
        __int128 divisor; if(qs_scale(1,constant->scale-column->scale,&divisor) || value%divisor) return 0;
        value/=divisor;
    }
    if(value<INT64_MIN || value>INT64_MAX) return 0;
    out[(*count)++]=(struct comparison){qs_values(c)+(size_t)slot*c->capacity,
        qs_nulls(c)+(size_t)slot*((c->capacity+7)/8),(int64_t)value,op}; return 1;
}
static int matches(int64_t value,const struct comparison *p)
{
    switch(p->op) {
    case Q_EQ:return value==p->constant; case Q_NE:return value!=p->constant;
    case Q_LT:return value<p->constant; case Q_LE:return value<=p->constant;
    case Q_GT:return value>p->constant; default:return value>=p->constant;
    }
}
static void select_scalar(struct qs_chunk *c,const struct comparison *predicates,unsigned count,unsigned begin)
{
    for(unsigned row=begin;row<c->rows;++row) {
        unsigned k=0;
        for(;k<count;++k) {
            const struct comparison *p=&predicates[k];
            if((p->nulls[row/8]&(1U<<(row%8))) || !matches(p->values[row],p)) break;
        }
        if(k==count) qs_selection(c)[c->selected++]=row;
    }
}
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static void select_avx2(struct qs_chunk *c,const struct comparison *predicates,unsigned count)
{
    unsigned row=0;
    for(;row+4<=c->rows;row+=4) {
        unsigned mask=15;
        for(unsigned k=0;k<count && mask;++k) {
            const struct comparison *p=&predicates[k];
            __m256i values=_mm256_loadu_si256((const __m256i *)(p->values+row));
            __m256i constant=_mm256_set1_epi64x(p->constant),result;
            switch(p->op) {
            case Q_EQ:case Q_NE:result=_mm256_cmpeq_epi64(values,constant);break;
            case Q_LT:case Q_GE:result=_mm256_cmpgt_epi64(constant,values);break;
            default:result=_mm256_cmpgt_epi64(values,constant);break;
            }
            unsigned selected=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(result));
            if(p->op==Q_NE || p->op==Q_GE || p->op==Q_LE) selected^=15;
            mask&=selected & ~(p->nulls[row/8]>>(row%8));
        }
        while(mask) { unsigned bit=__builtin_ctz(mask); qs_selection(c)[c->selected++]=row+bit; mask&=mask-1; }
    }
    select_scalar(c,predicates,count,row);
}
#endif
int64_t pg_sina_filter(struct femu_csd_args *a)
{
    if(!a || a->numr!=2 || a->mr_len[1]<(long long)sizeof(struct qs_plan)) return -1;
    struct qs_chunk *c=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1];
    if(a->mr_len[0]<(long long)sizeof(*c) || qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0] ||
        c->ncols!=p->ncols || c->rows>c->capacity || p->nexprs>QS_EXPRS) return -1;
    c->selected=0;
    struct comparison predicates[QS_EXPRS]; unsigned count=0;
    /* For a lone predicate the existing pipeline currently has better query
     * latency (Q1 A/B), despite a faster comparison loop. Keep that path until
     * submission/wait scheduling is addressed; accelerate conjunctions only. */
    if(comparisons(p,c,p->predicate,predicates,&count) && count>1) {
#if defined(__x86_64__) || defined(__i386__)
        if(__builtin_cpu_supports("avx2")) select_avx2(c,predicates,count);
        else
#endif
            select_scalar(c,predicates,count,0);
        return 0;
    }
    for(unsigned row=0;row<c->rows;++row) {
        struct qs_eval e; e.plan=p; e.chunk=c; e.row=row; memset(e.valid,0,p->nexprs); struct qs_cell value;
        if(qs_evaluate(&e,p->predicate,&value)) return -2;
        if(!value.isnull && value.value) qs_selection(c)[c->selected++]=row;
    }
    return 0;
}
