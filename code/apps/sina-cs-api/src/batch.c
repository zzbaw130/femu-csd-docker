#include "internal.h"
#include <endian.h>
static void batch_destroy(struct cs_object *o)
{
    struct cs_batch *b = (void *)o;
    for (unsigned i = 0; i < b->capacity; i++)
        cs_task_free(b->entries[i].task);
}
CS_STATUS csAllocBatchRequest(CS_BATCH_MODE mode, int max, CS_BATCH_HANDLE *out)
{
    if (!out || max <= 0 || max > CS_MAX_BATCH)
        return CS_INVALID_ARG;
    *out = NULL;
    if (mode < CS_BATCH_SERIAL || mode > CS_BATCH_HYBRID)
        return CS_INVALID_OPTION;
    struct cs_batch *b = (void *)cs_new(sizeof(*b), BATCH, batch_destroy);
    if (!b)
        return CS_OUT_OF_RESOURCES;
    b->mode = mode;
    b->capacity = max;
    *out = (void *)b->base.id;
    return CS_SUCCESS;
}
CS_STATUS csFreeBatchRequest(CS_BATCH_HANDLE h)
{
    struct cs_batch *b = (void *)cs_get(h, BATCH);
    if (!b)
        return CS_INVALID_HANDLE;
    CS_STATUS s = cs_retire(&b->base);
    cs_put(&b->base);
    return s;
}
static bool acyclic(const struct cs_entry *entries, unsigned max)
{
    uint64_t all = 0, done = 0;
    for (unsigned i = 0; i < max; i++)
        if (entries[i].task)
            all |= UINT64_C(1) << i;
    for (unsigned pass = 0; pass < max; pass++) {
        uint64_t before = done;
        for (unsigned i = 0; i < max; i++)
            if ((all & (UINT64_C(1) << i)) && !(entries[i].dependencies & ~done))
                done |= UINT64_C(1) << i;
        if (done == all)
            return true;
        if (done == before)
            return false;
    }
    return false;
}
CS_STATUS csConfigureBatchEntry(CS_BATCH_HANDLE h, CS_BATCH_CONFIG_TYPE action,
                                const CsBatchRequest *req, CS_BATCH_INDEX before,
                                CS_BATCH_INDEX after, CS_BATCH_INDEX *curr)
{
    struct cs_batch *b = (void *)cs_get(h, BATCH);
    if (!b)
        return CS_INVALID_HANDLE;
    struct cs_task *newtask = NULL, *oldtask = NULL;
    CS_STATUS s = CS_SUCCESS;
    if (action == CS_BATCH_ADD || action == CS_BATCH_RECONFIG) {
        if (!req) {
            s = CS_INVALID_ARG;
            goto out;
        }
        s = cs_task_clone(req, &newtask);
        if (s != CS_SUCCESS)
            goto out;
    }
    pthread_mutex_lock(&cs_lock);
    if (b->base.refs != 2) {
        s = CS_HANDLE_IN_USE;
        goto unlock;
    }
    if (before > b->capacity || after > b->capacity || (before && !b->entries[before - 1].task) ||
        (after && !b->entries[after - 1].task)) {
        s = CS_INVALID_ID;
        goto unlock;
    }
    if (b->mode == CS_BATCH_PARALLEL && ((action == CS_BATCH_ADD && (before || after)) ||
                                         action == CS_BATCH_JOIN || action == CS_BATCH_SPLIT)) {
        s = CS_INVALID_OPTION;
        goto unlock;
    }
    struct cs_entry copy[CS_MAX_BATCH];
    memcpy(copy, b->entries, sizeof(copy));
    uint64_t bit_before = before ? UINT64_C(1) << (before - 1) : 0,
             bit_after = after ? UINT64_C(1) << (after - 1) : 0;
    unsigned index = 0;
    if (action == CS_BATCH_ADD) {
        if (!curr) {
            s = CS_INVALID_ARG;
            goto unlock;
        }
        while (index < b->capacity && copy[index].task)
            index++;
        if (index == b->capacity) {
            s = CS_OUT_OF_RESOURCES;
            goto unlock;
        }
        /* One batch refers to one CSx in this backend. */
        for (unsigned i = 0; i < b->capacity; i++)
            if (copy[i].task && copy[i].task->dev != newtask->dev) {
                s = CS_UNSUPPORTED;
                goto unlock;
            }
        copy[index] = (struct cs_entry){newtask, bit_after};
        if (before) {
            copy[before - 1].dependencies &= ~bit_after;
            copy[before - 1].dependencies |= UINT64_C(1) << index;
        }
    } else if (action == CS_BATCH_JOIN) {
        if (req || curr || !before || !after) {
            s = CS_INVALID_ARG;
            goto unlock;
        }
        copy[before - 1].dependencies |= bit_after;
    } else if (action == CS_BATCH_SPLIT) {
        if (req || curr || (!before && !after)) {
            s = CS_INVALID_ARG;
            goto unlock;
        }
        if (before && after)
            copy[after - 1].dependencies &= ~bit_before;
        else if (before) {
            for (unsigned i = 0; i < b->capacity; i++)
                copy[i].dependencies &= ~bit_before;
        } else
            copy[after - 1].dependencies = 0;
    } else if (action == CS_BATCH_DELETE) {
        if (req || curr || !before || after) {
            s = CS_INVALID_ARG;
            goto unlock;
        }
        oldtask = copy[before - 1].task;
        copy[before - 1] = (struct cs_entry){0};
        for (unsigned i = 0; i < b->capacity; i++)
            copy[i].dependencies &= ~bit_before;
    } else if (action == CS_BATCH_RECONFIG) {
        if (curr || !before || after) {
            s = CS_INVALID_ARG;
            goto unlock;
        }
        if (newtask->dev != copy[before - 1].task->dev) {
            s = CS_UNSUPPORTED;
            goto unlock;
        }
        oldtask = copy[before - 1].task;
        copy[before - 1].task = newtask;
    } else {
        s = CS_INVALID_OPTION;
        goto unlock;
    }
    if (!acyclic(copy, b->capacity)) {
        oldtask = NULL;
        s = CS_INVALID_ARG;
        goto unlock;
    }
    memcpy(b->entries, copy, sizeof(copy));
    if (action == CS_BATCH_ADD)
        *curr = index + 1;
    if (action == CS_BATCH_ADD || action == CS_BATCH_RECONFIG)
        newtask = NULL;
unlock:
    pthread_mutex_unlock(&cs_lock);
out:
    cs_task_free(newtask);
    if (s == CS_SUCCESS)
        cs_task_free(oldtask);
    cs_put(&b->base);
    return s;
}
CS_STATUS csHelperResizeBatchRequest(CS_BATCH_HANDLE h, int max)
{
    if (max <= 0 || max > CS_MAX_BATCH)
        return CS_INVALID_ARG;
    struct cs_batch *b = (void *)cs_get(h, BATCH);
    if (!b)
        return CS_INVALID_HANDLE;
    pthread_mutex_lock(&cs_lock);
    CS_STATUS s = b->base.refs != 2 ? CS_HANDLE_IN_USE : CS_SUCCESS;
    if (s == CS_SUCCESS) {
        for (unsigned i = max; i < b->capacity; i++)
            if (b->entries[i].task)
                s = CS_INVALID_LENGTH;
    }
    if (s == CS_SUCCESS)
        b->capacity = max;
    pthread_mutex_unlock(&cs_lock);
    cs_put(&b->base);
    return s;
}
struct execution {
    struct cs_batch *b;
    atomic_bool *cancelled;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint64_t all, started, done;
    unsigned running;
    CS_STATUS error;
    u64 value;
};
static void *batch_worker(void *arg)
{
    struct execution *x = arg;
    for (;;) {
        pthread_mutex_lock(&x->lock);
        unsigned index;
        for (;;) {
            if (x->error != CS_SUCCESS || atomic_load(x->cancelled) || x->done == x->all) {
                pthread_mutex_unlock(&x->lock);
                return NULL;
            }
            for (index = 0; index < x->b->capacity; index++)
                if ((x->all & (UINT64_C(1) << index)) && !(x->started & (UINT64_C(1) << index)) &&
                    !(x->b->entries[index].dependencies & ~x->done))
                    break;
            if (index < x->b->capacity)
                break;
            if (!x->running) {
                x->error = CS_INVALID_ARG;
                pthread_cond_broadcast(&x->cond);
                pthread_mutex_unlock(&x->lock);
                return NULL;
            }
            pthread_cond_wait(&x->cond, &x->lock);
        }
        x->started |= UINT64_C(1) << index;
        x->running++;
        pthread_mutex_unlock(&x->lock);
        u64 value = 0;
        CS_STATUS s = cs_task_resume(x->b->entries[index].task, x->cancelled, x, &value);
        pthread_mutex_lock(&x->lock);
        x->running--;
        x->done |= UINT64_C(1) << index;
        x->value = value;
        if (s != CS_SUCCESS && x->error == CS_SUCCESS)
            x->error = s;
        pthread_cond_broadcast(&x->cond);
        pthread_mutex_unlock(&x->lock);
    }
}
CS_STATUS cs_batch_run(struct cs_batch *b, atomic_bool *cancelled, u64 *value)
{
    struct execution x = {.b = b,
                          .cancelled = cancelled,
                          .lock = PTHREAD_MUTEX_INITIALIZER,
                          .cond = PTHREAD_COND_INITIALIZER,
                          .error = CS_SUCCESS};
    unsigned workers = 1;
    struct cs_device *device = NULL;
    for (unsigned i = 0; i < b->capacity; i++)
        if (b->entries[i].task) {
            x.all |= UINT64_C(1) << i;
            device = b->entries[i].task->dev;
            workers = device->caps.workers;
        }
    if (!x.all) {
        pthread_mutex_destroy(&x.lock);
        pthread_cond_destroy(&x.cond);
        return CS_INVALID_ARG;
    }
    if (b->mode == CS_BATCH_SERIAL)
        workers = 1;
    if (!workers)
        workers = 1;
    if (workers > CS_MAX_BATCH)
        workers = CS_MAX_BATCH;
    pthread_t threads[CS_MAX_BATCH];
    unsigned launched = 0;
    for (unsigned i = 1; i < workers; i++) {
        if (pthread_create(&threads[launched], NULL, batch_worker, &x))
            break;
        launched++;
    }
    batch_worker(&x);
    for (unsigned i = 0; i < launched; i++)
        pthread_join(threads[i], NULL);
    cs_spill_scope_end(device, &x, x.error == CS_SUCCESS && !atomic_load(cancelled));
    if (value)
        *value = x.value;
    pthread_mutex_destroy(&x.lock);
    pthread_cond_destroy(&x.cond);
    return atomic_load(cancelled) ? CS_ERROR_IN_EXECUTION : x.error;
}
CS_STATUS csQueueBatchRequest(CS_BATCH_HANDLE h, void *ctx, csQueueCallbackFn cb, CS_EVT_HANDLE ev,
                              CS_REQ_HANDLE *req, u64 *value)
{
    struct cs_batch *b = (void *)cs_get(h, BATCH);
    if (!b)
        return CS_INVALID_HANDLE;
    return cs_submit(NULL, b, ctx, cb, ev, req, value);
}
