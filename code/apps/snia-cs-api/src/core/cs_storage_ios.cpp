// SNIA 存储 I/O；可变长请求的临时副本自动释放。
#include "cs_common.hpp"
using namespace snia;
CS_STATUS csQueueStorageRequest(const CsStorageRequest *request, void *context, csQueueCallbackFn callback,
                                CS_EVT_HANDLE event, CS_REQ_HANDLE *out, u64 *value)
{
    return apiCall([&] {
        if (!request) return CS_INVALID_ARG;
        size_t bytes = sizeof(*request);
        if (request->Mode == CS_STORAGE_BLOCK_IO) {
            int n = request->u.BlockIo.NumRanges;
            if (n <= 0 || n > 4096) return CS_INVALID_ARG;
            bytes = offsetof(CsStorageRequest, u.BlockIo.Range) + n * sizeof(CSBlockRange);
        } else if (request->Mode != CS_STORAGE_FILE_IO) return CS_INVALID_OPTION;
        auto buffer = requestBuffer(offsetof(CsBatchRequest, u.StorageIo) + bytes);
        buffer->ReqType = CS_STORAGE_IO;
        std::memcpy(&buffer->u.StorageIo, request, bytes);
        return submitRequest(prepareRequest(*buffer), {}, context, callback, event, out, value);
    });
}
