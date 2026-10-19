/* 跨 queue* API 共享：复制/绑定请求、保留资源，执行同步或异步请求并发布完成。 */
#include "cs_internal.h"
#include <stddef.h>
pthread_mutex_t cs_csf_binding_lock = PTHREAD_MUTEX_INITIALIZER;
static CS_STATUS bind(struct cs_prepared_request *t, void *handle, unsigned kind, void **token)
{
    struct cs_object *o = cs_handle_acquire(handle, kind);
    if (!o) return kind == MEM ? CS_UNKNOWN_MEMORY : kind == FUNCTION ? CS_INVALID_CSF_ID : CS_INVALID_HANDLE;
    struct cs_device *d = kind == DEV ? (void *)o : kind == MEM ? ((struct cs_memory *)o)->dev : ((struct cs_function *)o)->dev;
    if (t->npins >= CS_API_MAX_ARGS + 4 || (t->dev && d != t->dev)) { cs_handle_release(o); return CS_INVALID_ARG; }
    t->dev = d; t->pins[t->npins++] = o;
    *token = kind == DEV ? d->private_data : kind == MEM ? ((struct cs_memory *)o)->private_data : ((struct cs_function *)o)->private_data;
    return CS_SUCCESS;
}
void cs_request_release(struct cs_prepared_request *t)
{
    if (!t) return;
    for (unsigned i = 0; i < t->npins; i++) cs_handle_release(t->pins[i]);
    free(t->request);
    free(t);
}
static CS_STATUS bind_memory(struct cs_prepared_request *t, const CsDevAFDM *src, CsDevAFDM *dst, u64 bytes)
{
    CS_STATUS s = bind(t, src->MemHandle, MEM, &dst->MemHandle);
    if (s != CS_SUCCESS) return s;
    struct cs_memory *m = (void *)t->pins[t->npins - 1];
    if (src->ByteOffset > m->bytes || bytes > m->bytes - src->ByteOffset) return CS_INVALID_LENGTH;
    return CS_SUCCESS;
}
CS_STATUS cs_request_prepare(const CsBatchRequest *r, struct cs_prepared_request **out)
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
    struct cs_prepared_request *t = calloc(1, sizeof(*t));
    if (!t) return CS_OUT_OF_RESOURCES;
    t->request = malloc(bytes);
    if (!t->request) { cs_request_release(t); return CS_OUT_OF_RESOURCES; }
    memcpy(t->request, r, bytes);
    CsBatchRequest *b = t->request;
    CS_STATUS s;
    pthread_mutex_lock(&cs_csf_binding_lock);
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
    if (s == CS_SUCCESS && t->dev->plugin->ops->validateRequest)
        s = t->dev->plugin->ops->validateRequest(t->dev->private_data, b);
    pthread_mutex_unlock(&cs_csf_binding_lock);
    if (s != CS_SUCCESS) { cs_request_release(t); return s; }
    *out = t; return CS_SUCCESS;
}
CS_STATUS cs_request_execute(struct cs_prepared_request *t, u64 *value)
{
    u64 ignored = 0;
    if (!value) value = &ignored;
    *value = 0;
    const struct cs_backend_ops *ops = t->dev->plugin->ops;
    void *device = t->dev->private_data;
    const CsBatchRequest *r = t->request;
    switch (r->ReqType) {
    case CS_QUEUE_COMPUTE:
        return ops->queueComputeRequest ? ops->queueComputeRequest(device, &r->u.Compute, value) : CS_UNSUPPORTED;
    case CS_COPY_AFDM:
        return ops->queueCopyMemRequest ? ops->queueCopyMemRequest(device, &r->u.CopyMem, value) : CS_UNSUPPORTED;
    case CS_STORAGE_IO:
        return ops->queueStorageRequest ? ops->queueStorageRequest(device, &r->u.StorageIo, value) : CS_UNSUPPORTED;
    default:
        return CS_INVALID_OPTION;
    }
}
static bool is_cancelled(void *context) { return atomic_load((atomic_bool *)context); }
CS_STATUS cs_request_execute_batch_entry(struct cs_prepared_request *t, atomic_bool *cancelled, void *scope, u64 *value)
{
    const struct cs_backend_ops *ops = t->dev->plugin->ops;
    return ops->executeBatchEntry ? ops->executeBatchEntry(t->dev->private_data, t->request, is_cancelled, cancelled, scope, value)
                              : cs_request_execute(t, value);
}
CS_STATUS cs_request_queue(const CsBatchRequest *r, void *ctx, csQueueCallbackFn cb,
                       CS_EVT_HANDLE ev, CS_REQ_HANDLE *req, u64 *value)
{
    struct cs_prepared_request *t;
    CS_STATUS s = cs_request_prepare(r, &t);
    return s == CS_SUCCESS ? cs_request_submit(t, NULL, ctx, cb, ev, req, value) : s;
}

static unsigned active_requests;
static void *request_main(void *opaque)
{
    struct cs_request *r = opaque;
    u64 value = 0;
    CS_STATUS status;
    pthread_mutex_lock(&cs_handle_lock);
    atomic_store(&r->started, true);
    bool skip = atomic_load(&r->cancelled);
    pthread_mutex_unlock(&cs_handle_lock);
    if (skip)
        status = CS_ERROR_IN_EXECUTION;
    else
        status =
            r->batch ? cs_batch_run(r->batch, &r->cancelled, &value) : cs_request_execute(r->task, &value);
    if (atomic_load(&r->cancelled))
        status = CS_ERROR_IN_EXECUTION;
    cs_request_release(r->task);
    r->task = NULL;
    if (r->batch) {
        cs_handle_release(&r->batch->base);
        r->batch = NULL;
    }
    cs_handle_remove(&r->base);
    if (r->event) {
        struct cs_completion *c = r->completion;
        c->status = status;
        c->value = value;
        c->context = r->context;
        pthread_mutex_lock(&cs_handle_lock);
        if (r->event->tail)
            r->event->tail->next = c;
        else
            r->event->head = c;
        r->event->tail = c;
        r->event->pending--;
        /* Registry ownership keeps this event alive; publish completion and
         * release the request's pin atomically so a successful poll can free it. */
        r->event->base.refs--;
        pthread_mutex_unlock(&cs_handle_lock);
    }
    if (r->callback)
        r->callback(r->context, status, value);
    pthread_mutex_lock(&cs_handle_lock);
    active_requests--;
    pthread_mutex_unlock(&cs_handle_lock);
    cs_handle_release(&r->base);
    return NULL;
}
CS_STATUS cs_request_submit(struct cs_prepared_request *task, struct cs_batch *batch, void *ctx, csQueueCallbackFn cb,
                    CS_EVT_HANDLE ev, CS_REQ_HANDLE *out, u64 *value)
{
    if (out)
        *out = NULL;
    if (value)
        *value = 0;
    CS_STATUS s = CS_SUCCESS;
    struct cs_event *event = NULL;
    if (cb && ev)
        s = CS_INVALID_ARG;
    if (s == CS_SUCCESS && ev) {
        event = (void *)cs_handle_acquire(ev, EVENT);
        if (!event)
            s = CS_INVALID_HANDLE;
    }
    if (s != CS_SUCCESS) {
        cs_request_release(task);
        if (batch)
            cs_handle_release(&batch->base);
        return s;
    }
    if (!cb && !event) {
        atomic_bool cancelled = ATOMIC_VAR_INIT(false);
        s = batch ? cs_batch_run(batch, &cancelled, value) : cs_request_execute(task, value);
        cs_request_release(task);
        if (batch)
            cs_handle_release(&batch->base);
        return s;
    }
    pthread_mutex_lock(&cs_handle_lock);
    bool full = active_requests >= 64;
    if (!full)
        active_requests++;
    pthread_mutex_unlock(&cs_handle_lock);
    if (full) {
        s = CS_OUT_OF_RESOURCES;
        goto fail;
    }
    struct cs_request *r = (void *)cs_handle_create(sizeof(*r), REQUEST, NULL);
    if (!r) {
        pthread_mutex_lock(&cs_handle_lock);
        active_requests--;
        pthread_mutex_unlock(&cs_handle_lock);
        s = CS_OUT_OF_RESOURCES;
        goto fail;
    }
    r->task = task;
    r->batch = batch;
    r->event = event;
    r->context = ctx;
    r->callback = cb;
    atomic_init(&r->started, false);
    atomic_init(&r->cancelled, false);
    if (event) {
        r->completion = calloc(1, sizeof(*r->completion));
        if (!r->completion) {
            s = CS_OUT_OF_RESOURCES;
            goto launch_fail;
        }
        pthread_mutex_lock(&cs_handle_lock);
        event->pending++;
        pthread_mutex_unlock(&cs_handle_lock);
    }
    cs_handle_retain(&r->base); /* worker reference */
    if (out)
        *out = (void *)r->base.id;
    pthread_t thread;
    if (pthread_create(&thread, NULL, request_main, r)) {
        if (out)
            *out = NULL;
        cs_handle_release(&r->base);
        if (event) {
            pthread_mutex_lock(&cs_handle_lock);
            event->pending--;
            pthread_mutex_unlock(&cs_handle_lock);
        }
        s = CS_OUT_OF_RESOURCES;
        goto launch_fail;
    }
    pthread_detach(thread);
    return CS_QUEUED;
launch_fail:
    free(r->completion);
    cs_handle_remove(&r->base);
    free(r);
    pthread_mutex_lock(&cs_handle_lock);
    active_requests--;
    pthread_mutex_unlock(&cs_handle_lock);
fail:
    if (event)
        cs_handle_release(&event->base);
    cs_request_release(task);
    if (batch)
        cs_handle_release(&batch->base);
    return s;
}
