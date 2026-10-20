// Exercise resident-only runs, allocation-triggered spill, read-buffer pressure and EOF ownership.
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "batch_sort.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <memory>
#include <numeric>
#include <vector>
#include <cstdio>
using Function = int64_t (*)(femu_csd_args *);
struct Spill {
    std::vector<std::vector<unsigned char>> blocks{1};
    unsigned stores = 0, loads = 0, frees = 0;
    int64_t run(Function function, femu_csd_args &args)
    {
        args.cparam2 = 0;
        for (;;) {
            int64_t result = function(&args);
            assert(result >= 0);
            auto action = static_cast<uint64_t>(result) & ~CS_IO_VALUE_MASK;
            auto payload = static_cast<uint64_t>(result) & CS_IO_VALUE_MASK;
            auto *io = static_cast<unsigned char *>(args.mr_addr[args.numr - 1]);
            if (action == CS_NEED_STORE) {
                assert(payload == SORT_BLOCK_BYTES);
                blocks.emplace_back(io, io + payload);
                args.cparam2 = blocks.size() - 1;
                ++stores;
            } else if (action == CS_NEED_LOAD) {
                assert(payload < blocks.size() && !blocks[payload].empty());
                std::memcpy(io, blocks[payload].data(), blocks[payload].size());
                args.cparam2 = payload;
                ++loads;
            } else if (action == CS_NEED_FREE) {
                assert(payload < blocks.size() && !blocks[payload].empty());
                blocks[payload].clear();
                args.cparam2 = payload;
                ++frees;
            } else return result;
        }
    }
};
static void check_empty()
{
    cs_work_stats stats;
    assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
}
static void scenario(Function sink, Function source, Function average, unsigned chunks)
{
    Spill spill;
    std::vector<int32_t> input(SORT_CHUNK_BYTES / 4), expected;
    std::vector<unsigned char> io(SORT_BLOCK_BYTES);
    auto output = std::make_unique<sorted_chunk>();
    average_result result{};
    void *addresses[] = {input.data(), io.data()};
    long long lengths[] = {SORT_CHUNK_BYTES, SORT_BLOCK_BYTES};
    femu_csd_args args{};
    args.numr = 2; args.mr_addr = addresses; args.mr_len = lengths;
    for (unsigned chunk = 0; chunk < chunks; ++chunk) {
        for (unsigned i = 0; i < input.size(); ++i)
            input[i] = static_cast<int32_t>((i * 7919U + chunk * 13) % 100003) - 50001;
        expected.insert(expected.end(), input.begin(), input.end());
        auto handle = spill.run(sink, args);
        assert(handle > 0);
        if (!chunk) args.cparam1 = handle;
        else assert(args.cparam1 == handle);
        if (chunk < 2) assert(spill.stores == 0); // Runs stay resident while allocation succeeds.
    }
    unsigned sinkStores = spill.stores;
    if (chunks > 2) assert(sinkStores > 0);
    else assert(sinkStores == 0);
    // Leave too little space for every spilled head: source must evict another resident run.
    cs_work_context *other = nullptr;
    if (chunks > 2) {
        other = cs_work_begin();
        assert(other && cs_work_alloc(other, (3U << 20) + (512U << 10)));
    }
    std::sort(expected.begin(), expected.end(), std::greater<int32_t>());
    addresses[0] = output.get(); lengths[0] = sizeof(*output);
    size_t offset = 0;
    do {
        assert(spill.run(source, args) == output->count);
        assert(offset + output->count <= expected.size());
        assert(std::equal(output->values, output->values + output->count, expected.begin() + offset));
        void *averageAddresses[] = {output.get(), &result};
        long long averageLengths[] = {sizeof(*output), sizeof(result)};
        femu_csd_args averageArgs{};
        averageArgs.numr = 2; averageArgs.mr_addr = averageAddresses; averageArgs.mr_len = averageLengths;
        assert(average(&averageArgs) == static_cast<int64_t>(offset + output->count));
        assert(result.count == offset + output->count);
        int64_t sum = std::accumulate(expected.begin(), expected.begin() + result.count, int64_t{0});
        assert(result.sum == sum && result.average == static_cast<double>(sum) / result.count);
        offset += output->count;
    } while (!output->finished);
    assert(offset == expected.size() && result.finished);
    assert(result.metrics.runs == chunks && result.metrics.peak_bytes <= SORT_MEMORY_BYTES);
    assert(result.metrics.spill_write_bytes == static_cast<uint64_t>(spill.stores) * SORT_BLOCK_BYTES);
    assert(result.metrics.spill_read_bytes == static_cast<uint64_t>(spill.loads) * SORT_BLOCK_BYTES);
    assert(result.metrics.spilled_runs * SORT_BLOCKS_PER_RUN == spill.stores);
    assert(result.metrics.sink.wall_ns && result.metrics.source.wall_ns && result.metrics.run_sort_ns);
    assert(result.metrics.run_sort_ns <= result.metrics.sink.wall_ns);
    assert(spill.frees == spill.stores && spill.loads == spill.stores);
    if (chunks > 2) {
        assert(spill.stores > sinkStores);
        assert(!cs_work_end(other));
    }
    check_empty(); // EOF, including the final full output chunk, destroys the state.
}
static void check_average(Function average)
{
    auto input = std::make_unique<sorted_chunk>();
    average_result result{};
    void *addresses[] = {input.get(), &result};
    long long lengths[] = {sizeof(*input), sizeof(result)};
    femu_csd_args args{};
    args.numr = 2; args.mr_addr = addresses; args.mr_len = lengths;
    input->count = 0;
    assert(average(&args) == 0 && result.count == 0 && result.average == 0);
    input->count = 2; input->values[0] = 10; input->values[1] = 20;
    assert(average(&args) == 2 && result.sum == 30 && result.average == 15);
    input->count = 1; input->values[0] = 90; input->finished = 1;
    assert(average(&args) == 3 && result.sum == 120 && result.average == 40 && result.finished);
    assert(average(&args) == -1); // EOF may not be accumulated twice.
    result = {};
    input->count = 2; input->values[0] = INT32_MIN; input->values[1] = INT32_MAX;
    assert(average(&args) == 2 && result.sum == -1 && result.average == -0.5);
    result = {};
    input->count = 0;
    assert(average(&args) == 0 && result.count == 0 && result.average == 0 && result.finished);
}
int main(int argc, char **argv)
{
    // Heap replacement, removal, ties and signed extrema, independent of I/O.
    sort_heap heap{};
    std::vector<int32_t> heads;
    for (unsigned i = 0; i < SORT_MAX_RUNS; ++i) {
        int32_t value = i == 0 ? INT32_MIN : i == 1 ? INT32_MAX : static_cast<int32_t>(i % 7) - 3;
        heads.push_back(value);
        sort_heap_push(&heap, {value, i});
    }
    sort_heap_replace(&heap, {INT32_MIN, heap.entries[0].run});
    *std::max_element(heads.begin(), heads.end()) = INT32_MIN;
    std::sort(heads.begin(), heads.end(), std::greater<int32_t>());
    for (auto value : heads) {
        assert(heap.count && heap.entries[0].value == value);
        sort_heap_pop(&heap);
    }
    assert(!heap.count);
    // Validate the shared C run sorter independently of both merge paths.
    for (unsigned count : {0U, 1U, 16U, 17U, 1024U}) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            std::vector<int32_t> values(count);
            for (unsigned i = 0; i < count; ++i)
                values[i] = mode == 0 ? 7 : mode == 1 ? static_cast<int32_t>(i)
                    : mode == 2 ? -static_cast<int32_t>(i) : static_cast<int32_t>(i * 2654435761U);
            auto expected = values;
            std::sort(expected.begin(), expected.end(), std::greater<int32_t>());
            sort_int32_desc(values.data(), values.size());
            assert(values == expected);
        }
    }
    assert(argc == 3);
    void *sort = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL), *average = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    assert(sort && average);
    auto sink = reinterpret_cast<Function>(dlsym(sort, "sort_sink"));
    auto source = reinterpret_cast<Function>(dlsym(sort, "sort_source"));
    auto aggregate = reinterpret_cast<Function>(dlsym(average, "avg_int32"));
    assert(sink && source && aggregate && !dlsym(sort, "sort_create") && !dlsym(sort, "sort_abort"));
    check_average(aggregate);
    assert(!cs_work_init(12U << 20));
    scenario(sink, source, aggregate, 1);
    scenario(sink, source, aggregate, 4);
    // Early termination also uses source, with only the I/O AFDM.
    std::vector<int32_t> input(SORT_CHUNK_BYTES / 4, 7);
    std::vector<unsigned char> io(SORT_BLOCK_BYTES);
    void *addresses[] = {input.data(), io.data()};
    long long lengths[] = {SORT_CHUNK_BYTES, SORT_BLOCK_BYTES};
    femu_csd_args args{};
    args.numr = 2; args.mr_addr = addresses; args.mr_len = lengths;
    args.cparam1 = sink(&args);
    assert(args.cparam1 > 0);
    args.numr = 1; addresses[0] = io.data(); lengths[0] = io.size();
    assert(source(&args) == 0);
    check_empty();
    dlclose(average); dlclose(sort);
    puts("PASS: resident runs without I/O, allocation-triggered spill, merge pressure, complete output, EOF cleanup, early stop");
}
