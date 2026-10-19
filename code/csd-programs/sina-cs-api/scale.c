/* Device program: AFDM contains only declared inputs and outputs. */
#include "cs_kernel.h"
#include "cs_csf.h"
#include <string.h>
#include <stdint.h>
#include <time.h>
int64_t cs_scale(struct femu_csd_args *args)
{
    struct cs_kernel_arg decoded[CS_API_MAX_ARGS];
    if (cs_kernel_decode(args, decoded) != 3)
        return -1;
    const struct cs_kernel_arg *in = &decoded[0], *out = &decoded[1],
                                   *count = &decoded[2];
    if (in->type != 1 || out->type != 1 || count->type != 4 || count->value_lo > in->bytes / 4 ||
        count->value_lo > out->bytes / 4)
        return -2;
    const uint32_t *src = in->address;
    uint32_t *dst = out->address;
    for (uint64_t i = 0; i < count->value_lo; i++)
        dst[i] = src[i] * 2;
    return count->value_lo;
}
int64_t cs_scalar_sum(struct femu_csd_args *args)
{
    uint64_t sum = 0;
    struct cs_kernel_arg decoded[CS_API_MAX_ARGS];
    int n = cs_kernel_decode(args, decoded);
    if (n < 0)
        return -1;
    for (int i = 0; i < n; i++) {
        if (decoded[i].type < 2 || decoded[i].type > 6)
            return -2;
        sum += decoded[i].value_lo;
    }
    return sum > INT32_MAX ? -3 : (int64_t)sum;
}
int64_t cs_delay(struct femu_csd_args *args)
{
    struct cs_kernel_arg decoded[CS_API_MAX_ARGS];
    int n = cs_kernel_decode(args, decoded);
    uint64_t *stamps = n == 1 && decoded[0].type == 1 && decoded[0].bytes >= 16
                           ? decoded[0].address : NULL;
    struct timespec now;
    if (stamps) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        stamps[0] = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    }
    struct timespec delay = {.tv_nsec = 100000000};
    nanosleep(&delay, 0);
    if (stamps) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        stamps[1] = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    }
    return 0;
}

int64_t cs_check_128(struct femu_csd_args *args)
{
    struct cs_kernel_arg decoded[CS_API_MAX_ARGS];
    if (cs_kernel_decode(args, decoded) != 1 || decoded[0].type != 6)
        return -1;
    return decoded[0].value_lo == 7 && decoded[0].value_hi == 3 ? 1 : -2;
}

/* A reproducible spill example: one AFDM is the declared temporary I/O buffer.
 * 8191 valid bytes deliberately exercises sector rounding without a host copy. */
static int spill_args(struct femu_csd_args *native, struct cs_kernel_arg *a)
{
    int n = cs_kernel_decode(native, a);
    if (n < 2 || a[n - 2].type != 1 || a[n - 2].bytes < 8192 || a[n - 1].type != 6 ||
        !csCsfIsControl(a[n - 1].value_lo, a[n - 1].value_hi))
        return -1;
    return n;
}
int64_t cs_result64(struct femu_csd_args *native)
{
    struct cs_kernel_arg a[CS_API_MAX_ARGS];
    if (cs_kernel_decode(native, a) != 1 || a[0].type != 5)
        return -1;
    return (int64_t)a[0].value_lo;
}
int64_t cs_spill_roundtrip(struct femu_csd_args *native)
{
    struct cs_kernel_arg a[CS_API_MAX_ARGS];
    int n = spill_args(native, a);
    if (n < 0)
        return -1;
    uint64_t lo = a[n - 1].value_lo;
    unsigned event = csCsfEvent(lo);
    uint64_t token = csCsfToken(lo);
    uint8_t *io = a[n - 2].address;
    if (event == CS_CSF_ABORT)
        return csCsfCompletion(CS_CSF_DONE, 0);
    if (event == CS_CSF_START) {
        for (unsigned i = 0; i < 8191; i++)
            io[i] = (uint8_t)(i * 13 + 7);
        io[8191] = 0; /* Padding belongs to the declared I/O buffer. */
        return csCsfCompletion(CS_CSF_NEED_STORE, 8191);
    }
    if (event == CS_CSF_STORE_DONE) {
        memset(io, 0, 8192);
        return csCsfCompletion(CS_CSF_NEED_LOAD, token);
    }
    if (event == CS_CSF_LOAD_DONE) {
        if (csCsfBytes(a[n - 1].value_hi) != 8191)
            return -2;
        for (unsigned i = 0; i < 8191; i++)
            if (io[i] != (uint8_t)(i * 13 + 7))
                return -3;
        return csCsfCompletion(CS_CSF_NEED_FREE, token);
    }
    if (event == CS_CSF_FREE_DONE)
        return csCsfCompletion(CS_CSF_DONE, 8191);
    return -4;
}
/* These two CSFs also demonstrate tokens surviving a successful batch boundary. */
int64_t cs_spill_store(struct femu_csd_args *native)
{
    struct cs_kernel_arg a[CS_API_MAX_ARGS];
    int n = spill_args(native, a);
    if (n < 0)
        return -1;
    unsigned event = csCsfEvent(a[n - 1].value_lo);
    if (event == CS_CSF_ABORT)
        return csCsfCompletion(CS_CSF_DONE, 0);
    if (event == CS_CSF_START) {
        memset(a[n - 2].address, 0x5a, 8192);
        return csCsfCompletion(CS_CSF_NEED_STORE, 8191);
    }
    return event == CS_CSF_STORE_DONE
        ? (int64_t)csCsfCompletion(CS_CSF_DONE, csCsfToken(a[n - 1].value_lo)) : -2;
}
int64_t cs_spill_load(struct femu_csd_args *native)
{
    struct cs_kernel_arg a[CS_API_MAX_ARGS];
    int n = spill_args(native, a);
    if (n != 3 || a[0].type != 5)
        return -1;
    unsigned event = csCsfEvent(a[2].value_lo);
    if (event == CS_CSF_ABORT)
        return csCsfCompletion(CS_CSF_DONE, 0);
    if (event == CS_CSF_START)
        return csCsfCompletion(CS_CSF_NEED_LOAD, a[0].value_lo);
    if (event == CS_CSF_LOAD_DONE) {
        const uint8_t *io = a[1].address;
        for (unsigned i = 0; i < 8191; i++)
            if (io[i] != 0x5a)
                return -2;
        return csCsfCompletion(CS_CSF_NEED_FREE, csCsfToken(a[2].value_lo));
    }
    return event == CS_CSF_FREE_DONE ? (int64_t)csCsfCompletion(CS_CSF_DONE, 8191) : -3;
}
int64_t cs_spill_fail(struct femu_csd_args *native)
{
    struct cs_kernel_arg a[CS_API_MAX_ARGS];
    int n = spill_args(native, a);
    if (n < 0)
        return -1;
    if (csCsfEvent(a[n - 1].value_lo) == CS_CSF_START)
        return csCsfCompletion(CS_CSF_NEED_STORE, 8191);
    return -9; /* A failed batch must reclaim the extent it just allocated. */
}
int64_t cs_spill_validate(struct femu_csd_args *native)
{
    struct cs_kernel_arg a[CS_API_MAX_ARGS];
    if (cs_kernel_decode(native, a) != 2)
        return -1;
    for (unsigned j = 0; j < 2; j++) {
        if (a[j].type != 1 || a[j].bytes < 8191)
            return -2;
        const uint8_t *io = a[j].address;
        for (unsigned i = 0; i < 8191; i++)
            if (io[i] != (uint8_t)(i * 13 + 7))
                return -3;
    }
    return 16382;
}
