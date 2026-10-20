/* Device sort: keep runs in workmem; evict only after allocation fails. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "batch_sort.h"
#include <algorithm>
#include <functional>
#include <cstring>

namespace {
constexpr unsigned RunValues = SORT_CHUNK_BYTES / sizeof(int32_t);
constexpr unsigned BlockValues = SORT_BLOCK_BYTES / sizeof(int32_t);
struct Run {
    int32_t *data;                     // Whole resident run, or one spilled-run read block.
    uint64_t blocks[SORT_BLOCKS_PER_RUN];
    unsigned position, loadedBlock;
    bool spilled;
};
enum class IO { None, Store, Load, Free };
struct SortState {
    cs_work_context *context;
    Run runs[SORT_MAX_RUNS];
    unsigned count, ioRun, ioBlock;
    uint64_t emitted;
    IO pending;
    bool finishing;
};
SortState *state(femu_csd_args *args) { return reinterpret_cast<SortState *>(args->cparam1); }
int resident(const SortState &s)
{
    for (unsigned i = 0; i < s.count; ++i)
        if (!s.runs[i].spilled && s.runs[i].data) return i;
    return -1;
}
int32_t head(const Run &run)
{
    return run.data[run.spilled ? run.position % BlockValues : run.position];
}
int64_t store_block(SortState &s, femu_csd_args &args)
{
    std::memcpy(args.mr_addr[1], s.runs[s.ioRun].data + s.ioBlock * BlockValues, SORT_BLOCK_BYTES);
    return csNeedStore(SORT_BLOCK_BYTES);
}
int64_t spill(SortState &s, unsigned run, femu_csd_args &args)
{
    s.ioRun = run; s.ioBlock = 0; s.pending = IO::Store;
    return store_block(s, args);
}
// Reentry only acknowledges the I/O requested by the previous call.
int64_t resume(SortState &s, femu_csd_args &args)
{
    if (!csIsReentry(&args)) return 0;
    auto &run = s.runs[s.ioRun];
    switch (s.pending) {
    case IO::Store:
        run.blocks[s.ioBlock++] = csReentryValue(&args);
        if (s.ioBlock < SORT_BLOCKS_PER_RUN) return store_block(s, args);
        cs_work_free(s.context, run.data);
        run.data = nullptr;
        run.spilled = true;
        break;
    case IO::Load:
        std::memcpy(run.data, args.mr_addr[1], SORT_BLOCK_BYTES);
        run.loadedBlock = s.ioBlock;
        break;
    case IO::Free:
        run.blocks[s.ioBlock] = 0;
        break;
    default: return -1;
    }
    s.pending = IO::None;
    return 0;
}
int64_t finish(SortState &s)
{
    // Normal EOF releases every retained token before destroying private memory.
    for (unsigned i = 0; i < s.count; ++i)
        for (unsigned j = 0; j < SORT_BLOCKS_PER_RUN; ++j)
            if (s.runs[i].blocks[j]) {
                s.ioRun = i; s.ioBlock = j; s.pending = IO::Free;
                return csNeedFree(s.runs[i].blocks[j]);
            }
    return cs_work_end(s.context);
}
}

// MR[0]=4 MiB input, MR[1]=spill I/O; cparam1=0 first, then the returned state handle.
extern "C" int64_t sort_sink(femu_csd_args *args)
{
    if (args->numr != 2 || args->mr_len[0] < SORT_CHUNK_BYTES || args->mr_len[1] < SORT_BLOCK_BYTES) return -1;
    auto *s = state(args);
    bool first = !s;
    if (first) {
        auto *context = cs_work_begin();
        s = context ? static_cast<SortState *>(cs_work_calloc(context, 1, sizeof(SortState))) : nullptr;
        if (!s) { if (context) cs_work_end(context); return -1; }
        s->context = context;
    }
    if (s->count == SORT_MAX_RUNS) return -1;
    int64_t result = resume(*s, *args);
    if (result) return result;
    auto *data = static_cast<int32_t *>(cs_work_alloc(s->context, SORT_CHUNK_BYTES));
    if (!data) {
        int victim = resident(*s);
        if (victim >= 0) return spill(*s, victim, *args);
        if (first) cs_work_end(s->context);
        return -2; // At least one run must fit to perform an in-place sort.
    }
    std::memcpy(data, args->mr_addr[0], SORT_CHUNK_BYTES);
    std::sort(data, data + RunValues, std::greater<int32_t>());
    s->runs[s->count++].data = data;
    return reinterpret_cast<int64_t>(s);
}

// MR[0]=sorted output, MR[1]=spill I/O. Repeated calls finish and destroy the state at EOF.
// For early stop/error, MR[0]=I/O only (numr=1) drops private memory; device close reclaims spill.
extern "C" int64_t sort_source(femu_csd_args *args)
{
    auto *s = state(args);
    if (!s) return -1;
    if (args->numr == 1) return cs_work_end(s->context);
    if (args->numr != 2 || args->mr_len[0] < static_cast<long long>(sizeof(sorted_chunk)) ||
        args->mr_len[1] < SORT_BLOCK_BYTES) return -1;
    auto *out = static_cast<sorted_chunk *>(args->mr_addr[0]);
    if (!csIsReentry(args)) out->count = out->finished = 0;
    int64_t result = resume(*s, *args);
    if (result) return result;
    while (!s->finishing && out->count < SORT_OUTPUT_VALUES) {
        int best = -1;
        for (unsigned i = 0; i < s->count; ++i) {
            auto &run = s->runs[i];
            if (run.position == RunValues) continue;
            if (run.spilled) {
                if (!run.data) {
                    run.data = static_cast<int32_t *>(cs_work_alloc(s->context, SORT_BLOCK_BYTES));
                    if (!run.data) {
                        int victim = resident(*s);
                        return victim >= 0 ? spill(*s, victim, *args) : -2;
                    }
                    run.loadedBlock = SORT_BLOCKS_PER_RUN;
                }
                unsigned block = run.position / BlockValues;
                if (run.loadedBlock != block) {
                    s->ioRun = i; s->ioBlock = block; s->pending = IO::Load;
                    return csNeedLoad(run.blocks[block]);
                }
            }
            if (best < 0 || head(run) > head(s->runs[best])) best = i;
        }
        if (best < 0) { s->finishing = true; break; }
        auto &run = s->runs[best];
        out->values[out->count++] = head(run);
        if (++run.position == RunValues) {
            cs_work_free(s->context, run.data);
            run.data = nullptr;
        }
        if (++s->emitted == static_cast<uint64_t>(s->count) * RunValues) s->finishing = true;
    }
    if (s->finishing) {
        result = finish(*s);
        if (result) return result;
        out->finished = 1; // The last chunk remains in AFDM after workmem is destroyed.
    }
    return out->count;
}
