#include "femu-csd-kernel.h"
#include "query_vector.h"
#include "cs_workmem.h"
#include "heap_numeric.h"
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
/* Like DuckDB's typed SelectExpression loops: normalize a conjunction once,
 * then compare column vectors and emit only a selection vector. Other SQL
 * expressions retain the exact evaluator (including three-valued logic). */
struct comparison { const int64_t *values; const unsigned char *nulls; int64_t constant; unsigned op, expression; };
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
        qs_nulls(c)+(size_t)slot*((c->capacity+7)/8),(int64_t)value,op,(unsigned)(column-p->exprs)}; return 1;
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
/* Keep the SIMD prefix (e.g. date bounds), then pass its selection through
 * numeric predicates. Adjacent bounds on one column share a single decode;
 * no boolean vectors or decoded copies are needed for this simple AND path. */
static int select_deferred(const struct qs_plan *p,struct qs_chunk *c,const struct comparison *predicates,
    unsigned count,unsigned prefix,const void *raw,uint64_t bytes)
{
    if(!raw) return -1;
    if(prefix) {
#if defined(__x86_64__) || defined(__i386__)
        if(__builtin_cpu_supports("avx2")) select_avx2(c,predicates,prefix);
        else
#endif
            select_scalar(c,predicates,prefix,0);
    } else {
        c->selected=c->rows;
        for(unsigned i=0;i<c->rows;++i) qs_selection(c)[i]=i;
    }
    unsigned ends[QS_EXPRS];
    for(unsigned k=prefix;k<count;k=ends[k]) {
        ends[k]=k+1;
        while(ends[k]<count && predicates[ends[k]].values==predicates[k].values) ++ends[k];
    }
    unsigned selected=c->selected; c->selected=0;
    for(unsigned i=0;i<selected;++i) {
        unsigned row=qs_selection(c)[i],k=prefix;
        /* All remaining columns of this row are close in the original heap.
         * Test them together instead of revisiting the raw block per column. */
        for(;k<count;k=ends[k]) {
            const struct comparison *test=&predicates[k];
            const struct qs_column *col=&p->cols[p->exprs[test->expression].column];
            if(test->nulls[row/8]&(1U<<(row%8))) break;
            int64_t value=test->values[row];
            if(col->deferred) {
                struct qs_heap_ref ref; memcpy(&ref,&value,sizeof(ref));
                if(ref.offset>bytes || ref.bytes>bytes-ref.offset) return -2;
                unsigned scale;
                if(qs_numeric_cents((const unsigned char *)raw+ref.offset,ref.bytes,&value,&scale)) return -2;
            }
            unsigned j=k;
            for(;j<ends[k];++j) if(!matches(value,&predicates[j])) break;
            if(j!=ends[k]) break;
        }
        if(k==count) qs_selection(c)[c->selected++]=row;
    }
    return 0;
}
int64_t pg_sina_filter(struct femu_csd_args *a)
{
    if(!a || (a->numr!=2 && a->numr!=3) || a->mr_len[1]<(long long)sizeof(struct qs_plan)) return -1;
    if(a->numr==3 && (a->mr_len[2]<0 || !a->mr_addr[2])) return -1;
    struct qs_chunk *c=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1];
    if(a->mr_len[0]<(long long)sizeof(*c) || qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0] ||
        c->ncols!=p->ncols || c->rows>c->capacity || p->nattrs>QS_COLS || p->nexprs>QS_EXPRS) return -1;
    c->selected=0;
    struct comparison predicates[QS_EXPRS]; unsigned count=0;
    if(comparisons(p,c,p->predicate,predicates,&count) && count) {
        unsigned prefix=0;
        while(prefix<count && !p->cols[p->exprs[predicates[prefix].expression].column].deferred) ++prefix;
        if(prefix<count) {
            int status=select_deferred(p,c,predicates,count,prefix,a->numr==3?a->mr_addr[2]:NULL,a->numr==3?a->mr_len[2]:0);
            if(status) return status;
        } else {
#if defined(__x86_64__) || defined(__i386__)
            if(__builtin_cpu_supports("avx2")) select_avx2(c,predicates,count);
            else
#endif
                select_scalar(c,predicates,count,0);
        }
    } else {
        uint8_t needed[QS_EXPRS]={0};
        if(qs_vector_require(p,p->predicate,needed)) return -2;
        cs_work_context *context=cs_work_begin(); if(!context) return -4;
        struct qs_vector *vectors=cs_work_alloc(context,(p->predicate+1)*sizeof(*vectors));
        if(!vectors) { cs_work_end(context); return -4; }
        for(unsigned first=0;first<c->rows;first+=QS_VECTOR_ROWS) {
            unsigned n=c->rows-first; if(n>QS_VECTOR_ROWS) n=QS_VECTOR_ROWS;
            if(qs_vector_execute_raw(p,c,NULL,first,n,needed,vectors,
                a->numr==3?a->mr_addr[2]:NULL,a->numr==3?a->mr_len[2]:0)) { cs_work_end(context); return -2; }
            const struct qs_vector *result=&vectors[p->predicate];
            for(unsigned i=0;i<n;++i) {
                if(result->errors[i]) { cs_work_end(context); return -2; }
                if(!qs_vector_null(result,i) && qs_vector_value(result,i)) qs_selection(c)[c->selected++]=first+i;
            }
        }
        cs_work_end(context);
    }
    /* The raw LOAD buffer stays in AFDM until Aggregate.sink finishes.
     * Decode aggregate-only numerics here to avoid an extra CSF command. */
    for(unsigned col=0;col<p->nattrs;++col) {
        const struct qs_column *spec=&p->cols[col];
        if(!spec->deferred) continue;
        if(a->numr!=3 || spec->type!=Q_DECIMAL || spec->slot<0 || (unsigned)spec->slot>=c->ncols || a->mr_len[2]<0) return -1;
        int64_t *values=qs_values(c)+(size_t)spec->slot*c->capacity;
        unsigned char *nulls=qs_nulls(c)+(size_t)spec->slot*((c->capacity+7)/8);
        unsigned char *scales=qs_dscales(c)+(size_t)spec->slot*c->capacity;
        for(unsigned i=0;i<c->selected;++i) {
            unsigned row=qs_selection(c)[i];
            if(nulls[row/8]&(1U<<(row%8))) continue;
            struct qs_heap_ref ref; memcpy(&ref,&values[row],sizeof(ref));
            if(ref.offset>(uint64_t)a->mr_len[2] || ref.bytes>(uint64_t)a->mr_len[2]-ref.offset) return -2;
            unsigned dscale;
            if(qs_numeric_cents((const unsigned char *)a->mr_addr[2]+ref.offset,ref.bytes,&values[row],&dscale)) return -3;
            scales[row]=dscale;
        }
    }
    return 0;
}
