/* FEMU vendor wire format. Little endian, offsets are into the copied payload.
 * No SQL or SNIA types cross this interface. Applications still include cs.h. */
#ifndef FEMU_CSD_GRAPH_H
#define FEMU_CSD_GRAPH_H
#include <stdint.h>
#define FEMU_CSD_EXEC_GRAPH 0xe9 /* host-to-device descriptor, like EXEC 0xe1 */
#define FEMU_CSD_GRAPH_SESSION 0xea /* no data payload */
#define FEMU_CSD_GRAPH_MAX_NODES 64
#define FEMU_CSD_GRAPH_MAX_BYTES (1U << 20)
#define FEMU_CSD_GRAPH_SERIAL 1U
#define FEMU_CSD_GRAPH_CACHE 4U /* Caller guarantees LOAD input is immutable during EXEC. */
#define FEMU_CSD_GRAPH_PROFILE 2U /* existing SNIA_FEMU_PROFILE diagnostics */
#define FEMU_CSD_GRAPH_CACHE_SUMMARY 8U /* session totals without per-event logging */
enum { FEMU_GRAPH_OPEN, FEMU_GRAPH_CLOSE, FEMU_GRAPH_CANCEL,
       FEMU_GRAPH_FORGET_CANCEL, FEMU_GRAPH_ROLLBACK };
enum { FEMU_GRAPH_LOAD=1, FEMU_GRAPH_EXEC, FEMU_GRAPH_STORE };
struct femu_graph_header {
    uint32_t count, flags;
    uint64_t session, cookie;
    uint32_t bytes, reserved;
};
struct femu_graph_node {
    uint64_t dependencies, cparam1, cparam2;
    uint32_t operation, program;
    uint32_t payload, count; /* EXEC: native MR records; I/O: ranges below */
    uint32_t afdm, data_offset;
    uint64_t afdm_offset;
    uint32_t data_bytes, reserved;
};
struct femu_graph_range { uint32_t nsid, blocks; uint64_t lba; };
/* CSF-requested namespace reads. Records occupy the last MR from offset zero;
 * afdm_index selects another MR of that same invocation. All fields are LE. */
struct femu_graph_read {
    uint32_t nsid, blocks, afdm_index, pool_offset;
    uint64_t lba, offset;
};
/* Optional internal-read DMA source pool. pool_offset is the byte offset
 * of this record in the destination MR (zero skips population). Slots
 * immediately precede it. The CSF selects only slots with zero readers
 * for writes, including writes that do not issue a namespace read.
 * Geometry is little-endian and fixed for the pool lifetime. The runtime
 * owns pinned and readers, initially zero in newly allocated AFDM. */
#define FEMU_GRAPH_READ_POOL_MAX_SLOTS 256
struct femu_graph_read_pool {
    uint32_t slot_bytes, slots, pin_limit;
    uint32_t pinned;
    uint16_t readers[];
};
#endif
