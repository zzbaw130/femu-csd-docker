#include "graph_state.h"
#include "graph_batch.h"

#ifndef QP_SPLIT_RUNTIME
int64_t materialize(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned cols=s->plan->nodes[n->left].expr.nout;
    while(!o->group_ready) {
        if(!o->have_left) {
            int64_t status=next_row(s,n->left,o->left);
            if(status==QP_WAIT || status<0 || status>2) return status;
            if(!status) { o->group_ready=1; break; }
            o->have_left=1;
        }
        int64_t status=spool_append(s,&o->group,o->left,cols); if(status) return status;
        o->have_left=0;
    }
    o->published_rows=&o->group;
    return 0;
}
#endif

static int64_t materialize_step(struct execution *,struct femu_csd_args *,unsigned,unsigned);

static int64_t materialized_read_row(struct execution *s,unsigned id,struct qs_cell *row)
{
    struct operator_state *state=&s->nodes[id];
    struct spool *rows=state->published_rows;
    if(state->position==rows->rows) return 0;
    int64_t status=spool_row(s,rows,state->position,s->plan->nodes[id].expr.nout,row);
    if(status) return status;
    ++state->position;
    return 2;
}

int64_t pg_sina_graph_cte_materialize(struct femu_csd_args *a)
{ return graph_tail_call(a,materialize_step,false); }

static int64_t materialize_step(struct execution *s,struct femu_csd_args *a,unsigned worker,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id];
    int64_t status;
    if(worker || n->kind!=QP_MATERIALIZED || n->left<0) return -1;
    struct operator_state *o=&s->nodes[id];
    if(a->graph_input==FEMU_GRAPH_INPUT_EOF) {
        o->group_ready=1;
        o->published_rows=&o->group;
        o->read_row=materialized_read_row;
        return csGraphStep(FEMU_GRAPH_STEP_DONE,QP_GRAPH_PHASE_DONE);
    }
    struct projection_batch *input=s->nodes[n->left].source_projection;
    if(!input || o->group_ready) return -1;
    size_t reserve=sort_spill_reserve(s);
    for(;input->position<input->count;++input->position) {
        struct qs_cell row[QS_OUT];
        for(unsigned col=0;col<input->plan.nout;++col) {
            unsigned expr=input->plan.outputs[col].index;
            if(input->vectors[expr].errors[input->position]) return -5;
            row[col]=qs_vector_cell(&input->plan,input->vectors,expr,input->position);
        }
        status=spool_append_context(s,&o->group,row,input->plan.nout*sizeof(*row),s->context,reserve);
        if(status) return status;
    }
    return graph_consume_input();
}
