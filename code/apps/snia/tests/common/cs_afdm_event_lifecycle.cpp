// 只使用公开 C API；在不同 C++ 分配点注入失败，检查回收与异常转换。
#include "cs.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <unistd.h>
static std::atomic<int> failAfter{-1};
void *operator new(std::size_t bytes)
{
    int remaining = failAfter.load();
    if (remaining >= 0 && failAfter.fetch_sub(1) == 0) throw std::bad_alloc();
    if (void *memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

int main()
{
    CS_DEV_HANDLE device;
    assert(csOpenCSx("/dev/nvme0n1", nullptr, &device) == CS_SUCCESS);
    CsMemFlags flags{};
    flags.FDMId = 1;
    unsigned failures = 0;
    for (int point = 0; point < 8; ++point) {
        CS_MEM_HANDLE memory = reinterpret_cast<void *>(1);
        failAfter = point;
        auto status = csAllocMem(device, 4096, &flags, &memory, nullptr);
        failAfter = -1;
        if (status == CS_SUCCESS) assert(csFreeMem(memory, &flags) == CS_SUCCESS);
        else { assert(status == CS_OUT_OF_RESOURCES && !memory); ++failures; }
        // 包括已分配 AFDM、但公开句柄创建失败的路径：整个 FDM 仍须可用。
        assert(csAllocMem(device, 64 << 20, &flags, &memory, nullptr) == CS_SUCCESS);
        assert(csFreeMem(memory, &flags) == CS_SUCCESS);
    }
    assert(failures >= 2);
    CS_EVT_HANDLE event = reinterpret_cast<void *>(1);
    failAfter = 0;
    auto status = csCreateEvent(&event);
    failAfter = -1;
    assert(status == CS_OUT_OF_RESOURCES && !event);

    CS_MEM_HANDLE memory;
    assert(csAllocMem(device, 4096, &flags, &memory, nullptr) == CS_SUCCESS);
    assert(csCreateEvent(&event) == CS_SUCCESS);
    char data[16]{};
    CsCopyMemRequest request{};
    request.Type = CS_COPY_TO_DEVICE;
    request.DevMem.MemHandle = memory;
    request.u.HostVAddress = data;
    request.Bytes = sizeof(data);
    unsigned submitted = 0, rejected = 0;
    for (int attempt = 0; attempt < 128; ++attempt) {
        CS_REQ_HANDLE handle = nullptr;
        failAfter = attempt % 8;
        status = csQueueCopyMemRequest(&request, nullptr, nullptr, event, &handle, nullptr);
        failAfter = -1;
        if (status == CS_OUT_OF_RESOURCES) {
            assert(!handle && csPollEvent(event, nullptr, nullptr) == CS_NOT_DONE);
            ++rejected;
        } else {
            assert(status == CS_QUEUED);
            for (int wait = 0; wait < 3000; ++wait) {
                status = csPollEvent(event, nullptr, nullptr);
                if (status != CS_NOT_DONE) break;
                usleep(1000);
            }
            assert(status == CS_SUCCESS);
            ++submitted;
        }
    }
    assert(submitted && rejected);
    assert(csDeleteEvent(event) == CS_SUCCESS);
    assert(csFreeMem(memory, &flags) == CS_SUCCESS);
    assert(csCloseCSx(device) == CS_SUCCESS);
    std::puts("PASS: allocation failure cleanup, C status conversion, async ownership and retry");
}
