// 各类公开句柄共享的类型检查与所有权；智能指针负责资源的最终析构。
#include "cs_common.hpp"
namespace snia {
std::mutex handlesMutex, csfBindingMutex;
std::unordered_map<uintptr_t, std::shared_ptr<Handle>> handles;
static uintptr_t nextId = 16;

void *publishHandle(std::shared_ptr<Handle> object)
{
    std::lock_guard<std::mutex> lock(handlesMutex);
    if (nextId == std::numeric_limits<uintptr_t>::max()) throw CS_OUT_OF_RESOURCES;
    object->id = nextId++;
    handles.emplace(object->id, object);
    return reinterpret_cast<void *>(object->id);
}
CloseHandle::~CloseHandle()
{
    if (!object) return;
    std::lock_guard<std::mutex> lock(handlesMutex);
    object->closing = false;
}
void CloseHandle::commit()
{
    std::lock_guard<std::mutex> lock(handlesMutex);
    handles.erase(object->id);
    object = nullptr;
}
bool csfInUse(const Device &device, u32 id)
{
    std::lock_guard<std::mutex> lock(handlesMutex);
    for (const auto &entry : handles) {
        auto *function = dynamic_cast<Function *>(entry.second.get());
        if (function && function->owner->plugin == device.plugin &&
            function->owner->path == device.path && function->resourceId == id && entry.second.use_count() > 1)
            return true;
    }
    return false;
}
Device::~Device()
{
    if (privateData) ops().closeCSx(privateData);
}
Memory::~Memory()
{
    if (privateData) {
        CsMemFlags flags{};
        owner->ops().freeMem(owner->privateData, privateData, &flags);
    }
}
} // namespace snia
