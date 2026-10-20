#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "merge_path_sort.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>
#include <vector>
using Function = int64_t (*)(femu_csd_args *);
struct Spill {
    std::mutex mutex;
    std::vector<std::vector<unsigned char>> blocks{1};
    uint64_t stores = 0, loads = 0, frees = 0;
    int64_t call(Function fn, femu_csd_args &a)
    {
        a.cparam2 = 0;
        for (;;) {
            int64_t value = fn(&a);
            assert(value >= 0);
            auto action = static_cast<uint64_t>(value) & ~CS_IO_VALUE_MASK;
            auto payload = static_cast<uint64_t>(value) & CS_IO_VALUE_MASK;
            auto *io = static_cast<unsigned char *>(a.mr_addr[a.numr - 1]);
            std::lock_guard<std::mutex> lock(mutex);
            if (action == CS_NEED_STORE) {
                assert(payload == SORT_BLOCK_BYTES);
                blocks.emplace_back(io, io + payload); a.cparam2 = blocks.size() - 1; ++stores;
            } else if (action == CS_NEED_LOAD) {
                assert(payload < blocks.size() && blocks[payload].size() == SORT_BLOCK_BYTES);
                std::memcpy(io, blocks[payload].data(), SORT_BLOCK_BYTES); a.cparam2 = payload; ++loads;
            } else if (action == CS_NEED_FREE) {
                assert(payload < blocks.size() && !blocks[payload].empty());
                blocks[payload].clear(); a.cparam2 = payload; ++frees;
            } else return value;
        }
    }
};
static void empty()
{
    cs_work_stats stats;
    assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
static void intersections()
{
    std::mt19937 random(42);
    for (unsigned trial = 0; trial < 400; ++trial) {
        unsigned count = 1 + random() % SORT_MAX_RUNS, length = 1 + random() % 80;
        std::vector<std::vector<int32_t>> runs(count, std::vector<int32_t>(length));
        std::vector<int32_t> all, prefix;
        for (auto &run : runs) {
            for (auto &v : run) v = trial % 3 ? static_cast<int32_t>(random() % 11) - 5 : 7;
            if (trial % 7 == 0) { run.front() = INT32_MAX; run.back() = INT32_MIN; }
            std::sort(run.begin(), run.end(), std::greater<int32_t>());
            all.insert(all.end(), run.begin(), run.end());
        }
        std::sort(all.begin(), all.end(), std::greater<int32_t>());
        for (unsigned rank : {0U, static_cast<unsigned>(all.size() / 2), static_cast<unsigned>(all.size())}) {
            merge_path_partition p{}; p.count = count; p.remaining = rank;
            unsigned run, position;
            while (merge_path_probe(&p, length, &run, &position)) {
                assert(run < count && position < length);
                merge_path_consider(&p, runs[run][position]);
            }
            prefix.clear();
            for (unsigned i = 0; i < count; ++i)
                prefix.insert(prefix.end(), runs[i].begin(), runs[i].begin() + p.cut[i]);
            std::sort(prefix.begin(), prefix.end(), std::greater<int32_t>());
            assert(prefix.size() == rank && std::equal(prefix.begin(), prefix.end(), all.begin()));
        }
    }
}
static void scenario(Function sink, Function source, Function avg, unsigned chunks, unsigned mode)
{
    Spill spill;
    sort_sink_task tasks[SORT_WORKERS]{};
    sort_run_group group{}; group.count = SORT_WORKERS;
    std::vector<std::vector<int32_t>> input(SORT_WORKERS, std::vector<int32_t>(SORT_CHUNK_BYTES / 4));
    std::vector<std::vector<unsigned char>> io(SORT_WORKERS, std::vector<unsigned char>(SORT_BLOCK_BYTES));
    std::vector<int32_t> expected;
    auto value = [mode](unsigned lane, unsigned chunk, unsigned i) {
        return mode == 1 ? 7 : mode == 2 ? (i % 13 ? INT32_MAX : INT32_MIN)
            : static_cast<int32_t>(i * 2654435761U + lane * 19 + chunk);
    };
    std::thread sinks[SORT_WORKERS];
    for (unsigned lane = 0; lane < SORT_WORKERS; ++lane) {
        tasks[lane].workers = SORT_WORKERS;
        for (unsigned chunk = 0; chunk < chunks; ++chunk)
            for (unsigned i = 0; i < input[lane].size(); ++i) expected.push_back(value(lane, chunk, i));
        sinks[lane] = std::thread([&, lane] {
            void *addresses[] = {input[lane].data(), &tasks[lane], io[lane].data()};
            long long lengths[] = {SORT_CHUNK_BYTES, sizeof(tasks[lane]), SORT_BLOCK_BYTES};
            femu_csd_args a{}; a.numr = 3; a.mr_addr = addresses; a.mr_len = lengths;
            for (unsigned chunk = 0; chunk < chunks; ++chunk) {
                for (unsigned i = 0; i < input[lane].size(); ++i) input[lane][i] = value(lane, chunk, i);
                assert(spill.call(sink, a) == static_cast<int64_t>(tasks[lane].state));
            }
        });
    }
    for (auto &thread : sinks) thread.join();
    for (unsigned i = 0; i < SORT_WORKERS; ++i) group.states[i] = tasks[i].state;
    std::sort(expected.begin(), expected.end(), std::greater<int32_t>());
    std::unique_ptr<sorted_chunk> output[2] = {std::make_unique<sorted_chunk>(), std::make_unique<sorted_chunk>()};
    merge_path_task path[2]{}; path[1].part = 1;
    average_result results[2]{};
    size_t emitted[2]{};
    uint64_t root = group.states[0];
    auto consume = [&](unsigned part) {
        void *addresses[] = {output[part].get(), &group, &path[part], io[part].data()};
        long long lengths[] = {sizeof(sorted_chunk), sizeof(group), sizeof(path[part]), SORT_BLOCK_BYTES};
        femu_csd_args a{}; a.numr = 4; a.mr_addr = addresses; a.mr_len = lengths; a.cparam1 = root;
        spill.call(source, a);
        auto offset = expected.size() / 2 * part + emitted[part];
        assert(offset + output[part]->count <= expected.size());
        assert(std::equal(output[part]->values, output[part]->values + output[part]->count, expected.begin() + offset));
        emitted[part] += output[part]->count;
        void *avg_addresses[] = {output[part].get(), &results[part]};
        long long avg_lengths[] = {sizeof(sorted_chunk), sizeof(average_result)};
        femu_csd_args b{}; b.numr = 2; b.mr_addr = avg_addresses; b.mr_len = avg_lengths;
        avg(&b);
    };
    consume(0); // Buffer preparation and the exact midpoint precede parallel reads.
    if (mode == 3) {
        femu_csd_args a{}; a.numr = 1; a.cparam1 = root;
        assert(source(&a) == 0); empty(); return;
    }
    std::thread sources[2];
    for (unsigned part = 0; part < 2; ++part) sources[part] = std::thread([&, part] {
        if (mode == 2 && !part) std::this_thread::sleep_for(std::chrono::milliseconds(300));
        while (!results[part].finished) consume(part);
    });
    for (auto &thread : sources) thread.join();
    assert(emitted[0] == expected.size() / 2 && emitted[1] == expected.size() - expected.size() / 2);
    assert(results[0].sum + results[1].sum == std::accumulate(expected.begin(), expected.end(), int64_t{0}));
    assert(results[0].count + results[1].count == expected.size());
    assert(results[0].metrics.peak_bytes <= SORT_MEMORY_BYTES && results[1].metrics.peak_bytes <= SORT_MEMORY_BYTES);
    assert(results[0].metrics.spill_read_bytes + results[1].metrics.spill_read_bytes == spill.loads * SORT_BLOCK_BYTES);
    assert(results[0].metrics.spill_write_bytes == spill.stores * SORT_BLOCK_BYTES && spill.stores == spill.frees);
    for (auto handle : group.states) assert(!handle);
    empty();
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    intersections();
    assert(!cs_work_init(SORT_MEMORY_BYTES));
    void *sort = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL), *average = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    assert(sort && average);
    auto sink = reinterpret_cast<Function>(dlsym(sort, "sort_sink"));
    auto source = reinterpret_cast<Function>(dlsym(sort, "sort_source"));
    auto avg = reinterpret_cast<Function>(dlsym(average, "avg_int32"));
    assert(sink && source && avg);
    scenario(sink, source, avg, 4, 0);
    scenario(sink, source, avg, 1, 1);
    scenario(sink, source, avg, 1, 2);
    scenario(sink, source, avg, 1, 3);
    dlclose(average); dlclose(sort);
    puts("PASS: exact k-way rank cuts, ties/skew/extrema, two concurrent sources, full ordering/AVG, spill and cleanup");
}
