#include "femu-csd-kernel.h"
#include "query_eval.h"
int64_t pg_sina_filter(struct femu_csd_args *a)
{
    if(!a || a->numr!=2 || a->mr_len[1]<(long long)sizeof(struct qs_plan)) return -1;
    struct qs_chunk *c=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1];
    if(a->mr_len[0]<(long long)sizeof(*c) || qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0] ||
        c->ncols!=p->ncols || c->rows>c->capacity || p->nexprs>QS_EXPRS) return -1;
    c->selected=0;
    for(unsigned row=0;row<c->rows;++row) {
        struct qs_eval e; e.plan=p; e.chunk=c; e.row=row; memset(e.valid,0,p->nexprs); struct qs_cell value;
        if(qs_evaluate(&e,p->predicate,&value)) return -2;
        if(!value.isnull && value.value) qs_selection(c)[c->selected++]=row;
    }
    return 0;
}
