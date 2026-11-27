#ifndef PG_SINA_GRAPH_BATCH_H
#define PG_SINA_GRAPH_BATCH_H

#include <stdbool.h>
#include <stdint.h>

struct femu_csd_args;
struct qs_chunk;

/* A view of the input for the current EXEC call. Fields are runtime-owned. */
struct graph_batch {
    struct qs_chunk *chunk;
    unsigned column;
    unsigned retained,next_row;
};

enum graph_batch_status { GRAPH_BATCH_ERROR=-1, GRAPH_BATCH_EOF, GRAPH_BATCH_READY };

/* The current input binding reads a heap-scan vector. PG supplies its
 * integer column slot in the EXEC arguments. */
enum graph_batch_status graph_batch_read(const struct femu_csd_args *args,struct graph_batch *batch);
unsigned graph_batch_count(const struct graph_batch *batch);
bool graph_batch_i64(const struct graph_batch *batch,unsigned row,int64_t *value);

/* With exclusive access to the input selection, read and retain in row order.
 * Retaining compacts row indices in place while leaving the columns intact. */
int graph_batch_keep(struct graph_batch *batch,unsigned row);
int64_t graph_batch_borrow(struct graph_batch *batch);

/* Return these statuses from an EXEC to drive the graph's stream edge. */
int64_t graph_publish_output(void);
int64_t graph_borrow_input(void);
int64_t graph_consume_input(void);
int64_t graph_finish_stream(void);

#endif
