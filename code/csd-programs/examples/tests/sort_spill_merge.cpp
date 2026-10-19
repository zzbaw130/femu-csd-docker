// Native operator test: emulate batch STORE/LOAD/FREE and verify the entire merged stream.
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "batch_sort.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <dlfcn.h>
#include <functional>
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
int main(int argc, char **argv)
{
    assert(argc == 3);
    void *sort = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL), *limit = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    assert(sort && limit);
    auto create = reinterpret_cast<Function>(dlsym(sort, "sort_create"));
    auto sink = reinterpret_cast<Function>(dlsym(sort, "sort_sink"));
    auto source = reinterpret_cast<Function>(dlsym(sort, "sort_source"));
    auto destroy = reinterpret_cast<Function>(dlsym(sort, "sort_destroy"));
    auto abort = reinterpret_cast<Function>(dlsym(sort, "sort_abort"));
    auto take = reinterpret_cast<Function>(dlsym(limit, "limit_five"));
    assert(create && sink && source && destroy && abort && take);
    assert(!cs_work_init(24U << 20));
    std::vector<int32_t> input(SORT_CHUNK_BYTES / 4), expected;
    std::vector<unsigned char> io(SORT_BLOCK_BYTES);
    sorted_chunk output{};
    top_five top{};
    void *addresses[] = {input.data(), io.data()};
    long long lengths[] = {SORT_CHUNK_BYTES, SORT_BLOCK_BYTES};
    femu_csd_args args{};
    args.numr = 2; args.mr_addr = addresses; args.mr_len = lengths;
    args.cparam1 = create(&args);
    assert(args.cparam1 > 0);
    Spill spill;
    // Duplicates, negatives and interleaved run ranges exercise merge refills and EOF.
    for (unsigned chunk = 0; chunk < 3; ++chunk) {
        for (unsigned i = 0; i < input.size(); ++i) input[i] = static_cast<int32_t>((i * 7919U + chunk * 13) % 100003) - 50001;
        expected.insert(expected.end(), input.begin(), input.end());
        assert(spill.run(sink, args) == chunk + 1);
    }
    std::sort(expected.begin(), expected.end(), std::greater<int32_t>());
    addresses[0] = &output; lengths[0] = sizeof(output);
    size_t offset = 0;
    while (spill.run(source, args)) {
        assert(offset + output.count <= expected.size());
        assert(std::equal(output.values, output.values + output.count, expected.begin() + offset));
        if (!offset) {
            void *limitAddresses[] = {&output, &top};
            long long limitLengths[] = {sizeof(output), sizeof(top)};
            femu_csd_args limitArgs{};
            limitArgs.numr = 2; limitArgs.mr_addr = limitAddresses; limitArgs.mr_len = limitLengths;
            assert(take(&limitArgs) == 5 && top.count == 5);
            assert(std::equal(top.values, top.values + 5, expected.begin()));
        }
        offset += output.count;
    }
    assert(offset == expected.size() && spill.stores == 3 * SORT_BLOCKS_PER_RUN && spill.loads == spill.stores);
    addresses[0] = io.data(); lengths[0] = io.size(); args.numr = 1;
    assert(spill.run(destroy, args) == 0 && spill.frees == spill.stores);
    args.cparam1 = create(&args);
    assert(args.cparam1 > 0 && abort(&args) == 0);
    cs_work_stats stats;
    assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts);
    dlclose(limit); dlclose(sort);
    puts("PASS: spill/reentry, full descending merge across blocks/chunks, duplicates/negatives, LIMIT, FREE, abort");
}
