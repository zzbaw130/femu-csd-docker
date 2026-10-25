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
#define FEMU_CSD_GRAPH_PROFILE 2U /* existing SNIA_FEMU_PROFILE diagnostics */
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
    uint32_t nsid, blocks, afdm_index, reserved;
    uint64_t lba, offset;
};
#endif
