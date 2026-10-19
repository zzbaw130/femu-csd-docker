#include "internal.h"
#include <stddef.h>
pthread_mutex_t cs_binding_lock = PTHREAD_MUTEX_INITIALIZER;
static CS_STATUS bind(struct cs_task *t, void *handle, unsigned kind, void **token)
{
    struct cs_object *o = cs_get(handle, kind);
    if (!o) return kind == MEM ? CS_UNKNOWN_MEMORY : kind == FUNCTION ? CS_INVALID_CSF_ID : CS_INVALID_HANDLE;
    struct cs_device *d = kind == DEV ? (void *)o : kind == MEM ? ((struct cs_memory *)o)->dev : ((struct cs_function *)o)->dev;
    if (t->npins >= CS_API_MAX_ARGS + 4 || (t->dev && d != t->dev)) { cs_put(o); return CS_INVALID_ARG; }
    t->dev = d; t->pins[t->npins++] = o;
    *token = kind == DEV ? d->private_data : kind == MEM ? ((struct cs_memory *)o)->private_data : ((struct cs_function *)o)->private_data;
    return CS_SUCCESS;
}
void cs_task_free(struct cs_task *t)
{
    if (!t) return;
    for (unsigned i = 0; i < t->npins; i++) cs_put(t->pins[i]);
    free(t->req); free(t->backend_req); free(t);
}
static CS_STATUS bind_memory(struct cs_task *t, const CsDevAFDM *src, CsDevAFDM *dst, u64 bytes)
{
    CS_STATUS s = bind(t, src->MemHandle, MEM, &dst->MemHandle);
    if (s != CS_SUCCESS) return s;
    struct cs_memory *m = (void *)t->pins[t->npins - 1];
    if (src->ByteOffset > m->bytes || bytes > m->bytes - src->ByteOffset) return CS_INVALID_LENGTH;
    return CS_SUCCESS;
}
CS_STATUS cs_task_clone(const CsBatchRequest *r, struct cs_task **out)
{
    if (!r || !out) return CS_INVALID_ARG;
    *out = NULL;
    size_t bytes = sizeof(*r);
    if (r->ReqType == CS_QUEUE_COMPUTE) {
        int n = r->u.Compute.NumArgs;
        if (n < 0 || n > CS_API_MAX_ARGS) return CS_INVALID_ARG;
        bytes = offsetof(CsBatchRequest, u.Compute.Args) + (size_t)n * sizeof(CsComputeArg);
    } else if (r->ReqType == CS_STORAGE_IO) {
        if (r->u.StorageIo.Mode == CS_STORAGE_BLOCK_IO) {
            int n = r->u.StorageIo.u.BlockIo.NumRanges;
            if (n <= 0 || n > 4096) return CS_INVALID_ARG;
            bytes = offsetof(CsBatchRequest, u.StorageIo.u.BlockIo.Range) + (size_t)n * sizeof(CSBlockRange);
        } else if (r->u.StorageIo.Mode != CS_STORAGE_FILE_IO) return CS_INVALID_OPTION;
    } else if (r->ReqType != CS_COPY_AFDM) return CS_INVALID_OPTION;
    if (bytes < sizeof(*r)) bytes = sizeof(*r);
    struct cs_task *t = calloc(1, sizeof(*t));
    if (!t) return CS_OUT_OF_RESOURCES;
    t->req = malloc(bytes); t->backend_req = malloc(bytes);
    if (!t->req || !t->backend_req) { cs_task_free(t); return CS_OUT_OF_RESOURCES; }
    memcpy(t->req, r, bytes); memcpy(t->backend_req, r, bytes);
    CsBatchRequest *b = t->backend_req;
    CS_STATUS s;
    pthread_mutex_lock(&cs_binding_lock);
    if (r->ReqType == CS_QUEUE_COMPUTE) {
        s = bind(t, r->u.Compute.CSFId, FUNCTION, &b->u.Compute.CSFId);
        for (int i = 0; s == CS_SUCCESS && i < r->u.Compute.NumArgs; i++)
            if (r->u.Compute.Args[i].Type == CS_AFDM_TYPE)
                s = bind_memory(t, &r->u.Compute.Args[i].u.DevMem, &b->u.Compute.Args[i].u.DevMem, 0);
    } else if (r->ReqType == CS_COPY_AFDM) {
        const CsCopyMemRequest *c = &r->u.CopyMem;
        s = c->Bytes <= 0 ? CS_INVALID_ARG : bind_memory(t, &c->DevMem, &b->u.CopyMem.DevMem, c->Bytes);
        if (s == CS_SUCCESS && c->Type == CS_COPY_WITHIN_DEVICE)
            s = bind_memory(t, &c->u.SrcDevMem, &b->u.CopyMem.u.SrcDevMem, c->Bytes);
        else if (s == CS_SUCCESS && c->Type != CS_COPY_TO_DEVICE && c->Type != CS_COPY_FROM_DEVICE)
            s = CS_INVALID_OPTION;
        else if (s == CS_SUCCESS && !c->u.HostVAddress) s = CS_INVALID_ARG;
    } else {
        const CsStorageRequest *sr = &r->u.StorageIo;
        s = bind(t, sr->DevHandle, DEV, &b->u.StorageIo.DevHandle);
        if (s == CS_SUCCESS) {
            if (sr->Mode == CS_STORAGE_BLOCK_IO)
                s = bind_memory(t, &sr->u.BlockIo.DevMem, &b->u.StorageIo.u.BlockIo.DevMem, 0);
            else s = bind_memory(t, &sr->u.FileIo.DevMem, &b->u.StorageIo.u.FileIo.DevMem, sr->u.FileIo.Bytes);
        }
    }
    if (s == CS_SUCCESS) s = t->dev->plugin->ops->validate(t->dev->private_data, b);
    pthread_mutex_unlock(&cs_binding_lock);
    if (s != CS_SUCCESS) { cs_task_free(t); return s; }
    *out = t; return CS_SUCCESS;
}
CS_STATUS cs_task_run(struct cs_task *t, u64 *value)
{
    u64 ignored = 0;
    return t->dev->plugin->ops->execute(t->dev->private_data, t->backend_req, value ? value : &ignored);
}
static bool is_cancelled(void *context) { return atomic_load((atomic_bool *)context); }
CS_STATUS cs_task_run_batch(struct cs_task *t, atomic_bool *cancelled, void *scope, u64 *value)
{
    const struct cs_backend_ops *ops = t->dev->plugin->ops;
    return ops->execute_batch ? ops->execute_batch(t->dev->private_data, t->backend_req, is_cancelled, cancelled, scope, value)
                              : cs_task_run(t, value);
}
CS_STATUS cs_queue_one(const CsBatchRequest *r, void *ctx, csQueueCallbackFn cb,
                       CS_EVT_HANDLE ev, CS_REQ_HANDLE *req, u64 *value)
{
    struct cs_task *t;
    CS_STATUS s = cs_task_clone(r, &t);
    return s == CS_SUCCESS ? cs_submit(t, NULL, ctx, cb, ev, req, value) : s;
}
