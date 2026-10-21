/* Small expression batches: reference columns/constants, materialize results.
 * Like DuckDB's reference/constant vectors and conjunction selection, children
 * receive only the rows still needed by their parent. Scratch stays in workmem. */
#ifndef QS_VECTOR_H
#define QS_VECTOR_H
#include "query_eval.h"
#include "heap_numeric.h"
#include "cs_workmem.h"
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#define QS_VECTOR_ROWS 256
enum qs_vector_kind { QS_WIDE, QS_NARROW, QS_REFERENCE, QS_CONSTANT };
struct qs_vector {
    union { __int128 values[QS_VECTOR_ROWS]; int64_t narrow[QS_VECTOR_ROWS]; };
    uint8_t nulls[QS_VECTOR_ROWS], dscales[QS_VECTOR_ROWS], errors[QS_VECTOR_ROWS];
    unsigned kind, done, all_valid;
    struct qs_cell constant;
    const int64_t *column;
    const uint8_t *column_nulls, *column_scales;
    const uint32_t *selection;
    unsigned first;
};
/* Filter scratch is temporary but can coexist with a retained sink state on
 * every slot. Keep its charged allocation (including allocator metadata) out
 * of the sink budget; both contexts together stay below that slot's share. */
static inline size_t qs_filter_memory(const struct qs_plan *p)
{ return p->predicate<0?0:((size_t)(p->predicate+1)*sizeof(struct qs_vector)+4095)/4096*4096+4096; }
static inline size_t qs_sink_memory(const struct qs_plan *p,unsigned workers)
{
    struct cs_work_stats stats;
    if(cs_work_get_stats(&stats) || workers>QS_WORKERS) return 0;
    size_t limit=stats.capacity_bytes/(workers?workers:1);
    if(p->work_mem_bytes && p->work_mem_bytes<limit) limit=p->work_mem_bytes;
    size_t scratch=qs_filter_memory(p);
    return limit>scratch?limit-scratch:0;
}
/* Bit 1 marks a dependency; bit 2 marks an explicitly requested result. A
 * shared child can also be a result, so keep both instead of inferring roots. */
static inline int qs_vector_dependency(const struct qs_plan *p,int index,uint8_t *needed)
{
    if(index<0 || (unsigned)index>=p->nexprs) return -1;
    if(needed[index]) return 0;
    const struct qs_expr *e=&p->exprs[index];
    if(e->op>Q_IS_NOT_NULL) return -1;
    if(e->op!=Q_COLUMN && e->op!=Q_CONST) {
        if(e->left<0 || e->left>=index || qs_vector_dependency(p,e->left,needed)) return -1;
        if(e->op!=Q_NOT && e->op!=Q_IS_NULL && e->op!=Q_IS_NOT_NULL &&
           (e->right<0 || e->right>=index || qs_vector_dependency(p,e->right,needed))) return -1;
    }
    needed[index]=1; return 0;
}
static inline int qs_vector_require(const struct qs_plan *p,int index,uint8_t *needed)
{
    if(qs_vector_dependency(p,index,needed)) return -1;
    needed[index]|=2; return 0;
}
static inline unsigned qs_vector_row(const struct qs_vector *v,unsigned i)
{ return v->selection?v->selection[v->first+i]:v->first+i; }
static inline __int128 qs_vector_value(const struct qs_vector *v,unsigned i)
{
    if(v->kind==QS_REFERENCE) return v->column[qs_vector_row(v,i)];
    if(v->kind==QS_CONSTANT) return v->constant.value;
    return v->kind==QS_NARROW?v->narrow[i]:v->values[i];
}
static inline unsigned qs_vector_null(const struct qs_vector *v,unsigned i)
{
    if(v->kind==QS_REFERENCE) { unsigned row=qs_vector_row(v,i); return (v->column_nulls[row/8]>>(row%8))&1; }
    return v->kind==QS_CONSTANT?v->constant.isnull:v->nulls[i];
}
static inline unsigned qs_vector_dscale(const struct qs_vector *v,unsigned i)
{ return v->kind==QS_REFERENCE?v->column_scales[qs_vector_row(v,i)]:v->kind==QS_CONSTANT?v->constant.dscale:v->dscales[i]; }
static inline struct qs_cell qs_vector_cell(const struct qs_plan *p,const struct qs_vector *v,unsigned index,unsigned row)
{
    return (struct qs_cell){.value=qs_vector_value(&v[index],row),.scale=p->exprs[index].scale,
        .dscale=qs_vector_dscale(&v[index],row),.isnull=qs_vector_null(&v[index],row)};
}
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static __m256i qs_vector_load(const struct qs_vector *v,unsigned i,int64_t constant)
{
    if(v->kind==QS_CONSTANT) return _mm256_set1_epi64x(constant);
    if(v->kind==QS_REFERENCE) {
        if(v->selection) return _mm256_i32gather_epi64((const long long *)v->column,
            _mm_loadu_si128((const __m128i *)(v->selection+v->first+i)),8);
        return _mm256_loadu_si256((const __m256i *)(v->column+v->first+i));
    }
    return _mm256_loadu_si256((const __m256i *)(v->narrow+i));
}
/* AVX2 has signed 32x32->64 multiplication. Use it only when all four input
 * pairs fit int32; add/sub check signed overflow. Otherwise recompute exactly
 * with int128. No wrapping result is ever exposed to an aggregate. */
__attribute__((target("avx2")))
static int qs_vector_simd(const struct qs_expr *e,const struct qs_expr *le,const struct qs_expr *re,
    const struct qs_vector *left,const struct qs_vector *right,struct qs_vector *out,unsigned count)
{
    if(!left->all_valid || !right->all_valid || left->kind==QS_WIDE || right->kind==QS_WIDE) return 0;
    int64_t lc=left->kind==QS_CONSTANT?(int64_t)left->constant.value:0;
    int64_t rc=right->kind==QS_CONSTANT?(int64_t)right->constant.value:0;
    if(e->op!=Q_MUL && le->scale!=re->scale) {
        unsigned scale=e->op==Q_ADD || e->op==Q_SUB?e->scale:le->scale>re->scale?le->scale:re->scale;
        if(le->scale!=scale) {
            if(left->kind!=QS_CONSTANT) return 0;
            __int128 value; if(qs_scale(left->constant.value,scale-le->scale,&value) || value<INT64_MIN || value>INT64_MAX) return 0;
            lc=(int64_t)value;
        }
        if(re->scale!=scale) {
            if(right->kind!=QS_CONSTANT) return 0;
            __int128 value; if(qs_scale(right->constant.value,scale-re->scale,&value) || value<INT64_MIN || value>INT64_MAX) return 0;
            rc=(int64_t)value;
        }
    } else if((e->op==Q_ADD || e->op==Q_SUB) && e->scale!=le->scale) return 0;
    unsigned i=0;
    for(;i+4<=count;i+=4) {
        __m256i l=qs_vector_load(left,i,lc),r=qs_vector_load(right,i,rc),value;
        if(e->op==Q_MUL) {
            __m256i l32=_mm256_or_si256(_mm256_srli_epi64(_mm256_slli_epi64(l,32),32),_mm256_slli_epi64(_mm256_srai_epi32(l,31),32));
            __m256i r32=_mm256_or_si256(_mm256_srli_epi64(_mm256_slli_epi64(r,32),32),_mm256_slli_epi64(_mm256_srai_epi32(r,31),32));
            if(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_and_si256(_mm256_cmpeq_epi64(l,l32),_mm256_cmpeq_epi64(r,r32))))!=15) return 0;
            value=_mm256_mul_epi32(l,r);
        } else if(e->op==Q_ADD || e->op==Q_SUB) {
            value=e->op==Q_ADD?_mm256_add_epi64(l,r):_mm256_sub_epi64(l,r);
            __m256i overflow=_mm256_and_si256(_mm256_xor_si256(l,value),_mm256_xor_si256(e->op==Q_ADD?r:l,e->op==Q_ADD?value:r));
            if(_mm256_movemask_pd(_mm256_castsi256_pd(overflow))) return 0;
        } else {
            switch(e->op) {
            case Q_EQ:case Q_NE:value=_mm256_cmpeq_epi64(l,r);break;
            case Q_LT:case Q_GE:value=_mm256_cmpgt_epi64(r,l);break;
            default:value=_mm256_cmpgt_epi64(l,r);break;
            }
            if(e->op==Q_NE || e->op==Q_GE || e->op==Q_LE) value=_mm256_xor_si256(value,_mm256_set1_epi64x(-1));
            value=_mm256_and_si256(value,_mm256_set1_epi64x(1));
        }
        _mm256_storeu_si256((__m256i *)(out->narrow+i),value);
    }
    for(;i<count;++i) {
        __int128 l=left->kind==QS_CONSTANT?lc:qs_vector_value(left,i);
        __int128 r=right->kind==QS_CONSTANT?rc:qs_vector_value(right,i),v;
        if(e->op==Q_MUL) v=l*r;
        else if(e->op==Q_ADD) v=l+r;
        else if(e->op==Q_SUB) v=l-r;
        else { int cmp=(l>r)-(l<r); v=e->op==Q_EQ?cmp==0:e->op==Q_NE?cmp!=0:e->op==Q_LT?cmp<0:e->op==Q_LE?cmp<=0:e->op==Q_GT?cmp>0:cmp>=0; }
        if(v<INT64_MIN || v>INT64_MAX) return 0;
        out->narrow[i]=(int64_t)v;
    }
    for(i=0;i<count;++i) {
        unsigned l=qs_vector_dscale(left,i),r=qs_vector_dscale(right,i);
        out->dscales[i]=e->op==Q_MUL?l+r:e->op==Q_ADD || e->op==Q_SUB?(l>r?l:r):e->dscale;
    }
    memset(out->errors,0,count); memset(out->nulls,0,count);
    out->kind=QS_NARROW; out->done=count; out->all_valid=1; return 1;
}
#endif
struct qs_vector_input {
    const struct qs_plan *plan;
    struct qs_vector *vectors;
    unsigned count;
    const unsigned char *raw;
    uint64_t raw_bytes;
};
/* errors==2 means this row has not been evaluated in this batch. It also
 * caches shared expressions reached through different AND/OR branches. */
static int qs_vector_evaluate(struct qs_vector_input *input,unsigned index,const uint16_t *active,unsigned count)
{
    const struct qs_plan *p=input->plan; const struct qs_expr *e=&p->exprs[index];
    struct qs_vector *out=&input->vectors[index];
    if(out->done==input->count) return 0;
    uint16_t pending[QS_VECTOR_ROWS],rhs[QS_VECTOR_ROWS];
    const uint16_t *rows=active; unsigned n=count,nrhs=0;
    if(out->done || count!=input->count) {
        n=0;
        for(unsigned j=0;j<count;++j) if(out->errors[active[j]]==2) pending[n++]=active[j];
        rows=pending;
    }
    if(!n) return 0;
    if(e->op==Q_COLUMN) {
        /* Deferred numeric references are decoded only when an active branch
         * first requests the row. The raw AFDM values remain references until
         * Filter materializes the final selection for downstream operators. */
        const struct qs_column *col=&p->cols[e->column];
        if(!input->raw || col->type!=Q_DECIMAL) return -1;
        for(unsigned j=0;j<n;++j) {
            unsigned i=rows[j],row=qs_vector_row(out,i),scale=0;
            out->errors[i]=0; out->nulls[i]=(out->column_nulls[row/8]>>(row%8))&1;
            out->narrow[i]=0;
            if(!out->nulls[i]) {
                struct qs_heap_ref ref; memcpy(&ref,&out->column[row],sizeof(ref));
                if(ref.offset>input->raw_bytes || ref.bytes>input->raw_bytes-ref.offset) return -1;
                out->errors[i]=qs_numeric_cents(input->raw+ref.offset,ref.bytes,&out->narrow[i],&scale)!=0;
            }
            out->dscales[i]=scale;
            out->all_valid&=!out->errors[i] && !out->nulls[i];
        }
        out->done+=n; return 0;
    }
    if(qs_vector_evaluate(input,e->left,rows,n)) return -1;
    const struct qs_vector *left=&input->vectors[e->left],*right=NULL;
    if(e->op!=Q_NOT && e->op!=Q_IS_NULL && e->op!=Q_IS_NOT_NULL) {
        const uint16_t *right_rows=rows; nrhs=n;
        if(e->op==Q_AND || e->op==Q_OR) {
            nrhs=0;
            for(unsigned j=0;j<n;++j) {
                unsigned i=rows[j];
                if(left->errors[i]) continue;
                if(!qs_vector_null(left,i) && (e->op==Q_AND?!qs_vector_value(left,i):!!qs_vector_value(left,i))) continue;
                rhs[nrhs++]=i;
            }
            right_rows=rhs;
        }
        if(qs_vector_evaluate(input,e->right,right_rows,nrhs)) return -1;
        right=&input->vectors[e->right];
#if defined(__x86_64__) || defined(__i386__)
        if(n==input->count && nrhs==n && !out->done &&
            e->op>=Q_ADD && e->op<=Q_GE && __builtin_cpu_supports("avx2") &&
            qs_vector_simd(e,&p->exprs[e->left],&p->exprs[e->right],left,right,out,n)) return 0;
#endif
    }
    for(unsigned j=0;j<n;++j) {
        unsigned i=rows[j]; struct qs_cell a=qs_vector_cell(p,input->vectors,e->left,i),b={0};
        struct qs_cell v={.scale=e->scale,.dscale=e->dscale}; unsigned error=left->errors[i];
        if(error) { /* Preserve the error without reading an unevaluated RHS. */ }
        else if((e->op==Q_AND || e->op==Q_OR) && !a.isnull && (e->op==Q_AND?!a.value:!!a.value)) v=a;
        else if(e->op==Q_NOT) { v=a; v.value=!a.value; }
        else if(e->op==Q_IS_NULL || e->op==Q_IS_NOT_NULL) v.value=e->op==Q_IS_NULL?a.isnull:!a.isnull;
        else {
            b=qs_vector_cell(p,input->vectors,e->right,i); error=right->errors[i];
            v.isnull=a.isnull || b.isnull;
            if(e->op==Q_AND || e->op==Q_OR) {
                v.value=e->op==Q_AND?(a.value && b.value):(a.value || b.value);
                if(e->op==Q_AND && !b.isnull && !b.value) v.isnull=0;
                if(e->op==Q_OR && !b.isnull && b.value) v.isnull=0;
            } else if(!v.isnull && !error) {
                if(e->op==Q_MUL) { error=__builtin_mul_overflow(a.value,b.value,&v.value); v.dscale=a.dscale+b.dscale; }
                else if(e->op==Q_ADD || e->op==Q_SUB) {
                    __int128 l,r;
                    error=qs_scale(a.value,e->scale-a.scale,&l) || qs_scale(b.value,e->scale-b.scale,&r);
                    if(!error) error=e->op==Q_ADD?__builtin_add_overflow(l,r,&v.value):__builtin_sub_overflow(l,r,&v.value);
                    v.dscale=a.dscale>b.dscale?a.dscale:b.dscale;
                } else {
                    int cmp=0; error=qs_compare(a,b,&cmp)!=0;
                    v.value=e->op==Q_EQ?cmp==0:e->op==Q_NE?cmp!=0:e->op==Q_LT?cmp<0:e->op==Q_LE?cmp<=0:e->op==Q_GT?cmp>0:cmp>=0;
                }
            }
        }
        out->values[i]=v.value; out->nulls[i]=v.isnull; out->dscales[i]=v.dscale; out->errors[i]=error;
        out->all_valid&=!error && !v.isnull;
    }
    out->done+=n; return 0;
}
static inline int qs_vector_execute_raw(const struct qs_plan *p,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,unsigned count,const uint8_t *needed,struct qs_vector *vectors,
    const void *raw,uint64_t raw_bytes)
{
    if(count>QS_VECTOR_ROWS) return -1;
    unsigned low=chunk->rows,high=0;
    for(unsigned i=0;i<count;++i) {
        unsigned row=selection?selection[first+i]:first+i;
        if(row>=chunk->rows) return -1;
        if(row<low) low=row;
        if(row>high) high=row;
    }
    for(unsigned index=0;index<p->nexprs;++index) {
        if(!needed[index]) continue;
        const struct qs_expr *e=&p->exprs[index]; struct qs_vector *out=&vectors[index];
        out->kind=QS_WIDE; out->done=0; out->all_valid=1;
        if(e->op==Q_COLUMN) {
            if(e->column<0 || (unsigned)e->column>=p->nattrs) return -1;
            const struct qs_column *col=&p->cols[e->column]; int slot=col->slot;
            if(slot<0 || (unsigned)slot>=chunk->ncols) return -1;
            out->column=qs_values(chunk)+(size_t)slot*chunk->capacity;
            out->column_nulls=qs_nulls(chunk)+(size_t)slot*((chunk->capacity+7)/8);
            out->column_scales=qs_dscales(chunk)+(size_t)slot*chunk->capacity;
            out->selection=selection; out->first=first;
            out->kind=raw && col->deferred?QS_NARROW:QS_REFERENCE;
            /* Check bounds once per selection above, not once per column.
             * Nearby rows usually share a zero NULL bitmap; inspecting its
             * bytes is cheaper than extracting one bit for every row. Extra
             * bits may disable SIMD conservatively, never hide a NULL. */
            if(count && high-low<count*8) {
                for(unsigned b=low/8;b<=high/8;++b) if(out->column_nulls[b]) { out->all_valid=0; break; }
            } else for(unsigned i=0;i<count;++i) {
                unsigned row=qs_vector_row(out,i);
                out->all_valid&=!((out->column_nulls[row/8]>>(row%8))&1);
            }
            if(out->kind==QS_REFERENCE) out->done=count;
        } else if(e->op==Q_CONST) {
            out->kind=QS_CONSTANT; out->constant=(struct qs_cell){.value=e->constant,.isnull=e->isnull,.dscale=e->dscale};
            out->done=count; out->all_valid=!e->isnull;
        }
        memset(out->errors,out->done==count?0:2,count);
    }
    uint16_t rows[QS_VECTOR_ROWS]; for(unsigned i=0;i<count;++i) rows[i]=i;
    struct qs_vector_input input={p,vectors,count,raw,raw_bytes};
    for(unsigned i=0;i<p->nexprs;++i) if((needed[i]&2) && qs_vector_evaluate(&input,i,rows,count)) return -1;
    return 0;
}
static inline int qs_vector_execute(const struct qs_plan *p,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,unsigned count,const uint8_t *needed,struct qs_vector *vectors)
{ return qs_vector_execute_raw(p,chunk,selection,first,count,needed,vectors,NULL,0); }
#endif
