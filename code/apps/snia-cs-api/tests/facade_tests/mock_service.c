/* A non-NVMe adapter. No core or FEMU headers are used. */
#include "cs_backend.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct device { int sum, reserved; };
struct memory { void *device; unsigned char *data; u64 bytes; };
static CS_STATUS output(int *n, void *out, size_t bytes)
{
    if (!n) return CS_INVALID_ARG;
    int have = *n; *n = bytes;
    return !out || have < (int)bytes ? CS_INVALID_LENGTH : CS_SUCCESS;
}
static CS_STATUS queryCSxList(int *n, char *out)
{
    const char name[] = "mock://device";
    CS_STATUS s = output(n, out, sizeof(name));
    if (s == CS_SUCCESS) memcpy(out, name, sizeof(name));
    return s;
}
static CS_STATUS getCSxFromPath(const char *path, unsigned *n, char *out)
{
    if (strcmp(path, "mock://device")) return CS_NO_MATCHING_DEVICE;
    int bytes = *n; CS_STATUS s = queryCSxList(&bytes, out); *n = bytes; return s;
}
static CS_STATUS openCSx(const char *path, void *ctx, void **out, unsigned *workers)
{
    (void)ctx;
    if (strcmp(path, "mock://device")) return CS_NO_MATCHING_DEVICE;
    *out = calloc(1, sizeof(struct device)); *workers = 2;
    return *out ? CS_SUCCESS : CS_OUT_OF_RESOURCES;
}
static void closeCSx(void *device) { free(device); }
static CS_STATUS allocMem(void *device, int n, const CsMemFlags *flags, void **out, u64 *bytes, CS_MEM_PTR *va)
{
    (void)flags; (void)va;
    struct memory *m = calloc(1, sizeof(*m));
    if (!m) return CS_OUT_OF_RESOURCES;
    m->data = calloc(1, n);
    if (!m->data) { free(m); return CS_OUT_OF_RESOURCES; }
    m->device = device; m->bytes = n; *out = m; *bytes = n; return CS_SUCCESS;
}
static CS_STATUS freeMem(void *device, void *memory, const CsMemFlags *flags)
{
    (void)flags;
    struct memory *m = memory;
    if (m->device != device) return CS_INVALID_ARG;
    free(m->data); free(m); return CS_SUCCESS;
}
static CS_STATUS initMem(void *device, void *memory, unsigned long offset, int bytes, const CsMemFlags *flags)
{
    struct memory *m = memory;
    if (m->device != device) return CS_INVALID_ARG;
    memset(m->data + offset, flags->Flags & CS_FDM_FILL ? flags->FillValue : 0, bytes);
    return CS_SUCCESS;
}
static CS_STATUS getCSFId(void *device, const char *name, u64 global, int *length, int *count, CSFIdinfo *out)
{
    (void)global;
    struct device *d = device;
    if (!name || (strcmp(name, "sum") && strcmp(name, "reserved"))) return CS_INVALID_CSF_NAME;
    *count = 1;
    CS_STATUS s = output(length, out, sizeof(*out));
    if (s == CS_SUCCESS) *out = (CSFIdinfo){.CSFId = !strcmp(name, "sum") ? (void *)&d->sum : (void *)&d->reserved};
    return s;
}
static u32 getCSFResourceId(void *device, CS_CSF_ID id)
{
    struct device *d = device;
    return id == &d->sum ? 77 : 78;
}
static CS_STATUS validateRequest(void *device, const CsBatchRequest *req)
{
    if (req->ReqType == CS_STORAGE_IO) return CS_UNSUPPORTED;
    if (req->ReqType == CS_COPY_AFDM) {
        struct memory *m = req->u.CopyMem.DevMem.MemHandle;
        return m->device == device ? CS_SUCCESS : CS_INVALID_ARG;
    }
    struct device *d = device;
    return req->u.Compute.CSFId == &d->sum || req->u.Compute.CSFId == &d->reserved ? CS_SUCCESS : CS_INVALID_CSF_ID;
}
static CS_STATUS queueCopyMemRequest(void *device, const CsCopyMemRequest *r, u64 *value)
{
    struct memory *m = r->DevMem.MemHandle;
    if (m->device != device) return CS_INVALID_ARG;
    if (r->Type == CS_COPY_TO_DEVICE)
        memcpy(m->data + r->DevMem.ByteOffset, r->u.HostVAddress, r->Bytes);
    else if (r->Type == CS_COPY_FROM_DEVICE)
        memcpy(r->u.HostVAddress, m->data + r->DevMem.ByteOffset, r->Bytes);
    else return CS_UNSUPPORTED;
    *value = r->Bytes;
    return CS_SUCCESS;
}
static CS_STATUS queueComputeRequest(void *device, const CsComputeRequest *r, u64 *value)
{
    struct device *d = device;
    if (r->CSFId == &d->reserved) { *value = (UINT64_C(1) << 60) | 128; return CS_SUCCESS; }
    *value = 0;
    for (int i = 0; i < r->NumArgs; i++) {
        const CsComputeArg *a = &r->Args[i];
        if (a->Type == CS_AFDM_TYPE) {
            struct memory *m = a->u.DevMem.MemHandle;
            if (m->device != device) return CS_INVALID_ARG;
            *value += m->data[a->u.DevMem.ByteOffset];
        } else if (a->Type == CS_64BIT_VALUE_TYPE) *value += a->u.Value64;
        else return CS_UNSUPPORTED_TYPE;
    }
    usleep(10000);
    return CS_SUCCESS;
}
static const struct cs_backend_ops ops = {
    .abi = CS_BACKEND_ABI, .name = "test-service", .type = CS_PLUGIN_CUSTOM,
    .queryCSxList = queryCSxList, .getCSxFromPath = getCSxFromPath, .openCSx = openCSx, .closeCSx = closeCSx,
    .getCSFId = getCSFId, .getCSFResourceId = getCSFResourceId,
    .allocMem = allocMem, .freeMem = freeMem, .initMem = initMem,
    .validateRequest = validateRequest,
    .queueComputeRequest = queueComputeRequest, .queueCopyMemRequest = queueCopyMemRequest
};
__attribute__((visibility("default")))
const struct cs_backend_ops *cs_backend_create(void) { return &ops; }
__attribute__((visibility("default")))
void cs_backend_destroy(const struct cs_backend_ops *backend) { (void)backend; }
