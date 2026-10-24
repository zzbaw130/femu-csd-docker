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
enum qs_vector_kind { QS_WIDE, QS_NARROW, QS_REFERENCE, QS_CONSTANT, QS_TEXT, QS_CELLS, QS_DYNAMIC };
struct qs_vector {
    union {
        __int128 values[QS_VECTOR_ROWS]; int64_t narrow[QS_VECTOR_ROWS];
        struct { const char *data; unsigned length; } strings[QS_VECTOR_ROWS];
    };
    uint8_t nulls[QS_VECTOR_ROWS], dscales[QS_VECTOR_ROWS], errors[QS_VECTOR_ROWS], scales[QS_VECTOR_ROWS];
    unsigned kind, done, all_valid;
    struct qs_cell constant;
    const struct qs_cell *cells;
    const int64_t *column;
    const uint8_t *column_nulls, *column_scales;
    const uint32_t *selection;
    unsigned first;
};
/* Filter scratch is temporary but can coexist with a retained sink state on
 * every slot. Keep its charged allocation (including allocator metadata) out
 * of the sink budget; both contexts together stay below that slot's share. */
static inline size_t qs_filter_memory(const struct qs_plan *p)
{
    int last=p->predicate+1;
    if(p->join.residual>last) last=p->join.residual;
    return !last?0:((size_t)last*sizeof(struct qs_vector)+4095)/4096*4096+4096;
}
static inline size_t qs_sink_memory(const struct qs_plan *p,unsigned workers)
{
    struct cs_work_stats stats;
    if(cs_work_get_stats(&stats) || workers>QS_WORKERS) return 0;
    size_t limit=stats.capacity_bytes/(workers?workers:1);
    if(p->join.relid) {
        size_t build=p->join.work_mem_bytes;
        if(!build || build>stats.capacity_bytes/2) build=stats.capacity_bytes/2;
        limit=(stats.capacity_bytes-build)/(workers?workers:1);
    }
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
    if(e->op>Q_CASE && e->op!=Q_YEAR && e->op!=Q_LIKE && e->op!=Q_SUBSTRING && e->op!=Q_PARAMETER) return -1; /* AGGREF/DIV are scalar final expressions. */
    if(e->op!=Q_COLUMN && e->op!=Q_CONST && e->op!=Q_PARAMETER) {
        if(e->left<0 || e->left>=index || qs_vector_dependency(p,e->left,needed)) return -1;
        if(e->op!=Q_NOT && e->op!=Q_IS_NULL && e->op!=Q_IS_NOT_NULL && e->op!=Q_YEAR &&
           (e->right<0 || e->right>=index || qs_vector_dependency(p,e->right,needed))) return -1;
        if((e->op==Q_CASE || e->op==Q_SUBSTRING) && (e->otherwise<0 || e->otherwise>=index ||
            qs_vector_dependency(p,e->otherwise,needed))) return -1;
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
    if(v->kind==QS_CELLS) return v->cells[i].value;
    if(v->kind==QS_TEXT) return 0; /* Text stays a byte view until materialized. */
    if(v->kind==QS_REFERENCE) return v->column[qs_vector_row(v,i)];
    if(v->kind==QS_CONSTANT) return v->constant.value;
    return v->kind==QS_NARROW?v->narrow[i]:v->values[i];
}
static inline unsigned qs_vector_null(const struct qs_vector *v,unsigned i)
{
    if(v->kind==QS_CELLS) return v->cells[i].isnull;
    if(v->kind==QS_REFERENCE) { unsigned row=qs_vector_row(v,i); return (v->column_nulls[row/8]>>(row%8))&1; }
    return v->kind==QS_CONSTANT?v->constant.isnull:v->nulls[i];
}
static inline unsigned qs_vector_dscale(const struct qs_vector *v,unsigned i)
{ return v->kind==QS_CELLS?v->cells[i].dscale:v->kind==QS_REFERENCE?v->column_scales[qs_vector_row(v,i)]:v->kind==QS_CONSTANT?v->constant.dscale:v->dscales[i]; }
static inline struct qs_cell qs_vector_cell(const struct qs_plan *p,const struct qs_vector *v,unsigned index,unsigned row)
{
    if(v[index].kind==QS_CELLS) return v[index].cells[row];
    if(v[index].kind==QS_CONSTANT) return v[index].constant;
    return (struct qs_cell){.value=qs_vector_value(&v[index],row),.scale=v[index].kind==QS_DYNAMIC?v[index].scales[row]:p->exprs[index].scale,
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
    if(!left->all_valid || !right->all_valid || left->kind==QS_WIDE || right->kind==QS_WIDE ||
        left->kind==QS_CELLS || right->kind==QS_CELLS || left->kind==QS_DYNAMIC || right->kind==QS_DYNAMIC) return 0;
    if((left->kind==QS_CONSTANT && (left->constant.value<INT64_MIN || left->constant.value>INT64_MAX)) ||
       (right->kind==QS_CONSTANT && (right->constant.value<INT64_MIN || right->constant.value>INT64_MAX))) return 0;
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
    struct qs_dict *dictionary;
    const struct qs_cell *parameters;
    unsigned nparameters;
    const struct qs_cell *cells; /* Materialized columns, QS_VECTOR_ROWS cells per slot. */
};
/* The byte view borrows the retained LOAD buffer or immutable dictionary.
 * SUBSTRING counts UTF-8 characters using PG's positive/zero/negative-start
 * convention; unlike DuckDB, negative lengths remain an error. */
static inline unsigned qs_text_character(const char *text,unsigned bytes)
{
    if(!bytes) return 0;
    unsigned char c=*text; unsigned n=c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:4;
    return n<=bytes?n:bytes;
}
static int qs_vector_text(const struct qs_vector_input *input,unsigned index,unsigned row,const char **data,unsigned *bytes)
{
    const struct qs_vector *v=&input->vectors[index];
    if(v->kind==QS_TEXT) { *data=v->strings[row].data; *bytes=v->strings[row].length; return 0; }
    return qs_dict_get(input->dictionary,(unsigned)qs_vector_value(v,row),data,bytes);
}
static inline int qs_vector_materialize_text(struct qs_vector *v,unsigned count,struct qs_dict *dictionary)
{
    if(v->kind!=QS_TEXT) return 0;
    if(!dictionary) return -1;
    for(unsigned i=0;i<count;++i) {
        int id=0;
        if(!v->nulls[i] && !v->errors[i]) {
            id=qs_dict_insert(dictionary,v->strings[i].data,v->strings[i].length); if(id<0) return -1;
        }
        v->values[i]=id;
    }
    v->kind=QS_WIDE; return 0;
}
static int qs_text_like(const char *pattern,unsigned pn,const char *text,unsigned tn)
{
    if(!pn) return !tn;
    if(!tn) { while(pn && *pattern=='%') { ++pattern; --pn; } return !pn; }
    if(*pattern=='%') {
        while(pn && *pattern=='%') { ++pattern; --pn; }
        if(!pn) return 1;
        for(unsigned i=0;;) {
            int result=qs_text_like(pattern,pn,text+i,tn-i); if(result) return result;
            if(i==tn) break;
            i+=qs_text_character(text+i,tn-i);
        }
        return 0;
    }
    if(*pattern=='\\') {
        if(pn==1) return -1;
        ++pattern; --pn;
        return *pattern==*text?qs_text_like(pattern+1,pn-1,text+1,tn-1):0;
    }
    if(*pattern=='_') { unsigned n=qs_text_character(text,tn); return qs_text_like(pattern+1,pn-1,text+n,tn-n); }
    return *pattern==*text?qs_text_like(pattern+1,pn-1,text+1,tn-1):0;
}
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
        if(!input->raw || (col->type!=Q_DECIMAL && col->type!=Q_STRING)) return -1;
        for(unsigned j=0;j<n;++j) {
            unsigned i=rows[j],row=qs_vector_row(out,i),scale=0;
            out->errors[i]=0; out->nulls[i]=(out->column_nulls[row/8]>>(row%8))&1;
            if(col->type==Q_STRING) { out->strings[i].data=NULL; out->strings[i].length=0; }
            else out->narrow[i]=0;
            if(!out->nulls[i]) {
                struct qs_heap_ref ref; memcpy(&ref,&out->column[row],sizeof(ref));
                if(ref.offset>input->raw_bytes || ref.bytes>input->raw_bytes-ref.offset) return -1;
                if(col->type==Q_STRING) {
                    const char *text=(const char *)input->raw+ref.offset; unsigned length=ref.bytes;
                    if(col->trim_spaces) while(length && text[length-1]==' ') --length;
                    out->strings[i].data=text; out->strings[i].length=length;
                } else out->errors[i]=qs_numeric_cents(input->raw+ref.offset,ref.bytes,&out->narrow[i],&scale)!=0;
            }
            out->dscales[i]=scale;
            out->all_valid&=!out->errors[i] && !out->nulls[i];
        }
        if(col->type==Q_STRING) out->kind=QS_TEXT;
        out->done+=n; return 0;
    }
    if(qs_vector_evaluate(input,e->left,rows,n)) return -1;
    const struct qs_vector *left=&input->vectors[e->left],*right=NULL;
    if(e->op==Q_YEAR) {
        for(unsigned j=0;j<n;++j) {
            unsigned i=rows[j]; int error=left->errors[i]; struct qs_cell a=qs_vector_cell(p,input->vectors,e->left,i);
            out->errors[i]=error; out->nulls[i]=a.isnull; out->dscales[i]=0;
            out->values[i]=!error && !a.isnull ? qs_pgdate_year((int32_t)a.value) : 0;
            out->all_valid&=!error && !a.isnull;
        }
        out->done+=n; return 0;
    }
    if(e->op==Q_CASE) {
        int text=e->type==Q_STRING && input->dictionary;
        uint16_t false_rows[QS_VECTOR_ROWS]; unsigned nt=0,nf=0;
        for(unsigned j=0;j<n;++j) {
            unsigned i=rows[j]; if(left->errors[i]) continue;
            if(!qs_vector_null(left,i) && qs_vector_value(left,i)) rhs[nt++]=i;
            else false_rows[nf++]=i;
        }
        if(qs_vector_evaluate(input,e->right,rhs,nt) ||
            qs_vector_evaluate(input,e->otherwise,false_rows,nf)) return -1;
        if(!text && (input->vectors[e->right].kind==QS_CELLS || input->vectors[e->right].kind==QS_DYNAMIC ||
            input->vectors[e->otherwise].kind==QS_CELLS || input->vectors[e->otherwise].kind==QS_DYNAMIC)) out->kind=QS_DYNAMIC;
        for(unsigned j=0;j<n;++j) {
            unsigned i=rows[j]; unsigned error=left->errors[i]; struct qs_cell v={0};
            if(!error) {
                unsigned branch=!qs_vector_null(left,i) && qs_vector_value(left,i)?e->right:e->otherwise;
                v=qs_vector_cell(p,input->vectors,branch,i); error=input->vectors[branch].errors[i];
                if(text && !v.isnull && !error) {
                    error=qs_vector_text(input,branch,i,&out->strings[i].data,&out->strings[i].length)!=0;
                } else if(!text && out->kind!=QS_DYNAMIC && !v.isnull) error|=v.scale>e->scale || qs_scale(v.value,e->scale-v.scale,&v.value);
            }
            if(!text) {
                if(out->kind!=QS_DYNAMIC) v.scale=e->scale;
                out->values[i]=v.value; out->scales[i]=v.scale;
            }
            out->nulls[i]=v.isnull; out->dscales[i]=v.dscale; out->errors[i]=error;
            out->all_valid&=!error && !v.isnull;
        }
        if(text) out->kind=QS_TEXT;
        out->done+=n; return 0;
    }
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
            p->exprs[e->left].type!=Q_STRING &&
            e->op>=Q_ADD && e->op<=Q_GE && __builtin_cpu_supports("avx2") &&
            qs_vector_simd(e,&p->exprs[e->left],&p->exprs[e->right],left,right,out,n)) return 0;
#endif
    }
    if(e->op==Q_SUBSTRING) {
        if(qs_vector_evaluate(input,e->otherwise,rows,n)) return -1;
        const struct qs_vector *lengths=&input->vectors[e->otherwise];
        for(unsigned j=0;j<n;++j) {
            unsigned i=rows[j]; unsigned error=left->errors[i] || right->errors[i] || lengths->errors[i];
            int isnull=qs_vector_null(left,i) || qs_vector_null(right,i) || qs_vector_null(lengths,i);
            if(!error && !isnull) {
                __int128 begin=qs_vector_value(right,i),length=qs_vector_value(lengths,i);
                if(begin<INT_MIN || begin>INT_MAX || length<0 || length>INT_MAX) error=1;
                else {
                    const char *text; unsigned bytes;
                    error=qs_vector_text(input,e->left,i,&text,&bytes)!=0;
                    if(!error) {
                        int64_t start=(int64_t)begin-1,end=start+(int64_t)length,character=0;
                        if(start<0) start=0;
                        if(end<start) end=start;
                        unsigned first=0,last;
                        while(first<bytes && character<start) { first+=qs_text_character(text+first,bytes-first); ++character; }
                        last=first;
                        while(last<bytes && character<end) { last+=qs_text_character(text+last,bytes-last); ++character; }
                        out->strings[i].data=text+first; out->strings[i].length=last-first;
                    }
                }
            }
            out->nulls[i]=isnull; out->errors[i]=error; out->dscales[i]=0;
            out->all_valid&=!error && !isnull;
        }
        out->kind=QS_TEXT; out->done+=n; return 0;
    }
    if((e->op==Q_ADD || e->op==Q_SUB || e->op==Q_MUL) &&
        (left->kind==QS_CELLS || left->kind==QS_DYNAMIC || right->kind==QS_CELLS || right->kind==QS_DYNAMIC)) out->kind=QS_DYNAMIC;
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
                if(e->op==Q_LIKE || (e->op>=Q_EQ && e->op<=Q_GE && p->exprs[e->left].type==Q_STRING &&
                    (input->dictionary || left->kind==QS_TEXT || right->kind==QS_TEXT))) {
                    const char *a_text,*b_text; unsigned an,bn;
                    error=qs_vector_text(input,e->left,i,&a_text,&an) || qs_vector_text(input,e->right,i,&b_text,&bn);
                    if(!error) {
                        if(e->op==Q_LIKE) { int match=qs_text_like(b_text,bn,a_text,an); error=match<0; v.value=match>0; }
                        else {
                            int cmp=memcmp(a_text,b_text,an<bn?an:bn); if(!cmp) cmp=(an>bn)-(an<bn);
                            v.value=e->op==Q_EQ?cmp==0:e->op==Q_NE?cmp!=0:e->op==Q_LT?cmp<0:e->op==Q_LE?cmp<=0:e->op==Q_GT?cmp>0:cmp>=0;
                        }
                    }
                } else if(e->op==Q_MUL) { error=__builtin_mul_overflow(a.value,b.value,&v.value); v.scale=a.scale+b.scale; v.dscale=a.dscale+b.dscale; }
                else if(e->op==Q_ADD || e->op==Q_SUB) {
                    __int128 l,r;
                    v.scale=out->kind==QS_DYNAMIC?(a.scale>b.scale?a.scale:b.scale):e->scale;
                    error=qs_scale(a.value,v.scale-a.scale,&l) || qs_scale(b.value,v.scale-b.scale,&r);
                    if(!error) error=e->op==Q_ADD?__builtin_add_overflow(l,r,&v.value):__builtin_sub_overflow(l,r,&v.value);
                    v.dscale=a.dscale>b.dscale?a.dscale:b.dscale;
                } else {
                    int cmp=0; error=qs_compare(a,b,&cmp)!=0;
                    v.value=e->op==Q_EQ?cmp==0:e->op==Q_NE?cmp!=0:e->op==Q_LT?cmp<0:e->op==Q_LE?cmp<=0:e->op==Q_GT?cmp>0:cmp>=0;
                }
            }
        }
        out->values[i]=v.value; out->scales[i]=v.scale; out->nulls[i]=v.isnull; out->dscales[i]=v.dscale; out->errors[i]=error;
        out->all_valid&=!error && !v.isnull;
    }
    out->done+=n; return 0;
}
static inline int qs_vector_execute_input(const struct qs_plan *p,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,unsigned count,const uint8_t *needed,struct qs_vector *vectors,
    struct qs_vector_input input)
{
    if(count>QS_VECTOR_ROWS) return -1;
    int column_input=chunk==NULL;
    if(column_input && ((p->ncols && !input.cells) || selection || first+count>QS_VECTOR_ROWS)) return -1;
    unsigned low=column_input?0:chunk->rows,high=0;
    for(unsigned i=0;!column_input && i<count;++i) {
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
            if(column_input) {
                if(slot<0 || (unsigned)slot>=p->ncols) return -1;
                out->cells=input.cells+(size_t)slot*QS_VECTOR_ROWS+first;
                out->kind=QS_NARROW; out->done=count;
                for(unsigned i=0;i<count;++i) {
                    struct qs_cell v=out->cells[i];
                    out->narrow[i]=v.isnull?0:(int64_t)v.value; out->nulls[i]=v.isnull; out->dscales[i]=v.dscale;
                    out->all_valid&=!v.isnull;
                    if(!v.isnull && (v.scale!=e->scale || v.value<INT64_MIN || v.value>INT64_MAX)) out->kind=QS_CELLS;
                }
                memset(out->errors,0,count); continue;
            }
            if(slot<0 || (unsigned)slot>=chunk->ncols) return -1;
            out->column=qs_values(chunk)+(size_t)slot*chunk->capacity;
            out->column_nulls=qs_nulls(chunk)+(size_t)slot*((chunk->capacity+7)/8);
            out->column_scales=qs_dscales(chunk)+(size_t)slot*chunk->capacity;
            out->selection=selection; out->first=first;
            out->kind=input.raw && col->deferred?QS_NARROW:QS_REFERENCE;
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
            out->kind=QS_CONSTANT; out->constant=(struct qs_cell){.value=e->constant,.scale=e->scale,.isnull=e->isnull,.dscale=e->dscale};
            out->done=count; out->all_valid=!e->isnull;
        } else if(e->op==Q_PARAMETER) {
            if(!input.parameters || e->column<0 || (unsigned)e->column>=input.nparameters) return -1;
            out->kind=QS_CONSTANT; out->constant=input.parameters[e->column];
            out->done=count; out->all_valid=!out->constant.isnull;
        }
        memset(out->errors,out->done==count?0:2,count);
    }
    uint16_t rows[QS_VECTOR_ROWS]; for(unsigned i=0;i<count;++i) rows[i]=i;
    input.plan=p; input.vectors=vectors; input.count=count;
    for(unsigned i=0;i<p->nexprs;++i) if((needed[i]&2) && qs_vector_evaluate(&input,i,rows,count)) return -1;
    return 0;
}
static inline int qs_vector_execute_raw_dict(const struct qs_plan *p,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,unsigned count,const uint8_t *needed,struct qs_vector *vectors,
    const void *raw,uint64_t raw_bytes,struct qs_dict *dictionary)
{
    struct qs_vector_input input={.raw=raw,.raw_bytes=raw_bytes,.dictionary=dictionary};
    return qs_vector_execute_input(p,chunk,selection,first,count,needed,vectors,input);
}
static inline int qs_vector_execute_raw(const struct qs_plan *p,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,unsigned count,const uint8_t *needed,struct qs_vector *vectors,
    const void *raw,uint64_t raw_bytes)
{ return qs_vector_execute_raw_dict(p,chunk,selection,first,count,needed,vectors,raw,raw_bytes,NULL); }
static inline int qs_vector_execute(const struct qs_plan *p,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,unsigned count,const uint8_t *needed,struct qs_vector *vectors)
{ return qs_vector_execute_raw(p,chunk,selection,first,count,needed,vectors,NULL,0); }
#endif
