/* Device sort: keep runs in workmem; evict only after allocation fails. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "batch_sort.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr unsigned RunValues = SORT_CHUNK_BYTES / sizeof(int32_t);
constexpr unsigned BlockValues = SORT_BLOCK_BYTES / sizeof(int32_t);
struct Run {
    int32_t *data;                     // Whole resident run, or one spilled-run read block.
    cs_work_context *owner;
    uint64_t blocks[SORT_BLOCKS_PER_RUN];
    unsigned position, loadedBlock;
    bool spilled;
};
enum class IO { None, Store, Load, Free };
struct SortState {
    cs_work_context *context;
    cs_work_context *owners[SORT_WORKERS];
    unsigned ownerCount, workers;
    bool combined;
    Run runs[SORT_MAX_RUNS];
    unsigned count, ioRun, ioBlock;
    uint64_t emitted;
    IO pending;
    bool finishing;
    sort_metrics metrics;
    sort_heap heap;
    unsigned initialized;
    bool root_pending;
};
SortState *state(femu_csd_args *args) { return reinterpret_cast<SortState *>(args->cparam1); }
int64_t timed_result(SortState &s, sort_timing start, int64_t result, bool source)
{
    sort_record(source ? &s.metrics.source : &s.metrics.sink, start);
    return result;
}
int resident(const SortState &s, cs_work_context *owner)
{
    for (unsigned i = 0; i < s.count; ++i)
        if (s.runs[i].owner == owner && !s.runs[i].spilled && s.runs[i].data) return i;
    return -1;
}
int32_t head(const Run &run)
{
    return run.data[run.spilled ? run.position % BlockValues : run.position];
}
int64_t store_block(SortState &s, femu_csd_args &args)
{
    std::memcpy(args.mr_addr[args.numr - 1], s.runs[s.ioRun].data + s.ioBlock * BlockValues, SORT_BLOCK_BYTES);
    return csNeedStore(SORT_BLOCK_BYTES);
}
int64_t spill(SortState &s, unsigned run, femu_csd_args &args)
{
    s.ioRun = run; s.ioBlock = 0; s.pending = IO::Store;
    return store_block(s, args);
}
int64_t load_head(SortState &s, unsigned index, femu_csd_args &args)
{
    auto &run = s.runs[index];
    if (!run.spilled) return 0;
    if (!run.data) {
        run.data = static_cast<int32_t *>(cs_work_alloc(run.owner, SORT_BLOCK_BYTES));
        if (!run.data) {
            int victim = resident(s, run.owner);
            return victim >= 0 ? spill(s, victim, args) : -2;
        }
        run.loadedBlock = SORT_BLOCKS_PER_RUN;
    }
    unsigned block = run.position / BlockValues;
    if (run.loadedBlock == block) return 0;
    s.ioRun = index; s.ioBlock = block; s.pending = IO::Load;
    return csNeedLoad(run.blocks[block]);
}
// Reentry only acknowledges the I/O requested by the previous call.
int64_t resume(SortState &s, femu_csd_args &args)
{
    if (!csIsReentry(&args)) return 0;
    auto &run = s.runs[s.ioRun];
    switch (s.pending) {
    case IO::Store:
        s.metrics.spill_write_bytes += SORT_BLOCK_BYTES;
        run.blocks[s.ioBlock++] = csReentryValue(&args);
        if (s.ioBlock < SORT_BLOCKS_PER_RUN) return store_block(s, args);
        cs_work_free(run.owner, run.data);
        run.data = nullptr;
        run.spilled = true;
        ++s.metrics.spilled_runs;
        break;
    case IO::Load:
        s.metrics.spill_read_bytes += SORT_BLOCK_BYTES;
        std::memcpy(run.data, args.mr_addr[args.numr - 1], SORT_BLOCK_BYTES);
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
    return 0; // Caller records final timings and destroys the context.
}
}

// Legacy: input, io; cparam1=state. Parallel: input, task(handle output), io.
extern "C" int64_t sort_sink(femu_csd_args *args)
{
    auto start = sort_clock();
    if ((args->numr != 2 && args->numr != 3) || args->mr_len[0] < SORT_CHUNK_BYTES ||
        args->mr_len[args->numr - 1] < SORT_BLOCK_BYTES) return -1;
    auto *task = args->numr == 3 ? static_cast<sort_sink_task *>(args->mr_addr[1]) : nullptr;
    if (task && (args->mr_len[1] < static_cast<long long>(sizeof(*task)) ||
                 !task->workers || task->workers > SORT_WORKERS)) return -1;
    auto *s = task ? reinterpret_cast<SortState *>(task->state) : state(args);
    bool first = !s;
    if (first) {
        unsigned workers = task ? task->workers : 1;
        cs_work_stats stats;
        if (cs_work_get_stats(&stats)) return -1;
        auto *context = cs_work_begin_limit(std::min(stats.capacity_bytes, sort_work_bytes(workers)) / workers);
        s = context ? static_cast<SortState *>(cs_work_calloc(context, 1, sizeof(SortState))) : nullptr;
        if (!s) { if (context) cs_work_end(context); return -1; }
        s->context = s->owners[0] = context;
        s->ownerCount = 1;
        s->workers = workers;
        if (task) task->state = reinterpret_cast<uint64_t>(s);
    }
    if (s->count == SORT_MAX_RUNS) return -1;
    int64_t result = resume(*s, *args);
    if (result) return timed_result(*s, start, result, false);
    auto *data = static_cast<int32_t *>(cs_work_alloc(s->context, SORT_CHUNK_BYTES));
    if (!data) {
        int victim = resident(*s, s->context);
        if (victim >= 0) return timed_result(*s, start, spill(*s, victim, *args), false);
        if (first) { cs_work_end(s->context); if (task) task->state = 0; }
        return -2; // At least one run must fit to perform an in-place sort.
    }
    std::memcpy(data, args->mr_addr[0], SORT_CHUNK_BYTES);
    auto sort_start = sort_clock();
    sort_int32_desc(data, RunValues);
    s->metrics.run_sort_ns += sort_clock().wall_ns - sort_start.wall_ns;
    s->runs[s->count].owner = s->context;
    s->runs[s->count++].data = data;
    return timed_result(*s, start, reinterpret_cast<int64_t>(s), false);
}

namespace {
void destroy(SortState &s)
{
    for (unsigned i = 1; i < s.ownerCount; ++i) cs_work_end(s.owners[i]);
    cs_work_end(s.context);
}
int combine(SortState &s, sort_run_group &group)
{
    if (!group.count || group.count > SORT_WORKERS || group.states[0] != reinterpret_cast<uint64_t>(&s)) return -1;
    unsigned total = s.count;
    for (unsigned i = 1; i < group.count; ++i) {
        auto *other = reinterpret_cast<SortState *>(group.states[i]);
        if (!other || other == &s || other->pending != IO::None || other->combined ||
            other->workers != s.workers || (total += other->count) > SORT_MAX_RUNS) return -1;
        for (unsigned j = 0; j < i; ++j) if (group.states[i] == group.states[j]) return -1;
    }
    for (unsigned i = 1; i < group.count; ++i) {
        auto *other = reinterpret_cast<SortState *>(group.states[i]);
        std::copy(other->runs, other->runs + other->count, s.runs + s.count);
        s.count += other->count;
        s.owners[s.ownerCount++] = other->context;
        s.metrics.spill_write_bytes += other->metrics.spill_write_bytes;
        s.metrics.spilled_runs += other->metrics.spilled_runs;
        s.metrics.sink.wall_ns += other->metrics.sink.wall_ns;
        s.metrics.sink.cpu_ns += other->metrics.sink.cpu_ns;
        s.metrics.run_sort_ns += other->metrics.run_sort_ns;
        group.states[i] = 0; // Ownership moved to source; error cleanup must not free twice.
    }
    s.combined = true;
    return 0;
}
}

// Legacy: output, io. Parallel: output, group, io; cparam1=group.states[0].
// numr=1 drops this state and every state adopted by the source.
extern "C" int64_t sort_source(femu_csd_args *args)
{
    auto start = sort_clock();
    auto *s = state(args);
    if (!s) return -1;
    if (args->numr == 1) { destroy(*s); return 0; }
    if ((args->numr != 2 && args->numr != 3) || args->mr_len[0] < static_cast<long long>(sizeof(sorted_chunk)) ||
        args->mr_len[args->numr - 1] < SORT_BLOCK_BYTES) return -1;
    auto *group = args->numr == 3 ? static_cast<sort_run_group *>(args->mr_addr[1]) : nullptr;
    if (group && (args->mr_len[1] < static_cast<long long>(sizeof(*group)) ||
                  (!s->combined && combine(*s, *group)))) return -1;
    auto *out = static_cast<sorted_chunk *>(args->mr_addr[0]);
    if (!csIsReentry(args)) out->count = out->finished = 0;
    int64_t result = resume(*s, *args);
    if (result) return timed_result(*s, start, result, true);
    while (s->initialized < s->count) {
        unsigned i = s->initialized;
        result = load_head(*s, i, *args);
        if (result) return timed_result(*s, start, result, true);
        sort_heap_push(&s->heap, {head(s->runs[i]), i});
        ++s->initialized;
    }
    while (!s->finishing && out->count < SORT_OUTPUT_VALUES) {
        unsigned index = s->heap.entries[0].run;
        auto &run = s->runs[index];
        // A block refill may yield to SNIA. Repair exactly once after reentry,
        // before emitting another value, including across output chunk boundaries.
        if (s->root_pending) {
            if (run.position == RunValues) {
                cs_work_free(run.owner, run.data);
                run.data = nullptr;
                sort_heap_pop(&s->heap);
            } else {
                result = load_head(*s, index, *args);
                if (result) return timed_result(*s, start, result, true);
                sort_heap_replace(&s->heap, {head(run), index});
            }
            s->root_pending = false;
            continue;
        }
        out->values[out->count++] = s->heap.entries[0].value;
        ++run.position;
        s->root_pending = true;
        if (++s->emitted == static_cast<uint64_t>(s->count) * RunValues) s->finishing = true;
    }
    if (s->finishing) {
        s->metrics.peak_bytes = sort_buffer_bytes(s->workers);
        for (unsigned i = 0; i < s->ownerCount; ++i) {
            cs_work_context_stats stats;
            if (cs_work_get_context_stats(s->owners[i], &stats)) return -1;
            s->metrics.peak_bytes += stats.peak_used_bytes;
        }
        s->metrics.runs = s->count;
        result = finish(*s);
        if (result) return timed_result(*s, start, result, true);
        out->metrics = s->metrics;
        destroy(*s);
        if (group) group->states[0] = 0;
        sort_record(&out->metrics.source, start);
        out->finished = 1; // The last chunk remains in AFDM after workmem is destroyed.
        return out->count;
    }
    return timed_result(*s, start, out->count, true);
}
