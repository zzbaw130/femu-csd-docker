#include "internal.h"
#include <stddef.h>
CS_STATUS csQueueStorageRequest(const CsStorageRequest *r, void *ctx, csQueueCallbackFn cb,
                                CS_EVT_HANDLE ev, CS_REQ_HANDLE *req, u64 *value)
{
    if (!r) return CS_INVALID_ARG;
    if (r->Mode == CS_STORAGE_FILE_IO) {
        CsBatchRequest b = {.ReqType = CS_STORAGE_IO}; b.u.StorageIo = *r;
        return cs_queue_one(&b, ctx, cb, ev, req, value);
    }
    if (r->Mode != CS_STORAGE_BLOCK_IO) return CS_INVALID_OPTION;
    if (r->u.BlockIo.NumRanges <= 0 || r->u.BlockIo.NumRanges > 4096)
        return CS_INVALID_ARG;
    size_t bytes = offsetof(CsBatchRequest, u.StorageIo.u.BlockIo.Range) +
                   (size_t)r->u.BlockIo.NumRanges * sizeof(CSBlockRange);
    if (bytes < sizeof(CsBatchRequest))
        bytes = sizeof(CsBatchRequest);
    CsBatchRequest *b = calloc(1, bytes);
    if (!b)
        return CS_OUT_OF_RESOURCES;
    b->ReqType = CS_STORAGE_IO;
    memcpy(&b->u.StorageIo, r,
           offsetof(CsStorageRequest, u.BlockIo.Range) +
               (size_t)r->u.BlockIo.NumRanges * sizeof(CSBlockRange));
    CS_STATUS s = cs_queue_one(b, ctx, cb, ev, req, value);
    free(b);
    return s;
}
