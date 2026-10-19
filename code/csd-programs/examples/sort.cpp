/* Device CSF: bounded in-memory runs, spill/reentry, then descending merge. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "batch_sort.h"
#include <algorithm>
#include <functional>
#include <cstring>

namespace {
struct Run {
    uint64_t blocks[SORT_BLOCKS_PER_RUN];
    int32_t *buffer;
    unsigned block, position;
};
struct SortState {
    cs_work_context *context;
    int32_t *scratch;
    Run runs[SORT_MAX_RUNS];
    unsigned count, storeBlock, loadingRun, freeRun, freeBlock;
    bool loading;
};
SortState *state(femu_csd_args *args) { return reinterpret_cast<SortState *>(args->cparam1); }
constexpr unsigned BlockValues = SORT_BLOCK_BYTES / sizeof(int32_t);
}

extern "C" int64_t sort_create(femu_csd_args *)
{
    auto *context = cs_work_begin();
    auto *s = context ? static_cast<SortState *>(cs_work_calloc(context, 1, sizeof(SortState))) : nullptr;
    if (!s) { if (context) cs_work_end(context); return -1; }
    s->context = context;
    s->scratch = static_cast<int32_t *>(cs_work_alloc(context, SORT_CHUNK_BYTES));
    if (!s->scratch) { cs_work_end(context); return -1; }
    return reinterpret_cast<int64_t>(s);
}

// MR[0]: one 4 MiB input chunk. MR[1]: 256 KiB spill I/O. cparam1: state handle.
extern "C" int64_t sort_sink(femu_csd_args *args)
{
    auto *s = state(args);
    if (!s || args->numr != 2 || args->mr_len[0] < SORT_CHUNK_BYTES ||
        args->mr_len[1] < SORT_BLOCK_BYTES || s->count >= SORT_MAX_RUNS) return -1;
    if (!csIsReentry(args)) {
        std::memcpy(s->scratch, args->mr_addr[0], SORT_CHUNK_BYTES);
        // std::sort works in place; its bounded stack does not allocate another large array.
        std::sort(s->scratch, s->scratch + SORT_CHUNK_BYTES / sizeof(int32_t), std::greater<int32_t>());
        s->storeBlock = 0;
    } else {
        // STORE completed: record the allocated spill token, then continue this run.
        s->runs[s->count].blocks[s->storeBlock++] = csReentryValue(args);
    }
    if (s->storeBlock == SORT_BLOCKS_PER_RUN) return ++s->count;
    std::memcpy(args->mr_addr[1], s->scratch + s->storeBlock * BlockValues, SORT_BLOCK_BYTES);
    return csNeedStore(SORT_BLOCK_BYTES);
}

// MR[0]: descending sorted_chunk output. MR[1]: spill I/O, always the last AFDM.
extern "C" int64_t sort_source(femu_csd_args *args)
{
    auto *s = state(args);
    if (!s || !s->count || args->numr != 2 || args->mr_len[0] < static_cast<long long>(sizeof(sorted_chunk)) ||
        args->mr_len[1] < SORT_BLOCK_BYTES) return -1;
    auto *out = static_cast<sorted_chunk *>(args->mr_addr[0]);
    if (!csIsReentry(args)) out->count = 0;
    else {
        // LOAD completed: only the selected run's next block was brought into I/O AFDM.
        auto &run = s->runs[s->loadingRun];
        std::memcpy(run.buffer, args->mr_addr[1], SORT_BLOCK_BYTES);
        run.position = 0;
        s->loading = false;
    }
    while (out->count < SORT_OUTPUT_VALUES) {
        int best = -1;
        for (unsigned i = 0; i < s->count; ++i) {
            auto &run = s->runs[i];
            if (run.buffer && run.position == BlockValues) {
                ++run.block;
                run.position = 0;
                // The buffer must be refilled before considering the next head.
                if (run.block < SORT_BLOCKS_PER_RUN) {
                    s->loadingRun = i; s->loading = true;
                    return csNeedLoad(run.blocks[run.block]);
                }
            }
            if (run.block == SORT_BLOCKS_PER_RUN) continue;
            if (!run.buffer) {
                run.buffer = static_cast<int32_t *>(cs_work_alloc(s->context, SORT_BLOCK_BYTES));
                if (!run.buffer) return -2;
                s->loadingRun = i; s->loading = true;
                return csNeedLoad(run.blocks[0]);
            }
            if (best < 0 || run.buffer[run.position] > s->runs[best].buffer[s->runs[best].position]) best = i;
        }
        if (best < 0) break;
        auto &run = s->runs[best];
        out->values[out->count++] = run.buffer[run.position++];
    }
    return out->count;
}

// Normal teardown uses the same last-AFDM convention to FREE every stored block.
extern "C" int64_t sort_destroy(femu_csd_args *args)
{
    auto *s = state(args);
    if (!s || args->numr != 1 || s->loading) return -1;
    if (!csIsReentry(args)) s->freeRun = s->freeBlock = 0;
    else if (++s->freeBlock == SORT_BLOCKS_PER_RUN) { s->freeBlock = 0; ++s->freeRun; }
    if (s->freeRun < s->count) return csNeedFree(s->runs[s->freeRun].blocks[s->freeBlock]);
    return cs_work_end(s->context);
}

// Error teardown drops private memory; closing the device reclaims remaining spill tokens.
extern "C" int64_t sort_abort(femu_csd_args *args)
{
    auto *s = state(args);
    return s ? cs_work_end(s->context) : -1;
}
