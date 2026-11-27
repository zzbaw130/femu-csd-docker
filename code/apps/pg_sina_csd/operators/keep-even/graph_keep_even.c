#include "graph_batch.h"

int64_t pg_sina_graph_keep_even(struct femu_csd_args *args)
{
    struct graph_batch batch;
    enum graph_batch_status input=graph_batch_read(args,&batch);
    if(input==GRAPH_BATCH_EOF) return graph_finish_stream();
    if(input!=GRAPH_BATCH_READY) return -1;

    bool retained=false;
    unsigned count=graph_batch_count(&batch);
    for(unsigned row=0;row<count;++row) {
        int64_t value;
        if(graph_batch_i64(&batch,row,&value) && value%2==0) {
            if(graph_batch_keep(&batch,row)) return -1;
            retained=true;
        }
    }
    return retained?graph_batch_borrow(&batch):graph_consume_input();
}
