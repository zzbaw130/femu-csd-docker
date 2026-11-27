#include "graph_state.h"
#include "graph_batch.h"

static int64_t project_read_row(struct execution *s,unsigned id,struct qs_cell *row)
{
    struct operator_state *state=&s->nodes[id];
    const struct qp_node *node=&s->plan->nodes[id];
    for(;;) {
        int64_t status=next_row(s,node->left,state->left);
        if(status!=2) return status;
        int projected=project(s,node,state->left,NULL,row);
        if(projected<0) return projected;
        if(projected) { ++state->emitted; return 2; }
    }
}

static int64_t stream_project(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    (void)sink;
    (void)out;
    if(!s->block_chain && !graph_device_source(&s->plan->nodes[scan])) return -1;
    int id=graph_stream_node(s); if(id<0) return -1;
    if(s->args->graph_input==FEMU_GRAPH_INPUT_EOF) return graph_finish_stream();
    s->nodes[id].read_row=project_read_row;
    s->nodes[id].stream_inlet=true;
    int64_t status=graph_stream_project(s,scan,id);
    return status==2?graph_publish_output():status;
}

int64_t pg_sina_graph_block_project(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_PROJECT,stream_project); }
