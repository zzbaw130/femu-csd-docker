#include "graph_state.h"

#ifndef QP_SPLIT_RUNTIME
int64_t materialized_row(struct execution *s,unsigned id,uint64_t position,struct qs_cell *row)
{
    int64_t status=materialize(s,id); if(status) return status;
    const struct qp_node *n=&s->plan->nodes[id];
    struct spool *group=&s->nodes[id].group;
    if(position==group->rows) return 0;
    status=spool_row(s,group,position,s->plan->nodes[n->left].expr.nout,row);
    return status?status:2;
}
#endif

static int cte_open_source(struct execution *s,unsigned id)
{
    struct operator_state *local=&s->nodes[id];
    if(local->input_done) return 0;
    const struct qp_node *n=&s->plan->nodes[id];
    int shared=n->left;
    if(!s->owner || shared<0 || (unsigned)shared>=s->plan->count || !n->expr.nattrs ||
       n->expr.nattrs!=s->plan->nodes[shared].expr.nout) return -1;
    const struct spool *published=graph_state_owner(s,shared)->published_rows;
    if(!published) return -1;
    s->source=*published;
    s->source.cache=NULL;
    s->source.loaded=UINT_MAX;
    local->input_done=1;
    return 0;
}

static int64_t projection_cte_read(struct execution *s,struct projection_batch *b)
{
    int opened=cte_open_source(s,b->scan); if(opened) return opened;
    unsigned columns=b->plan.nattrs;
    unsigned block_rows=QS_SORT_IO_BYTES/(columns*sizeof(struct qs_cell));
    while(b->input_count<QS_VECTOR_ROWS) {
        if(s->source_at==s->source_end) {
            s->source_at=atomic_fetch_add(&s->owner->next_offset,block_rows);
            if(s->source_at>=s->source.rows) {
                s->source_end=s->source_at;
                break;
            }
            s->source_end=s->source_at+block_rows;
            if(s->source_end>s->source.rows) s->source_end=s->source.rows;
        }
        struct qs_cell row[QS_OUT];
        int64_t status=spool_row(s,&s->source,s->source_at,columns,row); if(status) return status;
        projection_append(b,row);
        ++s->source_at;
    }
    return b->input_count?2:0;
}

int64_t projection_cte_fill(struct execution *s,struct projection_batch *b)
{
    if(s->stream_source_window && !b->input_count) { s->scan_drained=true; return QP_WAIT; }
    if(!s->stream_source_window) {
        int64_t status=projection_cte_read(s,b); if(status!=2) return status;
    }
    b->count=b->input_count; b->input_count=b->position=0;
    struct qs_vector_input input={.cells=b->cells,.dictionary=s->dictionary,
        .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    return projection_execute(b,NULL,NULL,0,input)?-5:2;
}

static int64_t cte_read_row(struct execution *s,unsigned id,struct qs_cell *row)
{
    struct operator_state *state=&s->nodes[id];
    return state->projection?projection_next(s,id,row,0):-1;
}

static int64_t stream_cte_scan(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    (void)sink;
    (void)out;
    s->nodes[scan].read_projection=projection_cte_fill;
    s->nodes[scan].read_row=cte_read_row;
    return graph_device_scan(s,scan,projection_cte_read);
}

int64_t pg_sina_graph_cte_scan(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_CTE_SCAN,stream_cte_scan); }
