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
struct comparison {
    const int64_t *values, *right;
    const unsigned char *nulls, *right_nulls;
    int64_t constant;
    unsigned op, expression;
};
static int comparisons(const struct qs_plan *p,struct qs_chunk *c,int index,
                       struct comparison *out,unsigned *count)
{
    if(index<0 || (unsigned)index>=p->nexprs) return 0;
    const struct qs_expr *e=&p->exprs[index];
    if(e->op==Q_AND)
        return comparisons(p,c,e->left,out,count) && comparisons(p,c,e->right,out,count);
    if(e->op<Q_EQ || e->op>Q_GE || e->left<0 || e->right<0 ||
        (unsigned)e->left>=p->nexprs || (unsigned)e->right>=p->nexprs) return 0;
    const struct qs_expr *column=&p->exprs[e->left],*other=&p->exprs[e->right];
    unsigned op=e->op;
    if(column->op==Q_CONST && other->op==Q_COLUMN) {
        const struct qs_expr *swap=column; column=other; other=swap;
        static const unsigned reversed[]={Q_EQ,Q_NE,Q_GT,Q_GE,Q_LT,Q_LE}; op=reversed[op-Q_EQ];
    }
    if(column->op!=Q_COLUMN ||
        column->column<0 || (unsigned)column->column>=p->nattrs || *count>=QS_EXPRS) return 0;
    const struct qs_column *left_col=&p->cols[column->column];
    if(left_col->type==Q_STRING) return 0;
    int slot=left_col->slot;
    if(slot<0 || (unsigned)slot>=c->ncols) return 0;
    size_t bitmap=(c->capacity+7)/8;
    struct comparison test={.values=qs_values(c)+(size_t)slot*c->capacity,
        .nulls=qs_nulls(c)+(size_t)slot*bitmap,.op=op,.expression=(unsigned)(column-p->exprs)};
    if(other->op==Q_COLUMN) {
        if(other->column<0 || (unsigned)other->column>=p->nattrs) return 0;
        const struct qs_column *right_col=&p->cols[other->column];
        int right_slot=right_col->slot;
        if((left_col->type!=Q_INTEGER && left_col->type!=Q_DATE) ||
           right_col->type!=left_col->type || column->type!=left_col->type ||
           other->type!=right_col->type || column->scale!=other->scale ||
           left_col->deferred || right_col->deferred ||
           right_slot<0 || (unsigned)right_slot>=c->ncols) return 0;
        test.right=qs_values(c)+(size_t)right_slot*c->capacity;
        test.right_nulls=qs_nulls(c)+(size_t)right_slot*bitmap;
        out[(*count)++]=test;
        return 1;
    }
    if(other->op!=Q_CONST || other->isnull) return 0;
    __int128 value=other->constant;
    if(column->scale>=other->scale) {
        if(qs_scale(value,column->scale-other->scale,&value)) return 0;
    } else {
        __int128 divisor; if(qs_scale(1,other->scale-column->scale,&divisor) || value%divisor) return 0;
        value/=divisor;
    }
    if(value<INT64_MIN || value>INT64_MAX) return 0;
    test.constant=(int64_t)value;
    out[(*count)++]=test;
    return 1;
}
static int matches(int64_t value,const struct comparison *p,unsigned row)
{
    if(p->right_nulls && (p->right_nulls[row/8]&(1U<<(row%8)))) return 0;
    int64_t right=p->right?p->right[row]:p->constant;
    switch(p->op) {
    case Q_EQ:return value==right; case Q_NE:return value!=right;
    case Q_LT:return value<right; case Q_LE:return value<=right;
    case Q_GT:return value>right; default:return value>=right;
    }
}
static int matches_row(const struct comparison *p,unsigned row)
{ return !(p->nulls[row/8]&(1U<<(row%8))) && matches(p->values[row],p,row); }
static void select_scalar(struct qs_chunk *c,const struct comparison *predicates,unsigned count,unsigned begin,int any)
{
    for(unsigned row=begin;row<c->rows;++row) {
        unsigned k=0;
        for(;k<count;++k) {
            const struct comparison *p=&predicates[k];
            if(matches_row(p,row)==any) break;
        }
        if(any?k<count:k==count) qs_selection(c)[c->selected++]=row;
    }
}
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static void select_avx2(struct qs_chunk *c,const struct comparison *predicates,unsigned count,int any)
{
    unsigned row=0;
    for(;row+4<=c->rows;row+=4) {
        unsigned mask=any?0:15;
        for(unsigned k=0;k<count && (any?mask!=15:mask!=0);++k) {
            const struct comparison *p=&predicates[k];
            __m256i values=_mm256_loadu_si256((const __m256i *)(p->values+row));
            __m256i right=p->right?_mm256_loadu_si256((const __m256i *)(p->right+row)):
                _mm256_set1_epi64x(p->constant),result;
            switch(p->op) {
            case Q_EQ:case Q_NE:result=_mm256_cmpeq_epi64(values,right);break;
            case Q_LT:case Q_GE:result=_mm256_cmpgt_epi64(right,values);break;
            default:result=_mm256_cmpgt_epi64(values,right);break;
            }
            unsigned selected=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(result));
            if(p->op==Q_NE || p->op==Q_GE || p->op==Q_LE) selected^=15;
            selected&=~(p->nulls[row/8]>>(row%8))&15;
            if(p->right_nulls) selected&=~(p->right_nulls[row/8]>>(row%8))&15;
            if(any) mask|=selected; else mask&=selected;
        }
        while(mask) { unsigned bit=__builtin_ctz(mask); qs_selection(c)[c->selected++]=row+bit; mask&=mask-1; }
    }
    select_scalar(c,predicates,count,row,any);
}
#endif
/* ScalarArrayOpExpr is already represented as OR comparisons. Recognize
 * that existing tree without adding an IN opcode or changing the plan ABI. */
static int disjunction(const struct qs_plan *p,struct qs_chunk *c,int id,
    struct comparison *out,unsigned *count)
{
    if(id<0 || (unsigned)id>=p->nexprs) return 0;
    const struct qs_expr *e=&p->exprs[id];
    if(e->op==Q_OR) return disjunction(p,c,e->left,out,count) && disjunction(p,c,e->right,out,count);
    unsigned before=*count;
    return comparisons(p,c,id,out,count) && *count==before+1;
}
struct text_literal { const char *bytes; unsigned length; };
static int text_equalities(const struct qs_plan *p,int id,const struct qs_dict *dictionary,
    const struct qs_column **column,struct text_literal *literals,unsigned *count)
{
    if(id<0 || (unsigned)id>=p->nexprs) return 0;
    const struct qs_expr *e=&p->exprs[id];
    if(e->op==Q_OR)
        return text_equalities(p,e->left,dictionary,column,literals,count) &&
            text_equalities(p,e->right,dictionary,column,literals,count);
    if(e->op!=Q_EQ || e->left<0 || e->right<0 || e->left>=id || e->right>=id ||
       *count>=QS_EXPRS) return 0;
    const struct qs_expr *key=&p->exprs[e->left],*value=&p->exprs[e->right];
    if(key->op==Q_CONST) { const struct qs_expr *swap=key; key=value; value=swap; }
    if(key->op!=Q_COLUMN || key->column<0 || (unsigned)key->column>=p->nattrs ||
       value->op!=Q_CONST || value->type!=Q_STRING || value->isnull) return 0;
    const struct qs_column *col=&p->cols[key->column];
    if(col->type!=Q_STRING || !col->deferred || (*column && *column!=col)) return 0;
    struct text_literal *literal=&literals[*count];
    if(qs_dict_get(dictionary,(unsigned)value->constant,&literal->bytes,&literal->length)) return 0;
    *column=col; ++*count; return 1;
}
/* A WHERE equality or IN-list can reject borrowed strings directly from
 * the active rows. The general vector evaluator only sees the survivors. */
static int select_text(const struct qs_plan *p,struct qs_chunk *c,int id,
    const struct qs_vector_input *input)
{
    struct text_literal literals[QS_EXPRS]; unsigned count=0;
    const struct qs_column *column=NULL;
    if(!input->raw || !input->dictionary ||
       !text_equalities(p,id,input->dictionary,&column,literals,&count) ||
       column->slot<0 || (unsigned)column->slot>=c->ncols) return 0;
    const int64_t *values=qs_values(c)+(size_t)column->slot*c->capacity;
    const unsigned char *nulls=qs_nulls(c)+(size_t)column->slot*((c->capacity+7)/8);
    unsigned rows=c->selected; c->selected=0;
    for(unsigned i=0;i<rows;++i) {
        unsigned row=qs_selection(c)[i];
        if(nulls[row/8]&(1U<<(row%8))) continue;
        struct qs_heap_ref ref; memcpy(&ref,&values[row],sizeof(ref));
        if(ref.offset>input->raw_bytes || ref.bytes>input->raw_bytes-ref.offset) return -2;
        const char *text=(const char *)input->raw+ref.offset; unsigned length=ref.bytes;
        for(unsigned k=0;k<count;++k) {
            const struct text_literal *literal=&literals[k];
            unsigned end=literal->length;
            if(end>length || (!column->trim_spaces && end!=length)) continue;
            if(end && (text[0]!=literal->bytes[0] || memcmp(text,literal->bytes,end))) continue;
            if(column->trim_spaces) {
                /* Check padding only for a candidate that matched the
                 * literal. Trimmed input cannot equal a space-ended literal. */
                if(end && literal->bytes[end-1]==' ') continue;
                while(end<length && text[end]==' ') ++end;
                if(end!=length) continue;
            }
            qs_selection(c)[c->selected++]=row; break;
        }
    }
    return 1;
}
/* Gather eager comparisons across the top-level conjunction so SIMD can
 * reject rows before loading borrowed strings or decoding Numeric values. */
static void eager_conjuncts(const struct qs_plan *p,struct qs_chunk *c,int id,
    struct comparison *predicates,unsigned *count)
{
    if(id<0 || (unsigned)id>=p->nexprs) return;
    const struct qs_expr *e=&p->exprs[id];
    if(e->op==Q_AND) {
        eager_conjuncts(p,c,e->left,predicates,count);
        eager_conjuncts(p,c,e->right,predicates,count);
        return;
    }
    unsigned before=*count;
    if(!comparisons(p,c,id,predicates,count) || *count!=before+1 ||
       p->cols[p->exprs[predicates[before].expression].column].deferred)
        *count=before;
}
/* UNKNOWN is rejected here only for a top-level WHERE conjunct. Report
 * whether all surviving rows already passed the entire predicate. */
static int prefilter(const struct qs_plan *p,struct qs_chunk *c,int id,struct comparison *predicates,
    const struct qs_vector_input *input)
{
    if(!c->selected) return 1;
    if(id<0 || (unsigned)id>=p->nexprs) return 0;
    const struct qs_expr *e=&p->exprs[id];
    if(e->op==Q_AND) {
        int left=prefilter(p,c,e->left,predicates,input);
        if(left<0) return left;
        int right=prefilter(p,c,e->right,predicates,input);
        return right<0?right:left && right;
    }
    unsigned count=0;
    int any=e->op==Q_OR;
    if(!(any?disjunction(p,c,id,predicates,&count):comparisons(p,c,id,predicates,&count)) || !count)
        return select_text(p,c,id,input);
    for(unsigned k=0;k<count;++k)
        if(p->cols[p->exprs[predicates[k].expression].column].deferred) return 0;
    if(!any) return 1; /* Evaluated by eager_conjuncts(). */
    unsigned rows=c->selected; c->selected=0;
    if(rows==c->rows) {
#if defined(__x86_64__) || defined(__i386__)
        if(__builtin_cpu_supports("avx2")) select_avx2(c,predicates,count,any);
        else
#endif
            select_scalar(c,predicates,count,0,any);
    } else for(unsigned i=0;i<rows;++i) {
        unsigned row=qs_selection(c)[i],k=0;
        for(;k<count;++k) {
            const struct comparison *t=&predicates[k];
            if(matches_row(t,row)==any) break;
        }
        if(any?k<count:k==count) qs_selection(c)[c->selected++]=row;
    }
    return 1;
}
/* Keep the SIMD prefix (e.g. date bounds), then pass its selection through
 * numeric predicates. Adjacent bounds on one column share a single decode;
 * no boolean vectors or decoded copies are needed for this simple AND path. */
static int select_deferred(const struct qs_plan *p,struct qs_chunk *c,const struct comparison *predicates,
    unsigned count,unsigned prefix,const void *raw,uint64_t bytes)
{
    if(!raw) return -1;
    if(prefix) {
#if defined(__x86_64__) || defined(__i386__)
        if(__builtin_cpu_supports("avx2")) select_avx2(c,predicates,prefix,0);
        else
#endif
            select_scalar(c,predicates,prefix,0,0);
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
            for(;j<ends[k];++j) if(!matches(value,&predicates[j],row)) break;
            if(j!=ends[k]) break;
        }
        if(k==count) qs_selection(c)[c->selected++]=row;
    }
    return 0;
}
static int64_t filter_columns(struct femu_csd_args *a,int preselected)
{
    if(!a || (a->numr<2 || a->numr>5) || a->mr_len[1]<(long long)sizeof(struct qs_plan)) return -1;
    if(a->numr>=3 && (a->mr_len[2]<0 || !a->mr_addr[2])) return -1;
    struct qs_chunk *c=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1];
    if(a->mr_len[0]<(long long)sizeof(*c) || qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0] ||
        c->ncols!=p->ncols || c->rows>c->capacity || p->nattrs>QS_COLS || p->nexprs>QS_EXPRS ||
        (preselected && c->selected>c->rows)) return -1;
    preselected=preselected && c->selected<c->rows;
    if(!preselected) c->selected=0;
    struct comparison predicates[QS_EXPRS]; unsigned count=0;
    if(!preselected && comparisons(p,c,p->predicate,predicates,&count) && count) {
        unsigned prefix=0;
        while(prefix<count && !p->cols[p->exprs[predicates[prefix].expression].column].deferred) ++prefix;
        if(prefix<count) {
            int status=select_deferred(p,c,predicates,count,prefix,a->numr>=3?a->mr_addr[2]:NULL,a->numr>=3?a->mr_len[2]:0);
            if(status) return status;
        } else {
#if defined(__x86_64__) || defined(__i386__)
            if(__builtin_cpu_supports("avx2")) select_avx2(c,predicates,count,0);
            else
#endif
                select_scalar(c,predicates,count,0,0);
        }
    } else {
        uint8_t needed[QS_EXPRS]={0};
        if(qs_vector_require(p,p->predicate,needed)) return -2;
        /* Do not reorder arithmetic or substring expressions that can throw. */
        int reorder=1;
        for(unsigned i=0;i<p->nexprs;++i) if(needed[i] &&
            (p->exprs[i].op==Q_ADD || p->exprs[i].op==Q_SUB || p->exprs[i].op==Q_MUL ||
             p->exprs[i].op==Q_DIV || p->exprs[i].op==Q_SUBSTRING)) reorder=0;
        struct qs_vector_input input={.raw=a->numr>=3?a->mr_addr[2]:NULL,
            .raw_bytes=a->numr>=3?a->mr_len[2]:0,.dictionary=a->numr>=4?a->mr_addr[3]:NULL,
            .parameters=a->numr==5?a->mr_addr[4]:NULL,
            .nparameters=a->numr==5 && a->mr_len[4]>0?(uint64_t)a->mr_len[4]/sizeof(struct qs_cell):0};
        count=0;
        if(reorder) eager_conjuncts(p,c,p->predicate,predicates,&count);
        if(count && preselected) {
            unsigned rows=c->selected; c->selected=0;
            for(unsigned i=0;i<rows;++i) {
                unsigned row=qs_selection(c)[i],k=0;
                while(k<count && matches_row(&predicates[k],row)) ++k;
                if(k==count) qs_selection(c)[c->selected++]=row;
            }
        } else if(count) {
#if defined(__x86_64__) || defined(__i386__)
            if(__builtin_cpu_supports("avx2")) select_avx2(c,predicates,count,0);
            else
#endif
                select_scalar(c,predicates,count,0,0);
        } else if(!preselected) {
            c->selected=c->rows;
            for(unsigned i=0;i<c->rows;++i) qs_selection(c)[i]=i;
        }
        if(reorder) {
            int status=prefilter(p,c,p->predicate,predicates,&input);
            if(status<0) return status;
            if(status) goto materialize;
        }
        unsigned candidates=c->selected;
        const uint32_t *selection=candidates<c->rows?qs_selection(c):NULL;
        c->selected=0;
        if(!candidates) return 0;
        size_t scratch=qs_filter_memory(p);
        if(p->work_mem_bytes && scratch>p->work_mem_bytes) return -4;
        cs_work_context *context=p->work_mem_parent?cs_work_begin_child((void *)(uintptr_t)p->work_mem_parent,scratch):cs_work_begin_limit(scratch); if(!context) return -4;
        struct qs_vector *vectors=cs_work_alloc(context,(p->predicate+1)*sizeof(*vectors));
        if(!vectors) { cs_work_end(context); return -4; }
        for(unsigned first=0;first<candidates;first+=QS_VECTOR_ROWS) {
            unsigned n=candidates-first; if(n>QS_VECTOR_ROWS) n=QS_VECTOR_ROWS;
            if(qs_vector_execute_input(p,c,selection,first,n,needed,vectors,input)) { cs_work_end(context); return -2; }
            const struct qs_vector *result=&vectors[p->predicate];
            for(unsigned i=0;i<n;++i) {
                if(result->errors[i]) { cs_work_end(context); return -2; }
                unsigned row=selection?selection[first+i]:first+i;
                if(!qs_vector_null(result,i) && qs_vector_value(result,i)) qs_selection(c)[c->selected++]=row;
            }
        }
        cs_work_end(context);
    }
materialize:
    /* The retained LOAD stays valid during filtering. Materialize values that
     * leave Scan; predicate-only references never escape this input block. */
    for(unsigned col=0;col<p->nattrs;++col) {
        const struct qs_column *spec=&p->cols[col];
        if(!spec->deferred || spec->deferred==QS_FILTER_ONLY) continue;
        if(a->numr<3 || (spec->type!=Q_DECIMAL && spec->type!=Q_STRING) || spec->slot<0 || (unsigned)spec->slot>=c->ncols || a->mr_len[2]<0) return -1;
        int64_t *values=qs_values(c)+(size_t)spec->slot*c->capacity;
        unsigned char *nulls=qs_nulls(c)+(size_t)spec->slot*((c->capacity+7)/8);
        unsigned char *scales=qs_dscales(c)+(size_t)spec->slot*c->capacity;
        for(unsigned i=0;i<c->selected;++i) {
            unsigned row=qs_selection(c)[i];
            if(nulls[row/8]&(1U<<(row%8))) continue;
            struct qs_heap_ref ref; memcpy(&ref,&values[row],sizeof(ref));
            if(ref.offset>(uint64_t)a->mr_len[2] || ref.bytes>(uint64_t)a->mr_len[2]-ref.offset) return -2;
            if(spec->type==Q_STRING) {
                if(a->numr<4 || !a->mr_addr[3]) return -1;
                const char *text=(const char *)a->mr_addr[2]+ref.offset; unsigned length=ref.bytes;
                if(spec->trim_spaces) while(length && text[length-1]==' ') --length;
                int id=qs_dict_insert(a->mr_addr[3],text,length); if(id<0) return -3;
                values[row]=id; scales[row]=0; continue;
            }
            unsigned dscale;
            if(qs_numeric_cents((const unsigned char *)a->mr_addr[2]+ref.offset,ref.bytes,&values[row],&dscale)) return -3;
            scales[row]=dscale;
        }
    }
    return 0;
}
int64_t pg_sina_filter(struct femu_csd_args *a)
{ return filter_columns(a,0); }
/* Internal callers can pass a selection made from an immutable join build. */
int64_t qs_filter_selected(struct femu_csd_args *a)
{ return filter_columns(a,1); }
