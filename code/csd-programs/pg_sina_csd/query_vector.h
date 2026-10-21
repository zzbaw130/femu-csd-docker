/* Bounded expression vectors shared by Filter and Aggregate. The caller owns
 * these arrays in workmem; no AFDM layout, device API or scheduling changes. */
#ifndef QS_VECTOR_H
#define QS_VECTOR_H
#include "query_eval.h"
#define QS_VECTOR_ROWS 256
struct qs_vector {
    __int128 values[QS_VECTOR_ROWS];
    uint8_t nulls[QS_VECTOR_ROWS], dscales[QS_VECTOR_ROWS], errors[QS_VECTOR_ROWS];
};
/* PG builds children before parents. Mark only expressions consumed by this
 * operator, then execute them in that existing order once per small batch. */
static inline int qs_vector_require(const struct qs_plan *p,int index,uint8_t *needed)
{
    if(index<0 || (unsigned)index>=p->nexprs) return -1;
    if(needed[index]) return 0;
    const struct qs_expr *e=&p->exprs[index];
    if(e->op>Q_IS_NOT_NULL) return -1;
    if(e->op!=Q_COLUMN && e->op!=Q_CONST) {
        if(e->left<0 || e->left>=index || qs_vector_require(p,e->left,needed)) return -1;
        if(e->op!=Q_NOT && e->op!=Q_IS_NULL && e->op!=Q_IS_NOT_NULL &&
           (e->right<0 || e->right>=index || qs_vector_require(p,e->right,needed))) return -1;
    }
    needed[index]=1; return 0;
}
static inline struct qs_cell qs_vector_cell(const struct qs_plan *p,const struct qs_vector *v,unsigned index,unsigned row)
{
    return (struct qs_cell){.value=v[index].values[row],.scale=p->exprs[index].scale,
        .dscale=v[index].dscales[row],.isnull=v[index].nulls[row]};
}
static inline int qs_vector_execute(const struct qs_plan *p,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,unsigned count,const uint8_t *needed,struct qs_vector *vectors)
{
    if(count>QS_VECTOR_ROWS) return -1;
    for(unsigned index=0;index<p->nexprs;++index) {
        if(!needed[index]) continue;
        const struct qs_expr *e=&p->exprs[index]; struct qs_vector *out=&vectors[index];
        memset(out->errors,0,count); memset(out->nulls,0,count); memset(out->dscales,e->dscale,count);
        if(e->op==Q_COLUMN) {
            if(e->column<0 || (unsigned)e->column>=p->nattrs) return -1;
            int slot=p->cols[e->column].slot;
            if(slot<0 || (unsigned)slot>=chunk->ncols) return -1;
            const int64_t *values=qs_values(chunk)+(size_t)slot*chunk->capacity;
            const uint8_t *nulls=qs_nulls(chunk)+(size_t)slot*((chunk->capacity+7)/8);
            const uint8_t *scales=qs_dscales(chunk)+(size_t)slot*chunk->capacity;
            for(unsigned i=0;i<count;++i) {
                unsigned row=selection?selection[first+i]:first+i;
                if(row>=chunk->rows) return -1;
                out->values[i]=values[row]; out->nulls[i]=(nulls[row/8]>>(row%8))&1; out->dscales[i]=scales[row];
            }
            continue;
        }
        if(e->op==Q_CONST) {
            for(unsigned i=0;i<count;++i) out->values[i]=e->constant;
            memset(out->nulls,e->isnull,count); continue;
        }
        const struct qs_vector *left=&vectors[e->left];
        if(e->op==Q_NOT || e->op==Q_IS_NULL || e->op==Q_IS_NOT_NULL) {
            for(unsigned i=0;i<count;++i) {
                out->errors[i]=left->errors[i];
                out->values[i]=e->op==Q_NOT?!left->values[i]:e->op==Q_IS_NULL?left->nulls[i]:!left->nulls[i];
                if(e->op==Q_NOT) { out->nulls[i]=left->nulls[i]; out->dscales[i]=left->dscales[i]; }
            }
            continue;
        }
        const struct qs_vector *right=&vectors[e->right];
        /* Errors are carried per row, not raised while computing a child.
         * AND/OR ignore RHS errors for rows which short-circuit on the LHS. */
        if(e->op==Q_AND || e->op==Q_OR) {
            for(unsigned i=0;i<count;++i) {
                if(!left->nulls[i] && (e->op==Q_AND?!left->values[i]:!!left->values[i])) {
                    out->values[i]=left->values[i]; out->nulls[i]=0; out->errors[i]=left->errors[i];
                } else {
                    out->errors[i]=left->errors[i]|right->errors[i];
                    out->values[i]=e->op==Q_AND?(left->values[i] && right->values[i]):(left->values[i] || right->values[i]);
                    out->nulls[i]=left->nulls[i]|right->nulls[i];
                    if(e->op==Q_AND && !right->nulls[i] && !right->values[i]) out->nulls[i]=0;
                    if(e->op==Q_OR && !right->nulls[i] && right->values[i]) out->nulls[i]=0;
                }
            }
            continue;
        }
        for(unsigned i=0;i<count;++i) {
            out->errors[i]=left->errors[i]|right->errors[i];
            out->nulls[i]=left->nulls[i]|right->nulls[i]; out->values[i]=0;
        }
        switch(e->op) {
        case Q_MUL:
            for(unsigned i=0;i<count;++i) if(!out->nulls[i] && !out->errors[i]) {
                out->errors[i]=__builtin_mul_overflow(left->values[i],right->values[i],&out->values[i]);
                out->dscales[i]=left->dscales[i]+right->dscales[i];
            }
            break;
        case Q_ADD:case Q_SUB: {
            unsigned lscale=e->scale-p->exprs[e->left].scale,rscale=e->scale-p->exprs[e->right].scale;
            __int128 lf,rf; int lover=qs_scale(1,lscale,&lf),rover=qs_scale(1,rscale,&rf);
            for(unsigned i=0;i<count;++i) if(!out->nulls[i] && !out->errors[i]) {
                __int128 l=0,r=0;
                out->errors[i]=(lover?left->values[i]!=0:__builtin_mul_overflow(left->values[i],lf,&l)) ||
                    (rover?right->values[i]!=0:__builtin_mul_overflow(right->values[i],rf,&r));
                if(!out->errors[i]) out->errors[i]=e->op==Q_ADD?
                    __builtin_add_overflow(l,r,&out->values[i]):__builtin_sub_overflow(l,r,&out->values[i]);
                out->dscales[i]=left->dscales[i]>right->dscales[i]?left->dscales[i]:right->dscales[i];
            }
            break;
        }
        default:
            if(e->op<Q_EQ || e->op>Q_GE) return -1;
            for(unsigned i=0;i<count;++i) if(!out->nulls[i] && !out->errors[i]) {
                int cmp=0;
                out->errors[i]=qs_compare(qs_vector_cell(p,vectors,e->left,i),qs_vector_cell(p,vectors,e->right,i),&cmp)!=0;
                switch(e->op) {
                case Q_EQ:out->values[i]=cmp==0;break; case Q_NE:out->values[i]=cmp!=0;break;
                case Q_LT:out->values[i]=cmp<0;break; case Q_LE:out->values[i]=cmp<=0;break;
                case Q_GT:out->values[i]=cmp>0;break; default:out->values[i]=cmp>=0;break;
                }
            }
        }
    }
    return 0;
}
#endif
