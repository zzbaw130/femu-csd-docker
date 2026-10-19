/* Native device programs: the caller knows each function's argument layout. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
int64_t cs_scale(struct femu_csd_args *a)
{
    /* MR[0]=input, MR[1]=output; cparam1=count; cparam2=in/out byte offsets. */
    uint64_t count = a->cparam1, in = (uint32_t)a->cparam2,
             out = (uint64_t)a->cparam2 >> 32;
    if (a->numr != 2 || in > (uint64_t)a->mr_len[0] || out > (uint64_t)a->mr_len[1] ||
        count > ((uint64_t)a->mr_len[0] - in) / 4 ||
        count > ((uint64_t)a->mr_len[1] - out) / 4)
        return -1;
    const uint32_t *src = (const void *)((const uint8_t *)a->mr_addr[0] + in);
    uint32_t *dst = (void *)((uint8_t *)a->mr_addr[1] + out);
    for (uint64_t i = 0; i < count; i++)
        dst[i] = src[i] * 2;
    return count;
}
int64_t cs_scalar_sum(struct femu_csd_args *a)
{
    return a->cparam1 + a->cparam2;
}
int64_t cs_delay(struct femu_csd_args *a)
{
    uint64_t off = a->cparam1;
    uint64_t *stamps = a->numr == 1 && off <= (uint64_t)a->mr_len[0] &&
                       (uint64_t)a->mr_len[0] - off >= 16
        ? (void *)((uint8_t *)a->mr_addr[0] + off) : NULL;
    struct timespec now;
    if (stamps) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        stamps[0] = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    }
    struct timespec delay = {.tv_nsec = 100000000};
    nanosleep(&delay, NULL);
    if (stamps) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        stamps[1] = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    }
    return 0;
}
int64_t cs_check_128(struct femu_csd_args *a)
{
    /* A 128-bit input occupies the two existing 64-bit native scalar fields. */
    return a->cparam1 == 7 && a->cparam2 == 3 ? 1 : -1;
}
int64_t cs_result64(struct femu_csd_args *a) { return a->cparam1; }

/* Private state survives reentry on any CU. It never occupies AFDM. The list
 * is specific to these examples; a real sort uses its own query/run object. */
struct spill_state {
    void *io;
    unsigned step;
    cs_work_context *ctx;
    struct spill_state *next;
};
static struct spill_state *states;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t spill_run(struct femu_csd_args *a, int load_only)
{
    if (a->numr != 1 || a->mr_len[0] < 8192)
        return -1;
    uint8_t *io = a->mr_addr[0];
    pthread_mutex_lock(&state_lock);
    struct spill_state **slot = &states;
    while (*slot && (*slot)->io != io)
        slot = &(*slot)->next;
    struct spill_state *state = *slot;
    int64_t result = -2;
    if (!csIsReentry(a)) {
        if (state) { /* Restarting the example also discards its previous private state. */
            *slot = state->next;
            cs_work_end(state->ctx);
        }
        cs_work_context *ctx = cs_work_begin();
        state = ctx ? cs_work_alloc(ctx, sizeof(*state)) : NULL;
        if (!state) {
            if (ctx)
                cs_work_end(ctx);
            goto out;
        }
        *state = (struct spill_state){.io = io, .ctx = ctx, .next = *slot};
        *slot = state;
        if (load_only) {
            result = csNeedLoad(a->cparam1);
        } else {
            for (unsigned i = 0; i < 8191; i++)
                io[i] = (uint8_t)(i * 13 + 7);
            io[8191] = 0;
            result = csNeedStore(8191);
        }
    } else if (state) {
        uint64_t id = csReentryValue(a);
        if (!load_only && state->step == 0) {
            state->step = 1;
            memset(io, 0, 8192);
            result = csNeedLoad(id);
        } else if (state->step == (load_only ? 0U : 1U)) {
            state->step = 2;
            for (unsigned i = 0; i < 8191; i++)
                if (io[i] != (load_only ? 0x5a : (uint8_t)(i * 13 + 7))) {
                    *slot = state->next;
                    cs_work_end(state->ctx);
                    result = -3;
                    goto out;
                }
            result = csNeedFree(id);
        } else {
            *slot = state->next;
            cs_work_end(state->ctx);
            result = 8191;
        }
    }
out:
    pthread_mutex_unlock(&state_lock);
    return result;
}
int64_t cs_spill_roundtrip(struct femu_csd_args *a) { return spill_run(a, 0); }
int64_t cs_spill_load(struct femu_csd_args *a) { return spill_run(a, 1); }
int64_t cs_spill_store(struct femu_csd_args *a)
{
    if (a->numr != 1 || a->mr_len[0] < 8192)
        return -1;
    if (csIsReentry(a))
        return csReentryValue(a); /* Caller may pass this spill ID to a later source. */
    memset(a->mr_addr[0], 0x5a, 8192);
    return csNeedStore(8191);
}
int64_t cs_spill_fail(struct femu_csd_args *a)
{
    return csIsReentry(a) ? -9 : csNeedStore(8191);
}
int64_t cs_spill_validate(struct femu_csd_args *a)
{
    if (a->numr != 2)
        return -1;
    for (unsigned j = 0; j < 2; j++) {
        if (a->mr_len[j] < 8191)
            return -2;
        const uint8_t *io = a->mr_addr[j];
        for (unsigned i = 0; i < 8191; i++)
            if (io[i] != (uint8_t)(i * 13 + 7))
                return -3;
    }
    return 16382;
}
