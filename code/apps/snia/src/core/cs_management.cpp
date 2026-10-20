// SNIA 设备管理；句柄与锁通过作用域自动释放。
#include "cs_internal.hpp"
using namespace snia;
CS_STATUS csCSFDownload(CS_DEV_HANDLE handle, const CsCSFDownloadInfo *info, u32 *out)
{
    return apiCall([&] {
        if (!info || !out) return CS_INVALID_ARG;
        auto device = acquireHandle<Device>(handle);
        if (!device) return CS_INVALID_HANDLE;
        std::lock_guard<std::mutex> lock(csfBindingMutex);
        if (csfInUse(*device, info->Index)) return CS_HANDLE_IN_USE;
        auto operation = device->ops().CSFDownload;
        return operation ? operation(device->privateData, info, out) : CS_UNSUPPORTED;
    });
}
CS_STATUS csConfig(CS_DEV_HANDLE handle, int *length, const CsConfigInfo *info, CsConfigData *out)
{
    return apiCall([&] {
        if (!info || !length) return CS_INVALID_ARG;
        auto device = acquireHandle<Device>(handle);
        if (!device) return CS_INVALID_HANDLE;
        std::lock_guard<std::mutex> lock(csfBindingMutex);
        if (info->Type == CS_CSF_ACTIVATE && info->u.CSFActivateInfo.State == CS_INACTIVE_STATE &&
            csfInUse(*device, info->u.CSFActivateInfo.CSFId)) return CS_HANDLE_IN_USE;
        auto operation = device->ops().config;
        return operation ? operation(device->privateData, length, info, out) : CS_UNSUPPORTED;
    });
}
CS_STATUS csReset(CS_DEV_HANDLE handle, CS_RESOURCE_TYPE type, u32 id)
{
    return apiCall([&] {
        auto device = acquireHandle<Device>(handle);
        if (!device) return CS_INVALID_HANDLE;
        auto operation = device->ops().reset;
        return operation ? operation(device->privateData, type, id) : CS_UNSUPPORTED;
    });
}
CS_STATUS csQueryDeviceProperties(CS_DEV_HANDLE handle, CS_RESOURCE_TYPE type, int *length, CsProperties *out)
{
    return apiCall([&] {
        if (!length) return CS_INVALID_ARG;
        auto device = acquireHandle<Device>(handle);
        if (!device) return CS_INVALID_HANDLE;
        auto operation = device->ops().queryDeviceProperties;
        return operation ? operation(device->privateData, type, length, out) : CS_UNSUPPORTED;
    });
}
CS_STATUS csQueryDeviceStatistics(CS_DEV_HANDLE handle, CS_STAT_TYPE type, void *id, CsStatsInfo *out)
{
    return apiCall([&] {
        if (!out) return CS_INVALID_ARG;
        auto device = acquireHandle<Device>(handle);
        if (!device) return CS_INVALID_HANDLE;
        auto operation = device->ops().queryDeviceStatistics;
        return operation ? operation(device->privateData, type, id, out) : CS_UNSUPPORTED;
    });
}
