#include "internal.h"
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
