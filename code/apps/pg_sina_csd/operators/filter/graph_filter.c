#include "graph_state.h"
#include "graph_batch.h"

static int64_t stream_filter(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    (void)sink; (void)out;
    const struct qp_node *leaf=&s->plan->nodes[scan];
    if(graph_device_source(leaf)) {
        if(s->args->graph_input==FEMU_GRAPH_INPUT_EOF) return graph_finish_stream();
        int64_t status=projection_fill(s,scan,0);
        if(status==QP_WAIT && s->scan_drained) return graph_consume_input();
        if(status!=2) return status;
        return graph_borrow_input();
    }
    struct operator_state *o=&s->nodes[scan];
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,leaf));
    if(s->args->graph_input==FEMU_GRAPH_INPUT_EOF) return graph_finish_stream();
    int64_t status=s->stream_filter_retry?-4:scan_filter_chunk(s,scan);
    if(status==-4) {
        if(!s->stream_filter_retry) {
            cs_work_free(s->context,o->scan_filter); o->scan_filter=NULL;
            s->stream_filter_retry=true;
        }
#ifdef QP_SPLIT_RUNTIME
        status=o->decode_chunk?o->decode_chunk(s,scan,o->scan_raw_bytes,true):-1;
#else
        status=scan_decode(s,scan,o->scan_raw_bytes,true);
#endif
    }
    if(status) return status;
    s->stream_filter_retry=s->scan_drained=false;
    if(!chunk->selected) return graph_consume_input();
    if(s->plan->nodes[s->root].kind==QP_INDEX_JOIN)
        s->nodes[s->root].input_done=s->nodes[s->root].finished=0;
    return graph_borrow_input();
}

int64_t pg_sina_graph_block_filter(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_FILTER,stream_filter); }
