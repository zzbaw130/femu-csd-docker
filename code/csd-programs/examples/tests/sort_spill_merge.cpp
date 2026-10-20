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
static void scenario(Function sink, Function source, Function limit, unsigned chunks)
{
    Spill spill;
    std::vector<int32_t> input(SORT_CHUNK_BYTES / 4), expected;
    std::vector<unsigned char> io(SORT_BLOCK_BYTES);
    auto output = std::make_unique<sorted_chunk>();
    top_five top{};
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
        void *limitAddresses[] = {output.get(), &top};
        long long limitLengths[] = {sizeof(*output), sizeof(top)};
        femu_csd_args limitArgs{};
        limitArgs.numr = 2; limitArgs.mr_addr = limitAddresses; limitArgs.mr_len = limitLengths;
        assert(limit(&limitArgs) == 5 && top.count == 5);
        assert(std::equal(top.values, top.values + 5, expected.begin()));
        offset += output->count;
    } while (!output->finished);
    assert(offset == expected.size() && top.finished);
    assert(spill.frees == spill.stores && spill.loads == spill.stores);
    if (chunks > 2) {
        assert(spill.stores > sinkStores);
        assert(!cs_work_end(other));
    }
    check_empty(); // EOF, including the final full output chunk, destroys the state.
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    void *sort = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL), *limit = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    assert(sort && limit);
    auto sink = reinterpret_cast<Function>(dlsym(sort, "sort_sink"));
    auto source = reinterpret_cast<Function>(dlsym(sort, "sort_source"));
    auto take = reinterpret_cast<Function>(dlsym(limit, "limit_five"));
    assert(sink && source && take && !dlsym(sort, "sort_create") && !dlsym(sort, "sort_abort"));
    assert(!cs_work_init(12U << 20));
    scenario(sink, source, take, 1);
    scenario(sink, source, take, 4);
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
    dlclose(limit); dlclose(sort);
    puts("PASS: resident runs without I/O, allocation-triggered spill, merge pressure, complete output, EOF cleanup, early stop");
}
