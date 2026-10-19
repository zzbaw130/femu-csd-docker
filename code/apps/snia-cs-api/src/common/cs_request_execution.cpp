// 各类 queue API 共用的请求快照、句柄绑定和同步/异步执行。
#include "cs_common.hpp"
using namespace snia;
namespace {
template<class T> void *bind(PreparedRequest &request, void *handle, CS_STATUS invalid)
{
    auto resource = acquireHandle<T>(handle);
    if (!resource) throw invalid;
    if (request.device && resource->device() != request.device) throw CS_INVALID_ARG;
    request.device = resource->device();
    void *token = resource->backendHandle();
    request.resources.push_back(std::move(resource));
    return token;
}
void bindMemory(PreparedRequest &request, const CsDevAFDM &source, CsDevAFDM &target, u64 bytes)
{
    target.MemHandle = bind<Memory>(request, source.MemHandle, CS_UNKNOWN_MEMORY);
    const auto &memory = static_cast<const Memory &>(*request.resources.back());
    if (source.ByteOffset > memory.bytes || bytes > memory.bytes - source.ByteOffset) throw CS_INVALID_LENGTH;
}
unsigned activeRequests = 0; // 由 handlesMutex 保护。
void runRequest(const std::shared_ptr<Request> &request)
{
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        request->started = true;
    }
    u64 value = 0;
    auto status = apiCall([&] {
        if (request->cancelled) return CS_ERROR_IN_EXECUTION;
        return request->batch ? runBatch(*request->batch, request->cancelled, &value)
                              : request->task->execute(&value);
    });
    if (request->cancelled) status = CS_ERROR_IN_EXECUTION;
    request->task.reset();
    request->batch.reset();
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        handles.erase(request->id);
        if (request->event) {
            request->completion.front() = {request->context, value, status};
            request->event->completions.splice(request->event->completions.end(), request->completion);
            // 发布完成与释放 event 引用在同一锁内，poll 成功后即可安全删除。
            request->event.reset();
        }
    }
    if (request->callback)
        apiCall([&] { request->callback(request->context, status, value); return CS_SUCCESS; });
    std::lock_guard<std::mutex> lock(handlesMutex);
    --activeRequests;
}
} // namespace

namespace snia {
RequestBuffer requestBuffer(size_t bytes)
{
    auto *buffer = static_cast<CsBatchRequest *>(std::calloc(1, std::max(bytes, sizeof(CsBatchRequest))));
    if (!buffer) throw CS_OUT_OF_RESOURCES;
    return RequestBuffer(buffer);
}
std::shared_ptr<PreparedRequest> prepareRequest(const CsBatchRequest &source)
{
    size_t bytes = sizeof(source);
    if (source.ReqType == CS_QUEUE_COMPUTE) {
        int n = source.u.Compute.NumArgs;
        if (n < 0 || n > static_cast<int>(MaxArgs)) throw CS_INVALID_ARG;
        bytes = offsetof(CsBatchRequest, u.Compute.Args) + n * sizeof(CsComputeArg);
    } else if (source.ReqType == CS_STORAGE_IO) {
        if (source.u.StorageIo.Mode == CS_STORAGE_BLOCK_IO) {
            int n = source.u.StorageIo.u.BlockIo.NumRanges;
            if (n <= 0 || n > 4096) throw CS_INVALID_ARG;
            bytes = offsetof(CsBatchRequest, u.StorageIo.u.BlockIo.Range) + n * sizeof(CSBlockRange);
        } else if (source.u.StorageIo.Mode != CS_STORAGE_FILE_IO) throw CS_INVALID_OPTION;
    } else if (source.ReqType != CS_COPY_AFDM) throw CS_INVALID_OPTION;
    auto prepared = std::make_shared<PreparedRequest>();
    prepared->request = requestBuffer(bytes);
    std::memcpy(prepared->request.get(), &source, std::max(bytes, sizeof(source)));
    auto &target = *prepared->request;
    std::lock_guard<std::mutex> lock(csfBindingMutex);
    if (source.ReqType == CS_QUEUE_COMPUTE) {
        target.u.Compute.CSFId = bind<Function>(*prepared, source.u.Compute.CSFId, CS_INVALID_CSF_ID);
        for (int i = 0; i < source.u.Compute.NumArgs; ++i)
            if (source.u.Compute.Args[i].Type == CS_AFDM_TYPE)
                bindMemory(*prepared, source.u.Compute.Args[i].u.DevMem, target.u.Compute.Args[i].u.DevMem, 0);
    } else if (source.ReqType == CS_COPY_AFDM) {
        const auto &copy = source.u.CopyMem;
        if (copy.Bytes <= 0) throw CS_INVALID_ARG;
        bindMemory(*prepared, copy.DevMem, target.u.CopyMem.DevMem, copy.Bytes);
        if (copy.Type == CS_COPY_WITHIN_DEVICE)
            bindMemory(*prepared, copy.u.SrcDevMem, target.u.CopyMem.u.SrcDevMem, copy.Bytes);
        else if (copy.Type != CS_COPY_TO_DEVICE && copy.Type != CS_COPY_FROM_DEVICE) throw CS_INVALID_OPTION;
        else if (!copy.u.HostVAddress) throw CS_INVALID_ARG;
    } else {
        const auto &storage = source.u.StorageIo;
        target.u.StorageIo.DevHandle = bind<Device>(*prepared, storage.DevHandle, CS_INVALID_HANDLE);
        if (storage.Mode == CS_STORAGE_BLOCK_IO)
            bindMemory(*prepared, storage.u.BlockIo.DevMem, target.u.StorageIo.u.BlockIo.DevMem, 0);
        else bindMemory(*prepared, storage.u.FileIo.DevMem, target.u.StorageIo.u.FileIo.DevMem, storage.u.FileIo.Bytes);
    }
    if (prepared->device->ops().validateRequest)
        check(prepared->device->ops().validateRequest(prepared->device->privateData, &target));
    return prepared;
}
CS_STATUS PreparedRequest::execute(u64 *value) const
{
    u64 ignored = 0;
    if (!value) value = &ignored;
    *value = 0;
    const auto &ops = device->ops();
    switch (request->ReqType) {
    case CS_QUEUE_COMPUTE:
        return ops.queueComputeRequest ? ops.queueComputeRequest(device->privateData, &request->u.Compute, value) : CS_UNSUPPORTED;
    case CS_COPY_AFDM:
        return ops.queueCopyMemRequest ? ops.queueCopyMemRequest(device->privateData, &request->u.CopyMem, value) : CS_UNSUPPORTED;
    case CS_STORAGE_IO:
        return ops.queueStorageRequest ? ops.queueStorageRequest(device->privateData, &request->u.StorageIo, value) : CS_UNSUPPORTED;
    default: return CS_INVALID_OPTION;
    }
}
CS_STATUS PreparedRequest::executeBatchEntry(std::atomic_bool &cancelled, void *execution, u64 *value) const
{
    auto isCancelled = [](void *p) { return static_cast<std::atomic_bool *>(p)->load(); };
    auto operation = device->ops().executeBatchEntry;
    return operation ? operation(device->privateData, request.get(), isCancelled, &cancelled, execution, value) : execute(value);
}
CS_STATUS submitRequest(std::shared_ptr<PreparedRequest> task, std::shared_ptr<Batch> batch,
                        void *context, csQueueCallbackFn callback, CS_EVT_HANDLE eventHandle,
                        CS_REQ_HANDLE *out, u64 *value)
{
    if (out) *out = nullptr;
    if (value) *value = 0;
    if (callback && eventHandle) return CS_INVALID_ARG;
    auto event = eventHandle ? acquireHandle<Event>(eventHandle) : nullptr;
    if (eventHandle && !event) return CS_INVALID_HANDLE;
    if (!callback && !event) {
        std::atomic_bool cancelled{false};
        return batch ? runBatch(*batch, cancelled, value) : task->execute(value);
    }
    auto request = std::make_shared<Request>();
    request->task = std::move(task);
    request->batch = std::move(batch);
    request->event = std::move(event);
    request->context = context;
    request->callback = callback;
    if (request->event) request->completion.push_back({context, 0, CS_NOT_DONE});
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        if (activeRequests >= 64) return CS_OUT_OF_RESOURCES;
        ++activeRequests;
    }
    try {
        auto handle = publishHandle(request);
        if (out) *out = handle;
        std::thread([request] { runRequest(request); }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(handlesMutex);
        handles.erase(request->id);
        --activeRequests;
        if (out) *out = nullptr;
        throw;
    }
    return CS_QUEUED;
}
} // namespace snia
