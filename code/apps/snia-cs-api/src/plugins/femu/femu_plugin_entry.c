/* C plugin factory and SNIA-named device operation table. */
#include "femu_common.h"
static u32 getCSFResourceId(void *device, CS_CSF_ID function)
{
    (void)device;
    return (u32)(uintptr_t)function;
}
static void finishBatchExecution(void *device, void *scope, bool success)
{
    cs_spill_scope_end(device, scope, success);
}
static const struct cs_backend_ops ops = {
    .abi = CS_BACKEND_ABI, .name = "femu", .type = CS_PLUGIN_NVME,
    .queryCSxList = femu_queryCSxList, .getCSxFromPath = femu_getCSxFromPath,
    .openCSx = femu_openCSx, .closeCSx = femu_closeCSx,
    .queryCSFList = femu_queryCSFList, .getCSFId = femu_getCSFId, .getCSFResourceId = getCSFResourceId,
    .allocMem = femu_allocMem, .freeMem = femu_freeMem, .initMem = femu_initMem,
    .validateRequest = femu_validateRequest,
    .queueComputeRequest = femu_queueComputeRequest,
    .queueCopyMemRequest = femu_queueCopyMemRequest,
    .queueStorageRequest = femu_queueStorageRequest,
    .executeBatchEntry = femu_executeBatchEntry,
    .finishBatchExecution = finishBatchExecution, .queryDeviceProperties = femu_queryDeviceProperties,
    .CSFDownload = femu_CSFDownload, .config = femu_config
};
__attribute__((visibility("default")))
const struct cs_backend_ops *cs_backend_create(void) { return &ops; }
__attribute__((visibility("default")))
void cs_backend_destroy(const struct cs_backend_ops *backend) { (void)backend; cs_catalog_clear(); }
