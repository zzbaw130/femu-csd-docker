#ifndef CS_COMMON_HPP
#define CS_COMMON_HPP
#include "cs.h"
#include "cs_backend_interface.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <list>
#include <memory>
#include <new>
#include <mutex>
#include <string>
#include <thread>
#include <system_error>
#include <utility>
#include <unordered_map>
#include <vector>

namespace snia {
constexpr unsigned MaxBatch = 64, MaxArgs = 64;
class Device;

// C API / worker 边界统一转换异常；内部失败由 RAII 自动释放已取得的资源。
template<class F> CS_STATUS apiCall(F &&call) noexcept
{
    try { return call(); }
    catch (CS_STATUS status) { return status; }
    catch (const std::bad_alloc &) { return CS_OUT_OF_RESOURCES; }
    catch (const std::system_error &) { return CS_OUT_OF_RESOURCES; }
    catch (...) { return CS_FATAL_ERROR; }
}
inline void check(CS_STATUS status) { if (status != CS_SUCCESS) throw status; }
inline CS_STATUS outputSize(int *length, void *buffer, size_t needed)
{
    if (!length || needed > static_cast<size_t>(std::numeric_limits<int>::max())) return CS_INVALID_ARG;
    int available = *length;
    *length = static_cast<int>(needed);
    return !buffer || available < *length ? CS_INVALID_LENGTH : CS_SUCCESS;
}

// 模块由设备及发现快照共同持有；最后一个引用释放时卸载。
struct Plugin final {
    void *library = nullptr;
    const cs_backend_ops *ops = nullptr;
    cs_backend_destroy_fn destroy = nullptr;
    std::string path;
    ~Plugin();
};
std::vector<std::shared_ptr<Plugin>> pluginsSnapshot();
std::shared_ptr<Device> openDevice(const char *name, void *context);

// 虚析构只用于库内部的真实资源类型；C 插件边界不传递这些 C++ 对象。
struct Handle {
    uintptr_t id = 0;
    bool closing = false;
    virtual ~Handle() = default;
};
struct Resource : Handle {
    virtual Device *device() = 0;
    virtual void *backendHandle() const = 0;
};
struct Device final : Resource {
    std::shared_ptr<Plugin> plugin;
    void *privateData = nullptr, *context = nullptr;
    unsigned concurrency = 1;
    std::string path;
    ~Device() override;
    Device *device() override { return this; }
    void *backendHandle() const override { return privateData; }
    const cs_backend_ops &ops() const { return *plugin->ops; }
};
struct Memory final : Resource {
    std::shared_ptr<Device> owner;
    void *privateData = nullptr;
    u64 bytes = 0;
    ~Memory() override;
    Device *device() override { return owner.get(); }
    void *backendHandle() const override { return privateData; }
};
struct Function final : Resource {
    std::shared_ptr<Device> owner;
    void *privateData = nullptr;
    u32 resourceId = 0;
    Device *device() override { return owner.get(); }
    void *backendHandle() const override { return privateData; }
};
struct Completion { void *context; u64 value; CS_STATUS status; };
struct Event final : Handle { std::list<Completion> completions; };
struct FreeBuffer { void operator()(CsBatchRequest *p) const { std::free(p); } };
using RequestBuffer = std::unique_ptr<CsBatchRequest, FreeBuffer>;
struct PreparedRequest final {
    RequestBuffer request;
    std::vector<std::shared_ptr<Resource>> resources;
    Device *device = nullptr; // resources 持有其所有者，执行期间始终有效。
    CS_STATUS execute(u64 *value) const;
    CS_STATUS executeBatchEntry(std::atomic_bool &cancelled, void *execution, u64 *value) const;
};
struct BatchEntry { std::shared_ptr<PreparedRequest> request; u64 dependencies = 0; };
struct Batch final : Handle {
    CS_BATCH_MODE mode;
    unsigned capacity;
    std::array<BatchEntry, MaxBatch> entries{};
    Batch(CS_BATCH_MODE mode, unsigned capacity) : mode(mode), capacity(capacity) {}
};
struct Request final : Handle {
    std::shared_ptr<PreparedRequest> task;
    std::shared_ptr<Batch> batch;
    std::shared_ptr<Event> event;
    std::list<Completion> completion; // 提交前分配，完成时 splice 不再申请内存。
    void *context = nullptr;
    csQueueCallbackFn callback = nullptr;
    std::atomic_bool cancelled{false}, started{false};
};

extern std::mutex handlesMutex, csfBindingMutex;
extern std::unordered_map<uintptr_t, std::shared_ptr<Handle>> handles;
void *publishHandle(std::shared_ptr<Handle> object);
template<class T> std::shared_ptr<T> acquireHandle(void *handle)
{
    std::lock_guard<std::mutex> lock(handlesMutex);
    auto it = handles.find(reinterpret_cast<uintptr_t>(handle));
    return it == handles.end() || it->second->closing ? nullptr : std::dynamic_pointer_cast<T>(it->second);
}
// 释放设备资源期间阻止新的请求引用；失败自动恢复，成功 commit 才删除句柄。
class CloseHandle final {
    Handle *object = nullptr;
public:
    template<class T> explicit CloseHandle(const std::shared_ptr<T> &handle)
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        if (handle.use_count() == 2 && !handle->closing) {
            object = handle.get();
            object->closing = true;
        }
    }
    ~CloseHandle();
    explicit operator bool() const { return object != nullptr; }
    void commit();
};
bool csfInUse(const Device &device, u32 id);
std::shared_ptr<PreparedRequest> prepareRequest(const CsBatchRequest &request);
RequestBuffer requestBuffer(size_t bytes);
CS_STATUS submitRequest(std::shared_ptr<PreparedRequest> task, std::shared_ptr<Batch> batch,
                        void *context, csQueueCallbackFn callback, CS_EVT_HANDLE event,
                        CS_REQ_HANDLE *request, u64 *value);
CS_STATUS runBatch(const Batch &batch, std::atomic_bool &cancelled, u64 *value);
} // namespace snia
#endif
