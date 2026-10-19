/* 跨 API 共享：公开句柄查找、类型检查、引用计数和资源退役。 */
#include "cs_internal.h"
#include <assert.h>
pthread_mutex_t cs_handle_lock = PTHREAD_MUTEX_INITIALIZER;
struct cs_object *cs_objects;
static uintptr_t next_id = 16;
struct cs_object *cs_handle_create(size_t bytes, unsigned kind, void (*destroy)(struct cs_object *))
{
    struct cs_object *o = calloc(1, bytes);
    if (!o)
        return NULL;
    pthread_mutex_lock(&cs_handle_lock);
    if (next_id > UINTPTR_MAX - 16) {
        pthread_mutex_unlock(&cs_handle_lock);
        free(o);
        return NULL;
    }
    o->id = next_id++;
    o->kind = kind;
    o->refs = 1;
    o->destroy = destroy;
    o->next = cs_objects;
    cs_objects = o;
    pthread_mutex_unlock(&cs_handle_lock);
    return o;
}
struct cs_object *cs_handle_acquire(void *h, unsigned kind)
{
    struct cs_object *o;
    pthread_mutex_lock(&cs_handle_lock);
    for (o = cs_objects; o; o = o->next)
        if (o->id == (uintptr_t)h && o->kind == kind) {
            o->refs++;
            break;
        }
    pthread_mutex_unlock(&cs_handle_lock);
    return o;
}
void cs_handle_retain(struct cs_object *o)
{
    pthread_mutex_lock(&cs_handle_lock);
    o->refs++;
    pthread_mutex_unlock(&cs_handle_lock);
}
void cs_handle_release(struct cs_object *o)
{
    bool dead;
    if (!o)
        return;
    pthread_mutex_lock(&cs_handle_lock);
    assert(o->refs);
    dead = !--o->refs;
    pthread_mutex_unlock(&cs_handle_lock);
    if (dead) {
        if (o->destroy)
            o->destroy(o);
        free(o);
    }
}
CS_STATUS cs_handle_retire(struct cs_object *o)
{
    struct cs_object **p;
    pthread_mutex_lock(&cs_handle_lock);
    if (o->refs != 2) {
        pthread_mutex_unlock(&cs_handle_lock);
        return CS_HANDLE_IN_USE;
    }
    for (p = &cs_objects; *p && *p != o; p = &(*p)->next) {
    }
    if (!*p) {
        pthread_mutex_unlock(&cs_handle_lock);
        return CS_INVALID_HANDLE;
    }
    *p = o->next;
    o->refs--;
    pthread_mutex_unlock(&cs_handle_lock);
    return CS_SUCCESS;
}
void cs_handle_remove(struct cs_object *o)
{
    pthread_mutex_lock(&cs_handle_lock);
    struct cs_object **p = &cs_objects;
    while (*p && *p != o)
        p = &(*p)->next;
    if (*p) {
        *p = o->next;
        o->refs--;
    }
    pthread_mutex_unlock(&cs_handle_lock);
}

void cs_handle_restore(struct cs_object *o)
{
    pthread_mutex_lock(&cs_handle_lock);
    o->refs++;
    o->next = cs_objects;
    cs_objects = o;
    pthread_mutex_unlock(&cs_handle_lock);
}

bool cs_csf_in_use(struct cs_device *d, unsigned index)
{
    bool busy = false;
    pthread_mutex_lock(&cs_handle_lock);
    for (struct cs_object *o = cs_objects; o; o = o->next)
        if (o->kind == FUNCTION && ((struct cs_function *)o)->dev->plugin == d->plugin &&
            !strcmp(((struct cs_function *)o)->dev->path, d->path) &&
            ((struct cs_function *)o)->id == index && o->refs > 1) {
            busy = true;
            break;
        }
    pthread_mutex_unlock(&cs_handle_lock);
    return busy;
}
