// SNIA 设备访问；设备及其子资源由 shared_ptr 维持插件生命周期。
#include "cs_internal.hpp"
using namespace snia;
namespace snia {
std::shared_ptr<Device> openDevice(const char *name, void *context)
{
    CS_STATUS result = CS_DEVICE_NOT_AVAILABLE;
    for (const auto &plugin : pluginsSnapshot()) {
        auto device = std::make_shared<Device>();
        device->plugin = plugin;
        device->path = name;
        device->context = context;
        auto status = device->ops().openCSx(name, context, &device->privateData, &device->concurrency);
        if (status == CS_SUCCESS) {
            device->concurrency = std::max(1U, device->concurrency);
            return device;
        }
        if (status != CS_DEVICE_NOT_AVAILABLE && status != CS_NO_MATCHING_DEVICE) result = status;
    }
    throw result;
}
} // namespace snia
CS_STATUS csOpenCSx(const char *name, void *context, CS_DEV_HANDLE *out)
{
    return apiCall([&] {
        if (!name || !out) return CS_INVALID_ARG;
        *out = nullptr;
        if (std::strlen(name) >= 4096) return CS_INVALID_DEVICE_NAME;
        *out = publishHandle(openDevice(name, context));
        return CS_SUCCESS;
    });
}
CS_STATUS csCloseCSx(CS_DEV_HANDLE handle)
{
    return apiCall([&] {
        auto device = acquireHandle<Device>(handle);
        if (!device) return CS_INVALID_HANDLE;
        std::lock_guard<std::mutex> lock(handlesMutex);
        size_t functions = 0;
        for (const auto &entry : handles) {
            auto *function = dynamic_cast<Function *>(entry.second.get());
            if (function && function->owner == device) {
                if (entry.second.use_count() != 1) return CS_HANDLE_IN_USE;
                ++functions;
            }
        }
        if (device.use_count() != static_cast<long>(functions + 2)) return CS_HANDLE_IN_USE;
        for (auto it = handles.begin(); it != handles.end();) {
            auto *function = dynamic_cast<Function *>(it->second.get());
            if (it->first == device->id || (function && function->owner == device)) it = handles.erase(it);
            else ++it;
        }
        return CS_SUCCESS;
    });
}
static CS_STATUS notify(const char *name, u32 options, csDevNotificationFn callback, bool add)
{
    return apiCall([&] {
        if (!name || !options || !callback) return CS_INVALID_ARG;
        auto device = openDevice(name, nullptr);
        auto operation = add ? device->ops().registerNotify : device->ops().deregisterNotify;
        return operation ? operation(name, options, callback) : CS_UNSUPPORTED;
    });
}
CS_STATUS csRegisterNotify(const char *name, u32 options, csDevNotificationFn callback)
{ return notify(name, options, callback, true); }
CS_STATUS csDeregisterNotify(const char *name, u32 options, csDevNotificationFn callback)
{ return notify(name, options, callback, false); }
