#include "femu-csd-kernel.h"
#include "query.h"

static int valid_batch(const struct femu_csd_args *a)
{
    return a && a->numr==2 && a->mr_len[0]>=(long long)sizeof(struct qs_result) &&
        a->mr_len[1]>=(long long)sizeof(struct qs_graph_tail);
}

int64_t pg_sina_graph_offset(struct femu_csd_args *a)
{
    if(!valid_batch(a)) return -1;
    struct qs_result *out=a->mr_addr[0];
    struct qs_graph_tail *state=a->mr_addr[1];
    if(out->status || out->begin>QS_RESULT_ROWS || out->rows>QS_RESULT_ROWS-out->begin) return -1;
    unsigned skip=state->remaining<out->rows?state->remaining:out->rows;
    state->remaining-=skip;
    out->begin+=skip;
    out->rows-=skip;
    return 0;
}

int64_t pg_sina_graph_sum(struct femu_csd_args *a)
{
    if(!valid_batch(a)) return -1;
    struct qs_result *out=a->mr_addr[0];
    struct qs_graph_tail *state=a->mr_addr[1];
    if(out->status || out->begin>QS_RESULT_ROWS || out->rows>QS_RESULT_ROWS-out->begin) return -1;
    for(unsigned i=out->begin;i<out->begin+out->rows;++i) {
        const struct qs_cell *cell=&out->data[i].cells[0];
        if(cell->isnull) continue;
        if(state->sum.isnull) state->sum=*cell;
        else {
            if(cell->scale!=state->sum.scale ||
                __builtin_add_overflow(state->sum.value,cell->value,&state->sum.value)) return -1;
            if(cell->dscale>state->sum.dscale) state->sum.dscale=cell->dscale;
        }
    }
    if(!out->finished) return 0;
    out->begin=0;
    out->rows=1;
    out->data[0].cells[0]=state->sum;
    return 1;
}
