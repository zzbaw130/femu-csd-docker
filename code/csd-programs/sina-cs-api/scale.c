/* Device program: AFDM contains only declared inputs and outputs. */
#include "cs_kernel.h"
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
