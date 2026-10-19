/* SNIA CSF 调度：获取函数句柄、计算请求、取消及 CSEE 下载。 */
#include "cs_internal.h"
#include <stdarg.h>
#include <stddef.h>
void csHelperSetComputeArg(CsComputeArg *a, CS_COMPUTE_ARG_TYPE type, ...)
{
    if (!a)
        return;
    memset(a, 0, sizeof(*a));
    a->Type = type;
    va_list ap;
    va_start(ap, type);
    switch (type) {
    case CS_AFDM_TYPE:
        a->u.DevMem.MemHandle = va_arg(ap, CS_MEM_HANDLE);
        a->u.DevMem.ByteOffset = va_arg(ap, unsigned long);
        break;
    case CS_8BIT_VALUE_TYPE:
        a->u.Value8 = (u8)va_arg(ap, int);
        break;
    case CS_16BIT_VALUE_TYPE:
        a->u.Value16 = (u16)va_arg(ap, int);
        break;
    case CS_32BIT_VALUE_TYPE:
        a->u.Value32 = va_arg(ap, u32);
        break;
    case CS_64BIT_VALUE_TYPE:
        a->u.Value64 = va_arg(ap, u64);
        break;
    default:
        break;
    }
    va_end(ap);
}
CS_STATUS csQueueComputeRequest(const CsComputeRequest *r, void *ctx, csQueueCallbackFn cb,
                                CS_EVT_HANDLE ev, CS_REQ_HANDLE *req, u64 *value)
{
    if (!r || r->NumArgs < 0 || r->NumArgs > CS_API_MAX_ARGS)
        return CS_INVALID_ARG;
    size_t bytes =
        offsetof(CsBatchRequest, u.Compute.Args) + (size_t)r->NumArgs * sizeof(CsComputeArg);
    if (bytes < sizeof(CsBatchRequest))
        bytes = sizeof(CsBatchRequest);
    CsBatchRequest *b = calloc(1, bytes);
    if (!b)
        return CS_OUT_OF_RESOURCES;
    b->ReqType = CS_QUEUE_COMPUTE;
    memcpy(&b->u.Compute, r,
           offsetof(CsComputeRequest, Args) + (size_t)r->NumArgs * sizeof(CsComputeArg));
    CS_STATUS s = cs_request_queue(b, ctx, cb, ev, req, value);
    free(b);
    return s;
}
CS_STATUS csAbortRequest(CS_REQ_HANDLE h)
{
    struct cs_request *r = (void *)cs_handle_acquire(h, REQUEST);
    if (!r)
        return CS_INVALID_HANDLE;
    /* Running legacy native functions are not safely preemptible. Do not
     * report an abort success while such a function can still write AFDM. */
    pthread_mutex_lock(&cs_handle_lock);
    CS_STATUS s = atomic_load(&r->started) ? CS_UNSUPPORTED : CS_SUCCESS;
    if (s == CS_SUCCESS)
        atomic_store(&r->cancelled, true);
    pthread_mutex_unlock(&cs_handle_lock);
    cs_handle_release(&r->base);
    return s;
}
static void function_destroy(struct cs_object *o) { cs_handle_release(&((struct cs_function *)o)->dev->base); }
CS_STATUS csGetCSFId(CS_DEV_HANDLE handle, const char *name, u64 global, int *length, int *count, CSFIdinfo *out)
{
    if (!length || !count || (!name && !global) || (name && global)) return CS_INVALID_ARG;
    if (name && (!*name || strnlen(name, 32) == 32)) return CS_INVALID_CSF_NAME;
    struct cs_device *d = (void *)cs_handle_acquire(handle, DEV);
    if (!d) return CS_INVALID_HANDLE;
    const struct cs_backend_ops *ops = d->plugin->ops;
    pthread_mutex_lock(&cs_csf_binding_lock);
    CS_STATUS s = ops->getCSFId && ops->getCSFResourceId ? ops->getCSFId(d->private_data, name, global, length, count, out) : CS_UNSUPPORTED;
    if (s == CS_SUCCESS) {
        int i;
        for (i = 0; i < *count; i++) {
            struct cs_function *f = (void *)cs_handle_create(sizeof(*f), FUNCTION, function_destroy);
            if (!f) { s = CS_OUT_OF_RESOURCES; break; }
            f->dev = d; cs_handle_retain(&d->base);
            f->private_data = out[i].CSFId; f->id = ops->getCSFResourceId(d->private_data, f->private_data);
            out[i].CSFId = (void *)f->base.id;
        }
        if (s != CS_SUCCESS) {
            for (int j = 0; j < i; j++) {
                struct cs_object *f = cs_handle_acquire(out[j].CSFId, FUNCTION);
                cs_handle_retire(f); cs_handle_release(f); out[j].CSFId = NULL;
            }
        }
    }
    pthread_mutex_unlock(&cs_csf_binding_lock);
    cs_handle_release(&d->base); return s;
}
CS_STATUS csCSEEDownload(CS_DEV_HANDLE handle, const CsCSEEDownloadInfo *info, u32 *out)
{
    if (!info || !out) return CS_INVALID_ARG;
    struct cs_device *d = (void *)cs_handle_acquire(handle, DEV);
    if (!d) return CS_INVALID_HANDLE;
    CS_STATUS s = d->plugin->ops->CSEEDownload ? d->plugin->ops->CSEEDownload(d->private_data, info, out) : CS_UNSUPPORTED;
    cs_handle_release(&d->base); return s;
}
