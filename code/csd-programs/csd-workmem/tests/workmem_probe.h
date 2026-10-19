/* Shared test ABI: fixed-width AFDM data, no pointers. Guest reads after completion. */
#ifndef CSD_WORKMEM_PROBE_H
#define CSD_WORKMEM_PROBE_H
#include <stdint.h>
#define WORKMEM_PROBE_SLOTS 4
struct workmem_probe_result {
    uint64_t capacity, used, requested, peak;
    uint32_t allocated, error, variant, reserved;
};
struct workmem_probe_control {
    uint32_t ready, observed;
    uint64_t bytes;
    uint32_t participants, reserved;
    struct workmem_probe_result slots[WORKMEM_PROBE_SLOTS];
};
#endif
