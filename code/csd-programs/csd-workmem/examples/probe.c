/* Device-side test CSF. A and B are distinct .so files sharing libcs-workmem.so. */
#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "workmem-probe.h"
#include <errno.h>
#include <time.h>
#ifndef PROBE_VARIANT
#define PROBE_VARIANT 1
#endif
static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int wait_count(uint32_t *counter, uint32_t expected, uint64_t deadline)
{
    struct timespec pause = {.tv_nsec = 100000};
    while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < expected) {
        if (now_ns() > deadline)
            return -1;
        nanosleep(&pause, NULL);
    }
    return 0;
}
int64_t cs_workmem_probe(struct femu_csd_args *native)
{
    if (native->numr != 1 || native->mr_len[0] < (long long)sizeof(struct workmem_probe_control) ||
        native->cparam1 < 0 || native->cparam1 >= WORKMEM_PROBE_SLOTS)
        return -1;
    struct workmem_probe_control *control = native->mr_addr[0];
    if (!control->participants || control->participants > WORKMEM_PROBE_SLOTS || !control->bytes)
        return -1;
    struct workmem_probe_result *result = &control->slots[native->cparam1];
    cs_work_context *ctx = cs_work_begin();
    if (!ctx)
        return -2;
    void *data = cs_work_alloc(ctx, control->bytes);
    result->allocated = data != NULL;
    result->error = data ? 0 : errno;
    result->variant = PROBE_VARIANT;
    if (data) {
        ((unsigned char *)data)[0] = PROBE_VARIANT;
        ((unsigned char *)data)[control->bytes - 1] = PROBE_VARIANT;
    }
    uint64_t deadline = now_ns() + UINT64_C(5000000000);
    __atomic_add_fetch(&control->ready, 1, __ATOMIC_RELEASE);
    if (wait_count(&control->ready, control->participants, deadline)) {
        cs_work_end(ctx);
        return -3;
    }
    cs_work_stats stats;
    cs_work_get_stats(&stats);
    result->capacity = stats.capacity_bytes;
    result->used = stats.used_bytes;
    result->requested = stats.requested_bytes;
    result->peak = stats.peak_used_bytes;
    __atomic_add_fetch(&control->observed, 1, __ATOMIC_RELEASE);
    int status = wait_count(&control->observed, control->participants, deadline);
    cs_work_end(ctx);
    return status ? -4 : 0;
}
/* Process-local test hooks, never passed over NVMe. */
cs_work_context *workmem_test_begin(void) { return cs_work_begin(); }
void *workmem_test_alloc(cs_work_context *ctx, size_t n) { return cs_work_alloc(ctx, n); }
int workmem_test_end(cs_work_context *ctx) { return cs_work_end(ctx); }
int workmem_test_stats(cs_work_stats *out) { return cs_work_get_stats(out); }
int64_t cs_workmem_used(struct femu_csd_args *unused)
{
    (void)unused;
    cs_work_stats stats;
    if (cs_work_get_stats(&stats) || stats.used_bytes > INT32_MAX)
        return -1;
    return stats.used_bytes;
}
int64_t cs_workmem_capacity(struct femu_csd_args *unused)
{
    (void)unused;
    cs_work_stats stats;
    if (cs_work_get_stats(&stats) || stats.capacity_bytes > INT32_MAX)
        return -1;
    return stats.capacity_bytes;
}
