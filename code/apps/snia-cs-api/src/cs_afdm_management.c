/* SNIA AFDM 管理与复制：维护公开内存句柄，转发插件内存操作。 */
#include "cs_internal.h"
static void memory_destroy(struct cs_object *o) { cs_handle_release(&((struct cs_memory *)o)->dev->base); }
CS_STATUS csAllocMem(CS_DEV_HANDLE handle, int bytes, const CsMemFlags *flags, CS_MEM_HANDLE *out, CS_MEM_PTR *va)
{
    if (!out || !flags || bytes <= 0) return CS_INVALID_ARG;
    *out = NULL;
    struct cs_device *d = (void *)cs_handle_acquire(handle, DEV);
    if (!d) return CS_INVALID_HANDLE;
    struct cs_memory *m = (void *)cs_handle_create(sizeof(*m), MEM, memory_destroy);
    if (!m) { cs_handle_release(&d->base); return CS_OUT_OF_RESOURCES; }
    m->dev = d;
    CS_STATUS s = d->plugin->ops->allocMem(d->private_data, bytes, flags, &m->private_data, &m->bytes, va);
    if (s != CS_SUCCESS) { cs_handle_retain(&m->base); cs_handle_retire(&m->base); cs_handle_release(&m->base); return s; }
    *out = (void *)m->base.id; return s;
}
CS_STATUS csInitMem(CS_MEM_HANDLE handle, unsigned long offset, int bytes, const CsMemFlags *flags)
{
    if (!flags || bytes <= 0) return CS_INVALID_ARG;
    struct cs_memory *m = (void *)cs_handle_acquire(handle, MEM);
    if (!m) return CS_INVALID_HANDLE;
    CS_STATUS s = offset > m->bytes || (u64)bytes > m->bytes - offset ? CS_INVALID_LENGTH
        : m->dev->plugin->ops->initMem(m->dev->private_data, m->private_data, offset, bytes, flags);
    cs_handle_release(&m->base); return s;
}
CS_STATUS csFreeMem(CS_MEM_HANDLE handle, const CsMemFlags *flags)
{
    if (!flags) return CS_INVALID_ARG;
    struct cs_memory *m = (void *)cs_handle_acquire(handle, MEM);
    if (!m) return CS_INVALID_HANDLE;
    CS_STATUS s = cs_handle_retire(&m->base);
    if (s != CS_SUCCESS) { cs_handle_release(&m->base); return CS_MEMORY_IN_USE; }
    s = m->dev->plugin->ops->freeMem(m->dev->private_data, m->private_data, flags);
    if (s != CS_SUCCESS) cs_handle_restore(&m->base);
    cs_handle_release(&m->base); return s;
}
CS_STATUS csQueueCopyMemRequest(const CsCopyMemRequest *r, void *ctx, csQueueCallbackFn cb,
                                CS_EVT_HANDLE ev, CS_REQ_HANDLE *req, u64 *value)
{
    if (!r) return CS_INVALID_ARG;
    CsBatchRequest b = {.ReqType = CS_COPY_AFDM}; b.u.CopyMem = *r;
    return cs_request_queue(&b, ctx, cb, ev, req, value);
}
