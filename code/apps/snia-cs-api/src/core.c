#include "internal.h"
#include <assert.h>
#include <limits.h>
pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cs_object *objects;
static uintptr_t next_id = 16;
struct cs_object *cs_new(size_t bytes, unsigned kind, void (*destroy)(struct cs_object *))
{
    struct cs_object *o = calloc(1, bytes);
    if (!o)
        return NULL;
    pthread_mutex_lock(&cs_lock);
    if (next_id > UINTPTR_MAX - 16) {
        pthread_mutex_unlock(&cs_lock);
        free(o);
        return NULL;
    }
    o->id = next_id++;
    o->kind = kind;
    o->refs = 1;
    o->destroy = destroy;
    o->next = objects;
    objects = o;
    pthread_mutex_unlock(&cs_lock);
    return o;
}
struct cs_object *cs_get(void *h, unsigned kind)
{
    struct cs_object *o;
    pthread_mutex_lock(&cs_lock);
    for (o = objects; o; o = o->next)
        if (o->id == (uintptr_t)h && o->kind == kind) {
            o->refs++;
            break;
        }
    pthread_mutex_unlock(&cs_lock);
    return o;
}
void cs_hold(struct cs_object *o)
{
    pthread_mutex_lock(&cs_lock);
    o->refs++;
    pthread_mutex_unlock(&cs_lock);
}
void cs_put(struct cs_object *o)
{
    bool dead;
    if (!o)
        return;
    pthread_mutex_lock(&cs_lock);
    assert(o->refs);
    dead = !--o->refs;
    pthread_mutex_unlock(&cs_lock);
    if (dead) {
        if (o->destroy)
            o->destroy(o);
        free(o);
    }
}
CS_STATUS cs_retire(struct cs_object *o)
{
    struct cs_object **p;
    pthread_mutex_lock(&cs_lock);
    if (o->refs != 2) {
        pthread_mutex_unlock(&cs_lock);
        return CS_HANDLE_IN_USE;
    }
    for (p = &objects; *p && *p != o; p = &(*p)->next) {
    }
    if (!*p) {
        pthread_mutex_unlock(&cs_lock);
        return CS_INVALID_HANDLE;
    }
    *p = o->next;
    o->refs--;
    pthread_mutex_unlock(&cs_lock);
    return CS_SUCCESS;
}
CS_STATUS cs_output(int *length, void *buffer, size_t needed)
{
    if (!length || needed > INT_MAX)
        return CS_INVALID_ARG;
    int have = *length;
    *length = (int)needed;
    return !buffer || have < 0 || (size_t)have < needed ? CS_INVALID_LENGTH : CS_SUCCESS;
}
static void device_destroy(struct cs_object *o)
{
    cs_spill_detach((void *)o);
    cs_transport_close((void *)o);
}
CS_STATUS csOpenCSx(const char *name, void *ctx, CS_DEV_HANDLE *handle)
{
    if (!name || !handle)
        return CS_INVALID_ARG;
    *handle = NULL;
    struct cs_device *d = (void *)cs_new(sizeof(*d), DEV, device_destroy);
    if (!d)
        return CS_OUT_OF_RESOURCES;
    d->fd = d->admin = -1;
    CS_STATUS s;
    if (cs_transport_open(d, name) < 0 || cs_profile_load(d, name) < 0)
        s = CS_DEVICE_NOT_AVAILABLE;
    else
        s = CS_SUCCESS;
    if (s != CS_SUCCESS) {
        cs_hold(&d->base);
        cs_retire(&d->base);
        cs_put(&d->base);
        return s;
    }
    d->context = ctx;
    *handle = (void *)d->base.id;
    return CS_SUCCESS;
}
CS_STATUS csCloseCSx(CS_DEV_HANDLE h)
{
    struct cs_device *d = (void *)cs_get(h, DEV);
    if (!d)
        return CS_INVALID_HANDLE;
    struct cs_object *garbage = NULL;
    unsigned functions = 0;
    CS_STATUS status = CS_SUCCESS;
    pthread_mutex_lock(&cs_lock);
    for (struct cs_object *o = objects; o; o = o->next) {
        if (o->kind == FUNCTION && ((struct cs_function *)o)->dev == d) {
            functions++;
            if (o->refs != 1)
                status = CS_HANDLE_IN_USE;
        }
    }
    if (d->base.refs != functions + 2)
        status = CS_HANDLE_IN_USE;
    if (status == CS_SUCCESS) {
        struct cs_object **p = &objects;
        while (*p) {
            struct cs_object *o = *p;
            if (o == &d->base) {
                *p = o->next;
                o->refs--;
            } else if (o->kind == FUNCTION && ((struct cs_function *)o)->dev == d) {
                *p = o->next;
                o->refs--;
                o->next = garbage;
                garbage = o;
            } else
                p = &o->next;
        }
    }
    pthread_mutex_unlock(&cs_lock);
    while (garbage) {
        struct cs_object *o = garbage;
        garbage = o->next;
        if (o->destroy)
            o->destroy(o);
        free(o);
    }
    cs_put(&d->base);
    return status;
}
void cs_unlink(struct cs_object *o)
{
    pthread_mutex_lock(&cs_lock);
    struct cs_object **p = &objects;
    while (*p && *p != o)
        p = &(*p)->next;
    if (*p) {
        *p = o->next;
        o->refs--;
    }
    pthread_mutex_unlock(&cs_lock);
}

void cs_restore(struct cs_object *o)
{
    pthread_mutex_lock(&cs_lock);
    o->refs++;
    o->next = objects;
    objects = o;
    pthread_mutex_unlock(&cs_lock);
}

bool cs_function_busy(struct cs_device *d, unsigned index)
{
    bool busy = false;
    pthread_mutex_lock(&cs_lock);
    for (struct cs_object *o = objects; o; o = o->next)
        if (o->kind == FUNCTION && !strcmp(((struct cs_function *)o)->dev->path, d->path) &&
            ((struct cs_function *)o)->index == index && o->refs > 1) {
            busy = true;
            break;
        }
    pthread_mutex_unlock(&cs_lock);
    return busy;
}
