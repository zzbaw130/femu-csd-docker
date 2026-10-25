/* Device-side helpers for reentrant CSFs. CompValue requests, not CS_STATUS. */
#ifndef SNIA_CS_CSF_H
#define SNIA_CS_CSF_H
#include <stdint.h>
#include "femu-csd-kernel.h"
#include "csd-graph.h"
/* A reentrant CSF reserves cparam2. The scheduler supplies the completed I/O's
 * spill ID or namespace-read count there; input/state remain in cparam1/AFDMs. */
static inline int csIsReentry(const struct femu_csd_args *args)
{
    return args->cparam2 != 0;
}
static inline uint64_t csReentryValue(const struct femu_csd_args *args)
{
    return (uint64_t)args->cparam2;
}
/* Batch CompValue: upper four bits are the operation, lower 60 length/ID/count.
 * Other values complete the node normally. No control argument is required. */
#define CS_NEED_STORE (UINT64_C(1) << 60)
#define CS_NEED_LOAD  (UINT64_C(2) << 60)
#define CS_NEED_FREE  (UINT64_C(3) << 60)
#define CS_NEED_NVM_READ (UINT64_C(4) << 60)
#define CS_IO_VALUE_MASK ((UINT64_C(1) << 60) - 1)
static inline int64_t csNeedStore(uint64_t bytes) { return bytes > CS_IO_VALUE_MASK ? -1 : (int64_t)(CS_NEED_STORE | bytes); }
static inline int64_t csNeedLoad(uint64_t id) { return id > CS_IO_VALUE_MASK ? -1 : (int64_t)(CS_NEED_LOAD | id); }
static inline int64_t csNeedFree(uint64_t id) { return id > CS_IO_VALUE_MASK ? -1 : (int64_t)(CS_NEED_FREE | id); }
/* FEMU exec_graph extension, not a standard SNIA status: snapshot count read
 * records from the final MR, load internally, then reenter with count in
 * cparam2. The CSF retains its own pending-operation state as for spill. */
static inline int64_t csNeedNvmRead(unsigned count)
{ return !count || count>FEMU_CSD_GRAPH_MAX_NODES ? -1 : (int64_t)(CS_NEED_NVM_READ | count); }
#endif
