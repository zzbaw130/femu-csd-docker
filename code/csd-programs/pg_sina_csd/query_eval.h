/* Exact scalar primitives shared by vector filter and aggregate CSFs. */
#ifndef QS_EVAL_H
#define QS_EVAL_H
#include "query.h"
#include <string.h>
#include <limits.h>
static inline int qs_scale(__int128 value, unsigned places, __int128 *out)
{
    while(places--) if(__builtin_mul_overflow(value,(__int128)10,&value)) return -1;
    *out=value; return 0;
}
static inline int qs_pgdate_year(int32_t days)
{
    /* PostgreSQL date is days since 2000-01-01; convert to civil year. */
    int64_t z=(int64_t)days+10957+719468;
    int64_t era=(z>=0?z:z-146096)/146097;
    unsigned doe=(unsigned)(z-era*146097);
    unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
    int64_t year=(int64_t)yoe+era*400;
    unsigned doy=doe-(365*yoe+yoe/4-yoe/100);
    unsigned mp=(5*doy+2)/153;
    unsigned month=mp+(mp<10?3:-9);
    if(month<=2) ++year;
    return (int)year;
}
static inline int qs_compare(struct qs_cell a, struct qs_cell b, int *result)
{
    if(a.dscale==UINT8_MAX || b.dscale==UINT8_MAX) {
        if(a.dscale!=UINT8_MAX || b.dscale!=UINT8_MAX) return -1;
        unsigned al=a.scale,bl=b.scale,n=al<bl?al:bl;
        int cmp=memcmp(&a.value,&b.value,n);
        if(!cmp) cmp=(al>bl)-(al<bl);
        *result=(cmp>0)-(cmp<0); return 0;
    }
    if(a.scale==b.scale) { *result=(a.value>b.value)-(a.value<b.value); return 0; }
    unsigned scale=a.scale>b.scale?a.scale:b.scale; __int128 x,y;
    if(!qs_scale(a.value,scale-a.scale,&x) && !qs_scale(b.value,scale-b.scale,&y)) {
        *result=(x>y)-(x<y); return 0;
    }
    /* Rare near-int128-limit values: compare digits without overflowing a
     * temporary common scale. Equal exponents are compared with zero padding. */
    if(a.value<=0 && b.value>=0) { *result=-1; return 0; }
    if(a.value>=0 && b.value<=0) { *result=1; return 0; }
    unsigned __int128 av=a.value<0?0-(unsigned __int128)a.value:(unsigned __int128)a.value;
    unsigned __int128 bv=b.value<0?0-(unsigned __int128)b.value:(unsigned __int128)b.value;
    unsigned char ad[40],bd[40]; int an=0,bn=0;
    do { ad[an++]=av%10; av/=10; } while(av);
    do { bd[bn++]=bv%10; bv/=10; } while(bv);
    int ae=an-a.scale,be=bn-b.scale,cmp=(ae>be)-(ae<be);
    if(!cmp) for(int i=0;i<(an>bn?an:bn);++i) {
        int ac=i<an?ad[an-1-i]:0,bc=i<bn?bd[bn-1-i]:0;
        if(ac!=bc) { cmp=(ac>bc)-(ac<bc); break; }
    }
    *result=cmp*(a.value<0?-1:1); return 0;
}

static inline int qs_divide(struct qs_cell,struct qs_cell,struct qs_cell *);
struct qs_eval { const struct qs_plan *plan; struct qs_chunk *chunk; unsigned row; uint8_t valid[QS_EXPRS]; struct qs_cell cells[QS_EXPRS]; const struct qs_cell *aggregates; const struct qs_cell *tuple; };
static inline int qs_evaluate(struct qs_eval *e, int index, struct qs_cell *out)
{
    if(index<0 || (unsigned)index>=e->plan->nexprs) return -1;
    if(e->valid[index]) { *out=e->cells[index]; return 0; }
    const struct qs_expr *x=&e->plan->exprs[index];
    struct qs_cell v={.scale=x->scale,.dscale=x->dscale},a={0},b={0};
    if(x->op==Q_CONST) { v.value=x->constant; v.isnull=x->isnull; }
    else if(x->op==Q_AGGREF) {
        if(!e->aggregates || x->column<0 || (unsigned)x->column>=e->plan->naggs) return -1;
        v=e->aggregates[x->column];
    }
    else if(x->op==Q_COLUMN) {
        if(e->tuple) { v=e->tuple[x->column]; e->valid[index]=1; e->cells[index]=v; *out=v; return 0; }
        int slot=e->plan->cols[x->column].slot;
        if(slot<0 || (unsigned)slot>=e->chunk->ncols) return -1;
        v.value=qs_values(e->chunk)[(size_t)slot*e->chunk->capacity+e->row];
        v.dscale=qs_dscales(e->chunk)[(size_t)slot*e->chunk->capacity+e->row];
        v.isnull=(qs_nulls(e->chunk)[(size_t)slot*((e->chunk->capacity+7)/8)+e->row/8]>>(e->row%8))&1;
    } else if(x->op==Q_YEAR) {
        if(qs_evaluate(e,x->left,&a)) return -1;
        v.isnull=a.isnull; v.scale=v.dscale=0;
        if(!v.isnull) v.value=qs_pgdate_year((int32_t)a.value);
    } else {
        if(qs_evaluate(e,x->left,&a)) return -1;
        if(x->op==Q_CASE) {
            if(qs_evaluate(e,!a.isnull && a.value?x->right:x->otherwise,&v)) return -1;
            if(!v.isnull && (v.scale>x->scale || qs_scale(v.value,x->scale-v.scale,&v.value))) return -1;
            v.scale=x->scale;
            e->valid[index]=1; e->cells[index]=v; *out=v; return 0;
        }
        if(x->op==Q_AND && !a.isnull && !a.value) { *out=a; return 0; }
        if(x->op==Q_OR && !a.isnull && a.value) { *out=a; return 0; }
        if(x->op==Q_NOT) { v=a; v.value=!a.value; }
        else if(x->op==Q_IS_NULL || x->op==Q_IS_NOT_NULL) { v.value=x->op==Q_IS_NULL?a.isnull:!a.isnull; }
        else {
            if(qs_evaluate(e,x->right,&b)) return -1;
            v.isnull=a.isnull || b.isnull;
            if(x->op==Q_AND || x->op==Q_OR) {
                v.value=x->op==Q_AND?(a.value && b.value):(a.value || b.value);
                if(x->op==Q_AND && ((!a.isnull&&!a.value)||(!b.isnull&&!b.value))) v.isnull=0;
                if(x->op==Q_OR && ((!a.isnull&&a.value)||(!b.isnull&&b.value))) v.isnull=0;
            } else if(!v.isnull) {
                if(x->op==Q_MUL) {
                    if(__builtin_mul_overflow(a.value,b.value,&v.value)) return -1;
                    v.scale=a.scale+b.scale;
                    v.dscale=a.dscale+b.dscale;
                } else if(x->op==Q_ADD || x->op==Q_SUB) {
                    v.scale=a.scale>b.scale?a.scale:b.scale;
                    __int128 left,right;
                    if(qs_scale(a.value,v.scale-a.scale,&left)||qs_scale(b.value,v.scale-b.scale,&right)) return -1;
                    if(x->op==Q_ADD ? __builtin_add_overflow(left,right,&v.value) : __builtin_sub_overflow(left,right,&v.value)) return -1;
                    v.dscale=a.dscale>b.dscale?a.dscale:b.dscale;
                } else if(x->op==Q_DIV) {
                    if(qs_divide(a,b,&v)) return -1;
                } else {
                    int cmp; if(qs_compare(a,b,&cmp)) return -1;
                    switch(x->op) {
                    case Q_EQ:v.value=cmp==0;break; case Q_NE:v.value=cmp!=0;break;
                    case Q_LT:v.value=cmp<0;break; case Q_LE:v.value=cmp<=0;break;
                    case Q_GT:v.value=cmp>0;break; case Q_GE:v.value=cmp>=0;break;
                    default:return -1;
                    }
                }
            }
        }
    }
    e->valid[index]=1; e->cells[index]=v; *out=v; return 0;
}
/* PostgreSQL numeric.c select_div_scale: base-10000 leading digit/weight. */
static inline void qs_leading(unsigned __int128 value, unsigned scale, int *weight, unsigned *digit)
{
    if(!value) { *weight=0; *digit=0; return; }
    unsigned digits=1; unsigned __int128 n=value;
    while(n>=10) { n/=10; ++digits; }
    int exponent=(int)digits-1-(int)scale;
    *weight=exponent>=0?exponent/4:(exponent-3)/4;
    int shift=(int)scale+*weight*4;
    while(shift>0) { value/=10; --shift; }
    while(shift<0) { value*=10; ++shift; }
    *digit=(unsigned)value;
}
static inline int qs_average(struct qs_cell sum, uint64_t count, struct qs_cell *out)
{
    int w1,w2; unsigned d1,d2;
    unsigned __int128 magnitude=sum.value<0?0-(unsigned __int128)sum.value:(unsigned __int128)sum.value;
    qs_leading(magnitude,sum.scale,&w1,&d1); qs_leading(count,0,&w2,&d2);
    int scale=16-(w1-w2-(d1<=d2))*4;
    if(scale<sum.dscale) scale=sum.dscale;
    if(scale<0) scale=0;
    if(scale>30) return -1;
    __int128 numerator,divisor=count;
    if(scale>=sum.scale) {
        if(qs_scale(sum.value,scale-sum.scale,&numerator)) return -1;
    } else {
        numerator=sum.value;
        if(qs_scale(divisor,sum.scale-scale,&divisor)) return -1;
    }
    __int128 quotient=numerator/divisor,remainder=numerator%divisor;
    unsigned __int128 abs_remainder=remainder<0?0-(unsigned __int128)remainder:(unsigned __int128)remainder;
    if(abs_remainder>=(unsigned __int128)(divisor/2+divisor%2)) quotient+=numerator<0?-1:1;
    *out=(struct qs_cell){.value=quotient,.scale=scale,.dscale=scale}; return 0;
}
/* Same select_div_scale/half-away-from-zero rule as PG numeric division. */
static inline int qs_divide(struct qs_cell a,struct qs_cell b,struct qs_cell *out)
{
    if(!b.value) return -1;
    unsigned __int128 av=a.value<0?0-(unsigned __int128)a.value:(unsigned __int128)a.value;
    unsigned __int128 bv=b.value<0?0-(unsigned __int128)b.value:(unsigned __int128)b.value;
    int w1,w2; unsigned d1,d2;
    qs_leading(av,a.scale,&w1,&d1); qs_leading(bv,b.scale,&w2,&d2);
    int scale=16-(w1-w2-(d1<=d2))*4;
    if(scale<a.dscale) scale=a.dscale;
    if(scale<b.dscale) scale=b.dscale;
    if(scale<0) scale=0;
    if(scale>30) return -1;
    int shift=scale+(int)b.scale-(int)a.scale;
    __int128 numerator=a.value,divisor=b.value;
    if(shift>=0) { if(qs_scale(numerator,shift,&numerator)) return -1; }
    else if(qs_scale(divisor,-shift,&divisor)) return -1;
    if(divisor<0 && (__builtin_sub_overflow((__int128)0,divisor,&divisor) ||
        __builtin_sub_overflow((__int128)0,numerator,&numerator))) return -1;
    __int128 quotient=numerator/divisor,remainder=numerator%divisor;
    unsigned __int128 rem=remainder<0?0-(unsigned __int128)remainder:(unsigned __int128)remainder;
    if(rem>=(unsigned __int128)(divisor/2+divisor%2) &&
        __builtin_add_overflow(quotient,(__int128)(numerator<0?-1:1),&quotient)) return -1;
    *out=(struct qs_cell){.value=quotient,.scale=scale,.dscale=scale}; return 0;
}
#endif
