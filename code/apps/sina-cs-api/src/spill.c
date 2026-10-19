/* Guest-side temporary extent ownership and resumable batch execution. */
#include "internal.h"
#include <endian.h>
#include <unistd.h>
struct spill_extent {
    uint64_t token, start, blocks;
    uint32_t bytes;
    void *scope;
    struct spill_extent *next;
};
struct cs_spill_pool {
    char path[4096];
    int fd;
    unsigned refs;
    uint64_t start, blocks;
    struct spill_extent *extents;
    struct cs_spill_pool *next;
};
static pthread_mutex_t spill_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cs_spill_pool *pools;
static uint64_t next_token = 1;
/* The pool lock also serializes extent I/O and prevents reuse during a LOAD.
 * It is never held while running a CSF. Independent CSFs can run concurrently. */
static CS_STATUS attach(struct cs_device *d)
{
    if (d->spill)
        return CS_SUCCESS;
    for (struct cs_spill_pool *p = pools; p; p = p->next)
        if (!strcmp(p->path, d->path)) {
            p->refs++;
            d->spill = p;
            return CS_SUCCESS;
        }
    struct cs_spill_pool *p = calloc(1, sizeof(*p));
    if (!p)
        return CS_OUT_OF_RESOURCES;
    p->fd = cs_spill_region(d, &p->start, &p->blocks);
    if (p->fd < 0) {
        free(p);
        return CS_DEVICE_NOT_AVAILABLE;
    }
    strcpy(p->path, d->path);
    p->refs = 1;
    p->next = pools;
    pools = d->spill = p;
    return CS_SUCCESS;
}
static CS_STATUS trim(struct cs_device *d, struct spill_extent *e)
{
    uint64_t start = e->start, remaining = e->blocks;
    while (remaining) {
        uint32_t count = remaining > UINT32_MAX ? UINT32_MAX : remaining;
        struct { uint32_t attributes, blocks; uint64_t start; } descriptor = {
            0, htole32(count), htole64(start)};
        struct nvme_passthru_cmd cmd = {.opcode = 9, .nsid = d->nsid,
            .addr = (uintptr_t)&descriptor, .data_len = sizeof(descriptor), .cdw11 = 4};
        CS_STATUS s = cs_io(d, false, &cmd);
        if (s != CS_SUCCESS)
            return s;
        start += count;
        remaining -= count;
    }
    return CS_SUCCESS;
}
static CS_STATUS transfer(struct cs_device *d, const CsDevAFDM *io,
                           struct spill_extent *e, bool store)
{
    CsStorageRequest r = {.Mode = CS_STORAGE_BLOCK_IO, .DevHandle = (void *)d->base.id,
        .u.BlockIo = {.Type = store ? CS_STORAGE_STORE_TYPE : CS_STORAGE_LOAD_TYPE,
            .DevMem = *io, .NumRanges = 1,
            .Range = {{d->nsid, e->start, (uint32_t)e->blocks}}}};
    return csQueueStorageRequest(&r, NULL, NULL, NULL, NULL, NULL);
}
static CS_STATUS exchange(struct cs_task *t, const CsDevAFDM *io, unsigned action,
                           uint64_t payload, void *scope, unsigned __int128 *control)
{
    struct cs_device *d = t->dev;
    struct cs_memory *m = (void *)cs_task_object(t, io->MemHandle, MEM);
    pthread_mutex_lock(&spill_lock);
    CS_STATUS s = attach(d);
    if (s != CS_SUCCESS)
        goto out;
    struct cs_spill_pool *p = d->spill;
    struct spill_extent **slot = &p->extents, *e;
    if (action == CS_CSF_NEED_STORE) {
        if (!payload || payload > UINT32_MAX || next_token > CS_CSF_PAYLOAD_MASK) {
            s = CS_INVALID_LENGTH;
            goto out;
        }
        uint64_t blocks = (payload + d->lba - 1) / d->lba;
        if (blocks > UINT32_MAX || blocks * d->lba > m->bytes - io->ByteOffset) {
            s = CS_INVALID_LENGTH;
            goto out;
        }
        uint64_t first = p->start;
        while (*slot) {
            if (blocks <= (*slot)->start - first)
                break;
            first = (*slot)->start + (*slot)->blocks;
            slot = &(*slot)->next;
        }
        if (first - p->start > p->blocks || blocks > p->blocks - (first - p->start)) {
            s = CS_NOT_ENOUGH_MEMORY;
            goto out;
        }
        e = calloc(1, sizeof(*e));
        if (!e) {
            s = CS_OUT_OF_RESOURCES;
            goto out;
        }
        *e = (struct spill_extent){.token = next_token++, .start = first, .blocks = blocks,
            .bytes = payload, .scope = scope, .next = *slot};
        *slot = e; /* Reserve before I/O; even a partial failure owns its extent. */
        s = transfer(d, io, e, true);
        if (s == CS_SUCCESS)
            *control = csCsfControl(CS_CSF_STORE_DONE, e->token, e->bytes);
    } else {
        while (*slot && (*slot)->token != payload)
            slot = &(*slot)->next;
        e = *slot;
        if (!e) {
            s = CS_INVALID_ID;
            goto out;
        }
        if (action == CS_CSF_NEED_LOAD) {
            if (e->blocks * d->lba > m->bytes - io->ByteOffset) {
                s = CS_INVALID_LENGTH;
                goto out;
            }
            s = transfer(d, io, e, false);
            if (s == CS_SUCCESS)
                *control = csCsfControl(CS_CSF_LOAD_DONE, e->token, e->bytes);
        } else if (action == CS_CSF_NEED_FREE) {
            s = trim(d, e);
            if (s == CS_SUCCESS) {
                *control = csCsfControl(CS_CSF_FREE_DONE, e->token, e->bytes);
                *slot = e->next;
                free(e);
            }
        } else
            s = CS_INVALID_OPTION;
    }
out:
    pthread_mutex_unlock(&spill_lock);
    return s;
}
void cs_spill_scope_end(struct cs_device *d, void *scope, bool success)
{
    pthread_mutex_lock(&spill_lock);
    struct cs_spill_pool *p = d->spill;
    if (p) {
        struct spill_extent **slot = &p->extents;
        while (*slot) {
            struct spill_extent *e = *slot;
            if (e->scope != scope) {
                slot = &e->next;
                continue;
            }
            e->scope = NULL;
            if (!success && trim(d, e) == CS_SUCCESS) {
                *slot = e->next;
                free(e);
            } else
                slot = &e->next;
        }
    }
    pthread_mutex_unlock(&spill_lock);
}
void cs_spill_detach(struct cs_device *d)
{
    pthread_mutex_lock(&spill_lock);
    struct cs_spill_pool *p = d->spill;
    if (p && !--p->refs) {
        while (p->extents) {
            struct spill_extent *e = p->extents;
            trim(d, e);
            p->extents = e->next;
            free(e);
        }
        struct cs_spill_pool **slot = &pools;
        while (*slot != p)
            slot = &(*slot)->next;
        *slot = p->next;
        close(p->fd);
        free(p);
    }
    d->spill = NULL;
    pthread_mutex_unlock(&spill_lock);
}
CS_STATUS cs_task_resume(struct cs_task *original, atomic_bool *cancelled, void *scope, u64 *value)
{
    if (original->req->ReqType != CS_QUEUE_COMPUTE)
        return cs_task_run(original, value);
    const CsComputeRequest *r = &original->req->u.Compute;
    if (r->NumArgs < 2 || r->Args[r->NumArgs - 1].Type != CS_128BIT_VALUE_TYPE)
        return cs_task_run(original, value);
    unsigned __int128 control = r->Args[r->NumArgs - 1].u.Value128;
    if (!csCsfIsControl(control, control >> 64))
        return cs_task_run(original, value);
    if (control != csCsfControl(CS_CSF_START, 0, 0) ||
        r->Args[r->NumArgs - 2].Type != CS_AFDM_TYPE)
        return CS_INVALID_ARG;
    struct cs_task *t;
    CS_STATUS s = cs_task_clone(original->req, &t);
    if (s != CS_SUCCESS)
        return s;
    CsComputeRequest *private = &t->req->u.Compute;
    const CsDevAFDM *io = &private->Args[private->NumArgs - 2].u.DevMem;
    bool invoked = false;
    for (;;) {
        if (atomic_load(cancelled)) {
            s = CS_ERROR_IN_EXECUTION;
            break;
        }
        private->Args[private->NumArgs - 1].u.Value128 = control;
        invoked = true;
        s = cs_task_run(t, value);
        if (s != CS_SUCCESS)
            break;
        if (!csCsfIsCompletion(*value)) {
            s = CS_INVALID_OPTION;
            break;
        }
        unsigned action = csCsfAction(*value);
        if (action == CS_CSF_DONE)
            break;
        if (atomic_load(cancelled)) {
            s = CS_ERROR_IN_EXECUTION;
            break;
        }
        s = exchange(t, io, action, csCsfPayload(*value), scope, &control);
        if (s != CS_SUCCESS)
            break;
    }
    if (s != CS_SUCCESS && invoked) {
        private->Args[private->NumArgs - 1].u.Value128 = csCsfControl(CS_CSF_ABORT, 0, 0);
        u64 ignored;
        cs_task_run(t, &ignored); /* CSF releases private state; never schedule its new I/O. */
    }
    cs_task_free(t);
    return s;
}
