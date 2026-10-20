// SNIA CSF 调度；内部资源由智能指针管理，C API 仅返回状态。
#include "cs_internal.hpp"
#include <cstdarg>
using namespace snia;
void csHelperSetComputeArg(CsComputeArg *a, CS_COMPUTE_ARG_TYPE type, ...)
{
    if (!a)
        return;
    memset(a, 0, sizeof(*a));
    a->Type = type;
    va_list ap;
    va_start(ap, type);
    switch (type) {
    case CS_AFDM_TYPE:
        a->u.DevMem.MemHandle = va_arg(ap, CS_MEM_HANDLE);
        a->u.DevMem.ByteOffset = va_arg(ap, unsigned long);
        break;
    case CS_8BIT_VALUE_TYPE:
        a->u.Value8 = (u8)va_arg(ap, int);
        break;
    case CS_16BIT_VALUE_TYPE:
        a->u.Value16 = (u16)va_arg(ap, int);
        break;
    case CS_32BIT_VALUE_TYPE:
        a->u.Value32 = va_arg(ap, u32);
        break;
    case CS_64BIT_VALUE_TYPE:
        a->u.Value64 = va_arg(ap, u64);
        break;
    default:
        break;
    }
    va_end(ap);
}
CS_STATUS csQueueComputeRequest(const CsComputeRequest *request, void *context, csQueueCallbackFn callback,
                                CS_EVT_HANDLE event, CS_REQ_HANDLE *out, u64 *value)
{
    return apiCall([&] {
        if (!request || request->NumArgs < 0 || request->NumArgs > static_cast<int>(MaxArgs)) return CS_INVALID_ARG;
        size_t bytes = offsetof(CsComputeRequest, Args) + request->NumArgs * sizeof(CsComputeArg);
        auto buffer = requestBuffer(offsetof(CsBatchRequest, u.Compute) + bytes);
        buffer->ReqType = CS_QUEUE_COMPUTE;
        std::memcpy(&buffer->u.Compute, request, bytes);
        return submitRequest(prepareRequest(*buffer), {}, context, callback, event, out, value);
    });
}
CS_STATUS csAbortRequest(CS_REQ_HANDLE handle)
{
    return apiCall([&] {
        auto request = acquireHandle<Request>(handle);
        if (!request) return CS_INVALID_HANDLE;
        std::lock_guard<std::mutex> lock(handlesMutex);
        if (request->started) return CS_UNSUPPORTED;
        request->cancelled = true;
        return CS_SUCCESS;
    });
}
CS_STATUS csGetCSFId(CS_DEV_HANDLE handle, const char *name, u64 global, int *length, int *count, CSFIdinfo *out)
{
    return apiCall([&] {
        if (!length || !count || (!name && !global) || (name && global)) return CS_INVALID_ARG;
        if (name && (!*name || strnlen(name, 32) == 32)) return CS_INVALID_CSF_NAME;
        auto device = acquireHandle<Device>(handle);
        if (!device) return CS_INVALID_HANDLE;
        const auto &ops = device->ops();
        if (!ops.getCSFId || !ops.getCSFResourceId) return CS_UNSUPPORTED;
        std::lock_guard<std::mutex> lock(csfBindingMutex);
        auto status = ops.getCSFId(device->privateData, name, global, length, count, out);
        if (status != CS_SUCCESS) return status;
        std::vector<uintptr_t> published;
        published.reserve(*count);
        try {
            for (int i = 0; i < *count; ++i) {
                auto function = std::make_shared<Function>();
                function->owner = device;
                function->privateData = out[i].CSFId;
                function->resourceId = ops.getCSFResourceId(device->privateData, function->privateData);
                out[i].CSFId = publishHandle(std::move(function));
                published.push_back(reinterpret_cast<uintptr_t>(out[i].CSFId));
            }
        } catch (...) {
            std::lock_guard<std::mutex> registryLock(handlesMutex);
            for (auto id : published) handles.erase(id);
            throw;
        }
        return CS_SUCCESS;
    });
}
CS_STATUS csCSEEDownload(CS_DEV_HANDLE handle, const CsCSEEDownloadInfo *info, u32 *out)
{
    return apiCall([&] {
        if (!info || !out) return CS_INVALID_ARG;
        auto device = acquireHandle<Device>(handle);
        if (!device) return CS_INVALID_HANDLE;
        auto operation = device->ops().CSEEDownload;
        return operation ? operation(device->privateData, info, out) : CS_UNSUPPORTED;
    });
}
