// SNIA AFDM 管理与复制；失败分支由 Memory/CloseHandle 自动回收或恢复。
#include "cs_common.hpp"
using namespace snia;
CS_STATUS csAllocMem(CS_DEV_HANDLE handle, int bytes, const CsMemFlags *flags, CS_MEM_HANDLE *out, CS_MEM_PTR *address)
{
    return apiCall([&] {
        if (!out || !flags || bytes <= 0) return CS_INVALID_ARG;
        *out = nullptr;
        auto device = acquireHandle<Device>(handle);
        if (!device) return CS_INVALID_HANDLE;
        auto memory = std::make_shared<Memory>();
        memory->owner = device;
        auto status = device->ops().allocMem(device->privateData, bytes, flags, &memory->privateData, &memory->bytes, address);
        if (status == CS_SUCCESS) *out = publishHandle(std::move(memory));
        return status;
    });
}
CS_STATUS csInitMem(CS_MEM_HANDLE handle, unsigned long offset, int bytes, const CsMemFlags *flags)
{
    return apiCall([&] {
        if (!flags || bytes <= 0) return CS_INVALID_ARG;
        auto memory = acquireHandle<Memory>(handle);
        if (!memory) return CS_INVALID_HANDLE;
        if (offset > memory->bytes || static_cast<u64>(bytes) > memory->bytes - offset) return CS_INVALID_LENGTH;
        return memory->owner->ops().initMem(memory->owner->privateData, memory->privateData, offset, bytes, flags);
    });
}
CS_STATUS csFreeMem(CS_MEM_HANDLE handle, const CsMemFlags *flags)
{
    return apiCall([&] {
        if (!flags) return CS_INVALID_ARG;
        auto memory = acquireHandle<Memory>(handle);
        if (!memory) return CS_INVALID_HANDLE;
        CloseHandle closing(memory);
        if (!closing) return CS_MEMORY_IN_USE;
        auto status = memory->owner->ops().freeMem(memory->owner->privateData, memory->privateData, flags);
        if (status == CS_SUCCESS) {
            memory->privateData = nullptr;
            closing.commit();
        }
        return status;
    });
}
CS_STATUS csQueueCopyMemRequest(const CsCopyMemRequest *request, void *context, csQueueCallbackFn callback,
                                CS_EVT_HANDLE event, CS_REQ_HANDLE *out, u64 *value)
{
    return apiCall([&] {
        if (!request) return CS_INVALID_ARG;
        CsBatchRequest batch{};
        batch.ReqType = CS_COPY_AFDM;
        batch.u.CopyMem = *request;
        return submitRequest(prepareRequest(batch), {}, context, callback, event, out, value);
    });
}
