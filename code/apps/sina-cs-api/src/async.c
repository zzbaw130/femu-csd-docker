#include "internal.h"
static unsigned active_requests;
static void event_destroy(struct cs_object *o)
{
    struct cs_event *e = (void *)o;
    while (e->head) {
        struct cs_completion *c = e->head;
        e->head = c->next;
        free(c);
    }
}
CS_STATUS csCreateEvent(CS_EVT_HANDLE *out)
{
    if (!out)
        return CS_INVALID_ARG;
    struct cs_event *e = (void *)cs_new(sizeof(*e), EVENT, event_destroy);
    *out = e ? (void *)e->base.id : NULL;
    return e ? CS_SUCCESS : CS_OUT_OF_RESOURCES;
}
CS_STATUS csDeleteEvent(CS_EVT_HANDLE h)
{
    struct cs_event *e = (void *)cs_get(h, EVENT);
    if (!e)
        return CS_INVALID_HANDLE;
    CS_STATUS s = cs_retire(&e->base);
    cs_put(&e->base);
    return s;
}
CS_STATUS csPollEvent(CS_EVT_HANDLE h, void **ctx, u64 *value)
{
    struct cs_event *e = (void *)cs_get(h, EVENT);
    if (!e)
        return CS_INVALID_HANDLE;
    pthread_mutex_lock(&cs_lock);
    struct cs_completion *c = e->head;
    if (c) {
        e->head = c->next;
        if (!e->head)
            e->tail = NULL;
    }
    pthread_mutex_unlock(&cs_lock);
    cs_put(&e->base);
    if (!c)
        return CS_NOT_DONE;
    if (ctx)
        *ctx = c->context;
    if (value)
        *value = c->value;
    CS_STATUS s = c->status;
    free(c);
    return s;
}
static void *request_main(void *opaque)
{
    struct cs_request *r = opaque;
    u64 value = 0;
    CS_STATUS status;
    pthread_mutex_lock(&cs_lock);
    atomic_store(&r->started, true);
    bool skip = atomic_load(&r->cancelled);
    pthread_mutex_unlock(&cs_lock);
    if (skip)
        status = CS_ERROR_IN_EXECUTION;
    else
        status =
            r->batch ? cs_batch_run(r->batch, &r->cancelled, &value) : cs_task_run(r->task, &value);
    if (atomic_load(&r->cancelled))
        status = CS_ERROR_IN_EXECUTION;
    cs_task_free(r->task);
    r->task = NULL;
    if (r->batch) {
        cs_put(&r->batch->base);
        r->batch = NULL;
    }
    cs_unlink(&r->base);
    if (r->event) {
        struct cs_completion *c = r->completion;
        c->status = status;
        c->value = value;
        c->context = r->context;
        pthread_mutex_lock(&cs_lock);
        if (r->event->tail)
            r->event->tail->next = c;
        else
            r->event->head = c;
        r->event->tail = c;
        r->event->pending--;
        /* Registry ownership keeps this event alive; publish completion and
         * release the request's pin atomically so a successful poll can free it. */
        r->event->base.refs--;
        pthread_mutex_unlock(&cs_lock);
    }
    if (r->callback)
        r->callback(r->context, status, value);
    pthread_mutex_lock(&cs_lock);
    active_requests--;
    pthread_mutex_unlock(&cs_lock);
    cs_put(&r->base);
    return NULL;
}
CS_STATUS cs_submit(struct cs_task *task, struct cs_batch *batch, void *ctx, csQueueCallbackFn cb,
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
        event = (void *)cs_get(ev, EVENT);
        if (!event)
            s = CS_INVALID_HANDLE;
    }
    if (s != CS_SUCCESS) {
        cs_task_free(task);
        if (batch)
            cs_put(&batch->base);
        return s;
    }
    if (!cb && !event) {
        atomic_bool cancelled = ATOMIC_VAR_INIT(false);
        s = batch ? cs_batch_run(batch, &cancelled, value) : cs_task_run(task, value);
        cs_task_free(task);
        if (batch)
            cs_put(&batch->base);
        return s;
    }
    pthread_mutex_lock(&cs_lock);
    bool full = active_requests >= 64;
    if (!full)
        active_requests++;
    pthread_mutex_unlock(&cs_lock);
    if (full) {
        s = CS_OUT_OF_RESOURCES;
        goto fail;
    }
    struct cs_request *r = (void *)cs_new(sizeof(*r), REQUEST, NULL);
    if (!r) {
        pthread_mutex_lock(&cs_lock);
        active_requests--;
        pthread_mutex_unlock(&cs_lock);
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
        pthread_mutex_lock(&cs_lock);
        event->pending++;
        pthread_mutex_unlock(&cs_lock);
    }
    cs_hold(&r->base); /* worker reference */
    if (out)
        *out = (void *)r->base.id;
    pthread_t thread;
    if (pthread_create(&thread, NULL, request_main, r)) {
        if (out)
            *out = NULL;
        cs_put(&r->base);
        if (event) {
            pthread_mutex_lock(&cs_lock);
            event->pending--;
            pthread_mutex_unlock(&cs_lock);
        }
        s = CS_OUT_OF_RESOURCES;
        goto launch_fail;
    }
    pthread_detach(thread);
    return CS_QUEUED;
launch_fail:
    free(r->completion);
    cs_unlink(&r->base);
    free(r);
    pthread_mutex_lock(&cs_lock);
    active_requests--;
    pthread_mutex_unlock(&cs_lock);
fail:
    if (event)
        cs_put(&event->base);
    cs_task_free(task);
    if (batch)
        cs_put(&batch->base);
    return s;
}
CS_STATUS csAbortRequest(CS_REQ_HANDLE h)
{
    struct cs_request *r = (void *)cs_get(h, REQUEST);
    if (!r)
        return CS_INVALID_HANDLE;
    /* Running legacy native functions are not safely preemptible. Do not
     * report an abort success while such a function can still write AFDM. */
    pthread_mutex_lock(&cs_lock);
    CS_STATUS s = atomic_load(&r->started) ? CS_UNSUPPORTED : CS_SUCCESS;
    if (s == CS_SUCCESS)
        atomic_store(&r->cancelled, true);
    pthread_mutex_unlock(&cs_lock);
    cs_put(&r->base);
    return s;
}
