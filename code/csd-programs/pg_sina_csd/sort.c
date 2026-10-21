#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "query_eval.h"
static int compare(const struct qs_row *a,const struct qs_row *b,const struct qs_plan *p)
{
    for(unsigned i=0;i<p->norder;++i) {
        const struct qs_order *key=&p->order[i];
        struct qs_cell x=a->cells[key->output],y=b->cells[key->output]; int cmp;
        if(x.isnull || y.isnull) {
            cmp=(x.isnull?1:0)-(y.isnull?1:0); if(key->nulls_first) cmp=-cmp;
        } else {
            if(qs_compare(x,y,&cmp)) return 0;
            if(key->descending) cmp=-cmp;
        }
        if(cmp) return cmp;
    }
    return 0;
}
static void sift(const struct qs_row *rows,uint16_t *order,unsigned n,unsigned root,const struct qs_plan *p)
{
    for(;;) {
        unsigned child=root*2+1; if(child>=n) break;
        if(child+1<n && compare(&rows[order[child]],&rows[order[child+1]],p)<0) ++child;
        if(compare(&rows[order[root]],&rows[order[child]],p)>=0) break;
        uint16_t temp=order[root]; order[root]=order[child]; order[child]=temp; root=child;
    }
}
int64_t pg_sina_sort(struct femu_csd_args *a)
{
    if(!a || a->numr!=2 || a->mr_len[0]<(long long)sizeof(struct qs_result) || a->mr_len[1]<(long long)sizeof(struct qs_plan)) return -1;
    struct qs_result *out=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1];
    if(out->rows>QS_GROUPS || p->norder>QS_OUT) return -1;
    if(out->rows<2) return 0;
    cs_work_context *ctx=cs_work_begin(); if(!ctx) return -4;
    struct qs_row *rows=cs_work_alloc(ctx,out->rows*(sizeof(*rows)+sizeof(uint16_t)));
    if(!rows) { cs_work_end(ctx); return -4; }
    uint16_t *order=(void *)(rows+out->rows);
    for(unsigned i=0;i<out->rows;++i) order[i]=i;
    for(unsigned i=out->rows/2;i>0;--i) sift(out->data,order,out->rows,i-1,p);
    for(unsigned n=out->rows;n>1;--n) {
        uint16_t temp=order[0]; order[0]=order[n-1]; order[n-1]=temp; sift(out->data,order,n-1,0,p);
    }
    for(unsigned i=0;i<out->rows;++i) rows[i]=out->data[order[i]];
    memcpy(out->data,rows,out->rows*sizeof(*rows)); cs_work_end(ctx); return 0;
}
