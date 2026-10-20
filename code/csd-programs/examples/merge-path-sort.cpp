/* Four independent sinks, then two disjoint output ranks of a k-way merge. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "merge_path_sort.h"
#include <algorithm>
#include <cstring>
namespace {
constexpr unsigned RunValues = SORT_CHUNK_BYTES / sizeof(int32_t);
constexpr unsigned BlockValues = SORT_BLOCK_BYTES / sizeof(int32_t);
enum class IO { None, Store, Load, Free };
struct Run {
    int32_t *data;
    cs_work_context *owner;
    uint64_t blocks[SORT_BLOCKS_PER_RUN];
    bool spilled;
};
struct Source {
    int32_t *buffers[SORT_MAX_RUNS];
    unsigned position[SORT_MAX_RUNS], end[SORT_MAX_RUNS], loaded[SORT_MAX_RUNS];
    sort_heap heap;
    unsigned initialized, ioRun, ioBlock;
    uint64_t emitted, target, readBytes;
    IO pending;
    bool rootPending, reported;
    sort_timing time;
};
struct State {
    cs_work_context *context, *owners[SORT_WORKERS];
    unsigned count, ownerCount, ioRun, ioBlock, completed;
    IO pending;
    bool combined, prepared, ready;
    Run runs[SORT_MAX_RUNS];
    Source sources[MERGE_PATH_WORKERS];
    merge_path_partition partition;
    sort_metrics metrics;
    uint64_t prepareStart, prepareNs, partitionStart, partitionNs, partitionReadBytes;
};
void destroy(State &s)
{
    for (unsigned i = 1; i < s.ownerCount; ++i) cs_work_end(s.owners[i]);
    cs_work_end(s.context);
}
int resident(State &s, cs_work_context *owner)
{
    for (unsigned i = 0; i < s.count; ++i)
        if (s.runs[i].owner == owner && !s.runs[i].spilled) return i;
    return -1;
}
int64_t store(State &s, femu_csd_args &a)
{
    std::memcpy(a.mr_addr[a.numr - 1], s.runs[s.ioRun].data + s.ioBlock * BlockValues, SORT_BLOCK_BYTES);
    return csNeedStore(SORT_BLOCK_BYTES);
}
int64_t spill(State &s, unsigned index, femu_csd_args &a)
{
    s.ioRun = index; s.ioBlock = 0; s.pending = IO::Store;
    return store(s, a);
}
int64_t resumeStore(State &s, femu_csd_args &a)
{
    if (!csIsReentry(&a)) return 0;
    if (s.pending != IO::Store) return -1;
    auto &r = s.runs[s.ioRun];
    r.blocks[s.ioBlock++] = csReentryValue(&a);
    s.metrics.spill_write_bytes += SORT_BLOCK_BYTES;
    if (s.ioBlock < SORT_BLOCKS_PER_RUN) return store(s, a);
    cs_work_free(r.owner, r.data); r.data = nullptr; r.spilled = true;
    ++s.metrics.spilled_runs; s.pending = IO::None;
    return 0;
}
int64_t timed(sort_timing &time, sort_timing start, int64_t result)
{
    sort_record(&time, start); return result;
}
int combine(State &s, sort_run_group &group)
{
    if (group.count != SORT_WORKERS || group.states[0] != reinterpret_cast<uint64_t>(&s)) return -1;
    unsigned count = s.count;
    for (unsigned i = 1; i < SORT_WORKERS; ++i) {
        auto *other = reinterpret_cast<State *>(group.states[i]);
        if (!other || other->combined || other->pending != IO::None ||
            (count += other->count) > SORT_MAX_RUNS) return -1;
        for (unsigned j = 0; j < i; ++j) if (group.states[i] == group.states[j]) return -1;
    }
    for (unsigned i = 1; i < SORT_WORKERS; ++i) {
        auto *other = reinterpret_cast<State *>(group.states[i]);
        std::copy(other->runs, other->runs + other->count, s.runs + s.count);
        s.count += other->count;
        s.owners[s.ownerCount++] = other->context;
        s.metrics.spill_write_bytes += other->metrics.spill_write_bytes;
        s.metrics.spilled_runs += other->metrics.spilled_runs;
        s.metrics.sink.wall_ns += other->metrics.sink.wall_ns;
        s.metrics.sink.cpu_ns += other->metrics.sink.cpu_ns;
        s.metrics.run_sort_ns += other->metrics.run_sort_ns;
        group.states[i] = 0;
    }
    s.combined = true; s.prepareStart = sort_clock().wall_ns;
    return 0;
}
int64_t prepare(State &s, femu_csd_args &a)
{
    // Only source 0 runs here. Afterwards runs are immutable until both sources finish.
    for (unsigned i = 0; i < s.count; ++i) if (s.runs[i].spilled)
        for (auto &source : s.sources) if (!source.buffers[i]) {
            source.buffers[i] = static_cast<int32_t *>(cs_work_alloc(s.runs[i].owner, SORT_BLOCK_BYTES));
            if (!source.buffers[i]) {
                int victim = resident(s, s.runs[i].owner);
                return victim >= 0 ? spill(s, victim, a) : -2;
            }
            source.loaded[i] = SORT_BLOCKS_PER_RUN;
        }
    s.prepared = true;
    s.prepareNs = sort_clock().wall_ns - s.prepareStart;
    s.metrics.peak_bytes = merge_path_buffer_bytes();
    for (unsigned i = 0; i < s.ownerCount; ++i) {
        cs_work_context_stats stats;
        if (cs_work_get_context_stats(s.owners[i], &stats)) return -1;
        s.metrics.peak_bytes += stats.peak_used_bytes;
    }
    s.partition.count = s.count;
    s.partition.remaining = static_cast<uint64_t>(s.count) * RunValues / 2;
    s.partitionStart = sort_clock().wall_ns;
    return 0;
}
int64_t loadValue(State &s, Source &source, unsigned index, unsigned position,
                  int32_t &value)
{
    const auto &r = s.runs[index];
    if (!r.spilled) { value = r.data[position]; return 0; }
    unsigned block = position / BlockValues;
    if (source.loaded[index] != block) {
        source.ioRun = index; source.ioBlock = block; source.pending = IO::Load;
        return csNeedLoad(r.blocks[block]);
    }
    value = source.buffers[index][position % BlockValues];
    return 0;
}
int resumeSource(State &s, Source &source, femu_csd_args &a)
{
    if (!csIsReentry(&a) || source.pending == IO::None) return 0;
    if (source.pending == IO::Load) {
        std::memcpy(source.buffers[source.ioRun], a.mr_addr[a.numr - 1], SORT_BLOCK_BYTES);
        source.loaded[source.ioRun] = source.ioBlock;
        source.readBytes += SORT_BLOCK_BYTES;
        if (!s.ready) s.partitionReadBytes += SORT_BLOCK_BYTES;
    } else if (source.pending == IO::Free) {
        s.runs[source.ioRun].blocks[source.ioBlock] = 0;
    } else return -1;
    source.pending = IO::None; return 0;
}
int64_t partition(State &s)
{
    unsigned run, position;
    while (merge_path_probe(&s.partition, RunValues, &run, &position)) {
        int32_t value;
        int64_t result = loadValue(s, s.sources[0], run, position, value);
        if (result) return result;
        merge_path_consider(&s.partition, value);
    }
    uint64_t total = static_cast<uint64_t>(s.count) * RunValues;
    for (unsigned i = 0; i < s.count; ++i) {
        s.sources[0].end[i] = s.partition.cut[i];
        s.sources[1].position[i] = s.partition.cut[i];
        s.sources[1].end[i] = RunValues;
    }
    s.sources[0].target = total / 2;
    s.sources[1].target = total - total / 2;
    s.partitionNs = sort_clock().wall_ns - s.partitionStart;
    s.ready = true; return 0;
}
void resultMetrics(State &s, Source &source, sorted_chunk &out)
{
    out.metrics = s.metrics;
    out.metrics.runs = s.count;
    out.metrics.source = source.time;
    out.metrics.spill_read_bytes = source.readBytes;
}
}

// MR: input, task(state output), spill I/O. Each branch owns its task and state.
extern "C" int64_t sort_sink(femu_csd_args *a)
{
    auto start = sort_clock();
    if (a->numr != 3 || a->mr_len[0] < SORT_CHUNK_BYTES ||
        a->mr_len[1] < static_cast<long long>(sizeof(sort_sink_task)) || a->mr_len[2] < SORT_BLOCK_BYTES) return -1;
    auto &task = *static_cast<sort_sink_task *>(a->mr_addr[1]);
    if (task.workers != SORT_WORKERS) return -1;
    auto *s = reinterpret_cast<State *>(task.state);
    if (!s) {
        auto *ctx = cs_work_begin_limit(merge_path_work_bytes() / SORT_WORKERS);
        s = ctx ? static_cast<State *>(cs_work_calloc(ctx, 1, sizeof(State))) : nullptr;
        if (!s) { if (ctx) cs_work_end(ctx); return -2; }
        s->context = s->owners[0] = ctx; s->ownerCount = 1;
        task.state = reinterpret_cast<uint64_t>(s);
    }
    int64_t result = resumeStore(*s, *a);
    if (result) return timed(s->metrics.sink, start, result);
    if (s->count == SORT_MAX_RUNS) return -1;
    auto *data = static_cast<int32_t *>(cs_work_alloc(s->context, SORT_CHUNK_BYTES));
    if (!data) {
        int victim = resident(*s, s->context);
        return timed(s->metrics.sink, start, victim >= 0 ? spill(*s, victim, *a) : -2);
    }
    std::memcpy(data, a->mr_addr[0], SORT_CHUNK_BYTES);
    auto sortStart = sort_clock();
    sort_int32_desc(data, RunValues);
    s->metrics.run_sort_ns += sort_clock().wall_ns - sortStart.wall_ns;
    s->runs[s->count].data = data; s->runs[s->count++].owner = s->context;
    return timed(s->metrics.sink, start, reinterpret_cast<int64_t>(s));
}

// MR: output, shared group, task(part 0/1), spill I/O. cparam1=group.states[0].
// Prime source 0 once before submitting both branches. numr=1 is error cleanup.
extern "C" int64_t sort_source(femu_csd_args *a)
{
    auto start = sort_clock();
    auto *s = reinterpret_cast<State *>(a->cparam1);
    if (!s) return -1;
    if (a->numr == 1) { destroy(*s); return 0; }
    if (a->numr != 4 || a->mr_len[0] < static_cast<long long>(sizeof(sorted_chunk)) ||
        a->mr_len[1] < static_cast<long long>(sizeof(sort_run_group)) ||
        a->mr_len[2] < static_cast<long long>(sizeof(merge_path_task)) || a->mr_len[3] < SORT_BLOCK_BYTES) return -1;
    auto &group = *static_cast<sort_run_group *>(a->mr_addr[1]);
    auto &task = *static_cast<merge_path_task *>(a->mr_addr[2]);
    unsigned part = task.part;
    if (part >= MERGE_PATH_WORKERS || (!s->ready && part)) return -1;
    if (!s->combined && combine(*s, group)) return -1;
    auto &source = s->sources[part];
    auto &out = *static_cast<sorted_chunk *>(a->mr_addr[0]);
    if (!csIsReentry(a)) out.count = out.finished = 0;
    int64_t result;
    if (!s->prepared) {
        result = resumeStore(*s, *a);
        if (result) return timed(source.time, start, result);
        result = prepare(*s, *a);
        if (result) return timed(source.time, start, result);
    }
    if (resumeSource(*s, source, *a)) return -1;
    if (!s->ready) {
        result = partition(*s);
        if (result) return timed(source.time, start, result);
    }
    task.prepare_ns = s->prepareNs; task.partition_ns = s->partitionNs;
    task.partition_read_bytes = s->partitionReadBytes;
    while (source.initialized < s->count) {
        unsigned i = source.initialized;
        if (source.position[i] < source.end[i]) {
            int32_t value;
            result = loadValue(*s, source, i, source.position[i], value);
            if (result) return timed(source.time, start, result);
            sort_heap_push(&source.heap, {value, i});
        }
        ++source.initialized;
    }
    while (out.count < SORT_OUTPUT_VALUES && source.emitted < source.target) {
        unsigned i = source.heap.entries[0].run;
        if (source.rootPending) {
            if (source.position[i] == source.end[i]) sort_heap_pop(&source.heap);
            else {
                int32_t value;
                result = loadValue(*s, source, i, source.position[i], value);
                if (result) return timed(source.time, start, result);
                sort_heap_replace(&source.heap, {value, i});
            }
            source.rootPending = false; continue;
        }
        out.values[out.count++] = source.heap.entries[0].value;
        ++source.position[i]; ++source.emitted; source.rootPending = true;
    }
    if (source.emitted != source.target) return timed(source.time, start, out.count);
    if (!source.reported) {
        sort_record(&source.time, start);
        resultMetrics(*s, source, out); out.finished = 1;
        source.reported = true;
        unsigned mask = __atomic_fetch_or(&s->completed, 1U << part, __ATOMIC_ACQ_REL) | (1U << part);
        // Publish after our last shared-state access. The other source may now destroy it.
        if (mask != (1U << MERGE_PATH_WORKERS) - 1) return out.count;
        out.finished = 0; start = sort_clock();
    }
    // Only the last source enters here, including its FREE reentries.
    for (unsigned i = 0; i < s->count; ++i)
        for (unsigned block = 0; block < SORT_BLOCKS_PER_RUN; ++block)
            if (s->runs[i].blocks[block]) {
                source.ioRun = i; source.ioBlock = block; source.pending = IO::Free;
                return timed(source.time, start, csNeedFree(s->runs[i].blocks[block]));
            }
    sort_record(&source.time, start); resultMetrics(*s, source, out);
    destroy(*s); group.states[0] = 0; out.finished = 1;
    return out.count;
}
