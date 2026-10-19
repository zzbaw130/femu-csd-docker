/* Guest-side temporary extent ownership and resumable batch execution. */
#include "femu.h"
#include <endian.h>
#include <unistd.h>
struct spill_extent {
    uint64_t token, start, blocks;
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
    CsStorageRequest r = {.Mode = CS_STORAGE_BLOCK_IO, .DevHandle = d,
        .u.BlockIo = {.Type = store ? CS_STORAGE_STORE_TYPE : CS_STORAGE_LOAD_TYPE,
            .DevMem = *io, .NumRanges = 1,
            .Range = {{d->nsid, e->start, (uint32_t)e->blocks}}}};
    CsBatchRequest req = {.ReqType = CS_STORAGE_IO};
    req.u.StorageIo = r;
    struct cs_task t = {.req = &req, .dev = d};
    return cs_task_run(&t, NULL);
}
static CS_STATUS exchange(struct cs_task *t, const CsDevAFDM *io, uint64_t action,
                           uint64_t payload, void *scope, uint64_t *reentry)
{
    struct cs_device *d = t->dev;
    struct cs_memory *m = (void *)io->MemHandle;
    pthread_mutex_lock(&spill_lock);
    CS_STATUS s = attach(d);
    if (s != CS_SUCCESS)
        goto out;
    struct cs_spill_pool *p = d->spill;
    struct spill_extent **slot = &p->extents, *e;
    if (action == CS_NEED_STORE) {
        if (!payload || payload > UINT32_MAX || next_token > CS_IO_VALUE_MASK) {
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
            .scope = scope, .next = *slot};
        *slot = e; /* Reserve before I/O; even a partial failure owns its extent. */
        s = transfer(d, io, e, true);
        if (s == CS_SUCCESS)
            *reentry = e->token;
    } else {
        while (*slot && (*slot)->token != payload)
            slot = &(*slot)->next;
        e = *slot;
        if (!e) {
            s = CS_INVALID_ID;
            goto out;
        }
        if (action == CS_NEED_LOAD) {
            if (e->blocks * d->lba > m->bytes - io->ByteOffset) {
                s = CS_INVALID_LENGTH;
                goto out;
            }
            s = transfer(d, io, e, false);
            if (s == CS_SUCCESS)
                *reentry = e->token;
        } else if (action == CS_NEED_FREE) {
            s = trim(d, e);
            if (s == CS_SUCCESS) {
                *reentry = e->token;
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
CS_STATUS femu_resume(void *device, const CsBatchRequest *req, bool (*cancelled)(void *),
                      void *cancel_context, void *scope, u64 *value)
{
    struct cs_task task = {.dev = device, .req = req};
    if (req->ReqType == CS_QUEUE_COMPUTE)
        for (int i = 0; i < req->u.Compute.NumArgs; i++)
            task.scalar_slots += req->u.Compute.Args[i].Type != CS_AFDM_TYPE;
    CS_STATUS s = cs_task_run(&task, value);
    if (s != CS_SUCCESS || req->ReqType != CS_QUEUE_COMPUTE) return s;
    for (;;) {
        uint64_t action = *value & ~CS_IO_VALUE_MASK;
        if (action != CS_NEED_STORE && action != CS_NEED_LOAD && action != CS_NEED_FREE) break;
        if (cancelled(cancel_context)) return CS_ERROR_IN_EXECUTION;
        if (task.scalar_slots > 1) return CS_INVALID_ARG;
        const CsComputeRequest *r = &req->u.Compute;
        const CsDevAFDM *io = NULL;
        for (int i = 0; i < r->NumArgs; i++)
            if (r->Args[i].Type == CS_AFDM_TYPE) io = &r->Args[i].u.DevMem;
        if (!io) return CS_INVALID_ARG;
        s = exchange(&task, io, action, *value & CS_IO_VALUE_MASK, scope, &task.reentry);
        if (s != CS_SUCCESS) return s;
        s = cs_task_run(&task, value);
        if (s != CS_SUCCESS) return s;
    }
    return s;
}
