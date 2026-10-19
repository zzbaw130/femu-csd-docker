/* Optional resumable CSF convention. These are CompValue payloads, not CS_STATUS.
 * Last two compute args: IO AFDM, 128-bit control made with csCsfControl().
 * The batch scheduler owns tokens/LBAs; the CSF owns its private algorithm state. */
#ifndef SINA_CS_CSF_H
#define SINA_CS_CSF_H
#include <stdint.h>
#define CS_CSF_PAYLOAD_MASK UINT64_C(0xffffffffff)
#define CS_CSF_COMPLETION_MAGIC UINT64_C(0x4353100000000000)
#define CS_CSF_CONTROL_MAGIC UINT64_C(0x4353463100000000)
enum cs_csf_action {
    CS_CSF_DONE = 0, CS_CSF_NEED_STORE = 1, CS_CSF_NEED_LOAD = 2, CS_CSF_NEED_FREE = 3
};
enum cs_csf_event {
    CS_CSF_START = 0, CS_CSF_STORE_DONE = 1, CS_CSF_LOAD_DONE = 2, CS_CSF_FREE_DONE = 3, CS_CSF_ABORT = 4
};
static inline uint64_t csCsfCompletion(unsigned action, uint64_t payload)
{
    return CS_CSF_COMPLETION_MAGIC | ((uint64_t)action << 40) |
           (payload & CS_CSF_PAYLOAD_MASK);
}
static inline int csCsfIsCompletion(uint64_t value)
{
    return (value & UINT64_C(0xfffff00000000000)) == CS_CSF_COMPLETION_MAGIC;
}
static inline unsigned csCsfAction(uint64_t value) { return (value >> 40) & 15; }
static inline uint64_t csCsfPayload(uint64_t value) { return value & CS_CSF_PAYLOAD_MASK; }
static inline unsigned __int128 csCsfControl(unsigned event, uint64_t token, uint32_t bytes)
{
    uint64_t lo = (UINT64_C(1) << 48) | ((uint64_t)event << 40) |
                  (token & CS_CSF_PAYLOAD_MASK);
    return ((unsigned __int128)(CS_CSF_CONTROL_MAGIC | bytes) << 64) | lo;
}
static inline int csCsfIsControl(uint64_t lo, uint64_t hi)
{
    return (hi >> 32) == (CS_CSF_CONTROL_MAGIC >> 32) && (lo >> 48) == 1;
}
static inline unsigned csCsfEvent(uint64_t lo) { return (lo >> 40) & 255; }
static inline uint64_t csCsfToken(uint64_t lo) { return lo & CS_CSF_PAYLOAD_MASK; }
static inline uint32_t csCsfBytes(uint64_t hi) { return (uint32_t)hi; }
#endif
