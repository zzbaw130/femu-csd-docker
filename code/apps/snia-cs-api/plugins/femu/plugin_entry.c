#include "femu.h"
CS_STATUS femu_openCSx(const char *name, void *context, void **out, unsigned *workers)
{
    (void)context;
    *out = NULL;
    struct cs_device *d = calloc(1, sizeof(*d));
    if (!d) return CS_OUT_OF_RESOURCES;
    d->fd = d->admin = -1;
    if (cs_transport_open(d, name) < 0 || cs_profile_load(d, name) < 0) {
        cs_transport_close(d); free(d); return CS_DEVICE_NOT_AVAILABLE;
    }
    *out = d; *workers = d->caps.workers;
    return CS_SUCCESS;
}
void femu_closeCSx(void *device)
{
    struct cs_device *d = device;
    cs_spill_detach(d); cs_transport_close(d); free(d);
}
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
