#include "femu.h"
CS_STATUS femu_open(const char *name, void *context, void **out, unsigned *workers)
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
void femu_close(void *device)
{
    struct cs_device *d = device;
    cs_spill_detach(d); cs_transport_close(d); free(d);
}
static u32 function_id(void *device, CS_CSF_ID function)
{
    (void)device;
    return (u32)(uintptr_t)function;
}
static CS_STATUS execute(void *device, const CsBatchRequest *req, u64 *value)
{
    struct cs_task task = {.dev = device, .req = req};
    return cs_task_run(&task, value);
}
static void scope_end(void *device, void *scope, bool success)
{
    cs_spill_scope_end(device, scope, success);
}
static const struct cs_backend_ops ops = {
    .abi = CS_BACKEND_ABI, .name = "femu", .type = CS_PLUGIN_NVME,
    .enumerate = femu_enumerate, .resolve = femu_resolve,
    .open = femu_open, .close = femu_close,
    .csf_list = femu_csf_list, .get_csf = femu_get_csf, .function_id = function_id,
    .alloc = femu_alloc, .free_mem = femu_free, .init_mem = femu_init,
    .validate = femu_validate, .execute = execute, .execute_batch = femu_resume,
    .scope_end = scope_end, .properties = femu_properties,
    .download_csf = femu_download, .config = femu_config
};
__attribute__((visibility("default")))
const struct cs_backend_ops *cs_backend_create(void) { return &ops; }
__attribute__((visibility("default")))
void cs_backend_destroy(const struct cs_backend_ops *backend) { (void)backend; cs_catalog_clear(); }
