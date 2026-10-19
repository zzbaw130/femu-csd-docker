#include "internal.h"
CS_STATUS csAllocBatchRequest(CS_BATCH_MODE mode, int max, CS_BATCH_HANDLE *out)
{ return cs_batch_alloc(mode, max, out); }
CS_STATUS csFreeBatchRequest(CS_BATCH_HANDLE handle)
{ return cs_batch_free(handle); }
CS_STATUS csConfigureBatchEntry(CS_BATCH_HANDLE handle, CS_BATCH_CONFIG_TYPE action,
    const CsBatchRequest *req, CS_BATCH_INDEX before, CS_BATCH_INDEX after, CS_BATCH_INDEX *curr)
{ return cs_batch_configure(handle, action, req, before, after, curr); }
CS_STATUS csHelperResizeBatchRequest(CS_BATCH_HANDLE handle, int max)
{ return cs_batch_resize(handle, max); }
CS_STATUS csQueueBatchRequest(CS_BATCH_HANDLE handle, void *ctx, csQueueCallbackFn cb,
    CS_EVT_HANDLE ev, CS_REQ_HANDLE *req, u64 *value)
{ return cs_batch_queue(handle, ctx, cb, ev, req, value); }
