#include "internal.h"
#include <endian.h>
#include <stddef.h>
static CS_STATUS pin(struct cs_task *t, void *h, unsigned kind)
{
    struct cs_object *o = cs_get(h, kind);
    if (!o)
        return kind == MEM        ? CS_UNKNOWN_MEMORY
               : kind == FUNCTION ? CS_INVALID_CSF_ID
                                  : CS_INVALID_HANDLE;
    struct cs_device *d = kind == DEV   ? (void *)o
                          : kind == MEM ? ((struct cs_memory *)o)->dev
                                        : ((struct cs_function *)o)->dev;
    if (t->npins >= CS_API_MAX_ARGS + 4 || (t->dev && d != t->dev)) {
        cs_put(o);
        return CS_INVALID_ARG;
    }
    t->dev = d;
    t->pins[t->npins++] = o;
    return CS_SUCCESS;
}
struct cs_object *cs_task_object(struct cs_task *t, void *h, unsigned kind)
{
    for (unsigned i = 0; i < t->npins; i++)
        if (t->pins[i]->id == (uintptr_t)h && t->pins[i]->kind == kind)
            return t->pins[i];
    return NULL;
}
void cs_task_free(struct cs_task *t)
{
    if (t) {
        for (unsigned i = 0; i < t->npins; i++)
            cs_put(t->pins[i]);
        free(t->req);
        free(t);
    }
}
CS_STATUS cs_task_clone(const CsBatchRequest *r, struct cs_task **out)
{
    if (!r || !out)
        return CS_INVALID_ARG;
    *out = NULL;
    size_t bytes = sizeof(*r);
    if (r->ReqType == CS_QUEUE_COMPUTE) {
        int n = r->u.Compute.NumArgs;
        if (n < 0 || n > CS_API_MAX_ARGS)
            return CS_INVALID_ARG;
        bytes = offsetof(CsBatchRequest, u.Compute.Args) + (size_t)n * sizeof(CsComputeArg);
    } else if (r->ReqType == CS_STORAGE_IO) {
        if (r->u.StorageIo.Mode != CS_STORAGE_BLOCK_IO)
            return CS_UNSUPPORTED;
        int n = r->u.StorageIo.u.BlockIo.NumRanges;
        if (n <= 0 || n > 4096)
            return CS_INVALID_ARG;
        bytes = offsetof(CsBatchRequest, u.StorageIo.u.BlockIo.Range) +
                (size_t)n * sizeof(CSBlockRange);
    } else if (r->ReqType != CS_COPY_AFDM)
        return CS_INVALID_OPTION;
    if (bytes < sizeof(*r))
        bytes = sizeof(*r);
    struct cs_task *t = calloc(1, sizeof(*t));
    if (!t)
        return CS_OUT_OF_RESOURCES;
    t->req = malloc(bytes);
    if (!t->req) {
        free(t);
        return CS_OUT_OF_RESOURCES;
    }
    memcpy(t->req, r, bytes);
    CS_STATUS s = CS_SUCCESS;
    if (r->ReqType == CS_QUEUE_COMPUTE) {
        pthread_mutex_lock(&cs_catalog_lock);
        s = pin(t, r->u.Compute.CSFId, FUNCTION);
        if (s == CS_SUCCESS && ((struct cs_function *)t->pins[0])->type != 3)
            s = CS_UNSUPPORTED_TYPE;
        if (s == CS_SUCCESS) {
            struct cs_function *f = (void *)t->pins[0];
            struct cs_catalog_entry *e = cs_catalog_find_locked(t->dev, f->index);
            if (!e || !e->info.active)
                s = CS_INVALID_CSF_ID;
        }
        for (int i = 0; s == CS_SUCCESS && i < r->u.Compute.NumArgs; i++) {
            const CsComputeArg *a = &r->u.Compute.Args[i];
            if (a->Type == CS_AFDM_TYPE) {
                s = pin(t, a->u.DevMem.MemHandle, MEM);
                if (s == CS_SUCCESS) {
                    struct cs_memory *m = (void *)t->pins[t->npins - 1];
                    if (a->u.DevMem.ByteOffset > m->bytes)
                        s = CS_INVALID_LENGTH;
                    else if (a->u.DevMem.ByteOffset)
                        s = CS_UNSUPPORTED;
                }
            } else if (a->Type < CS_8BIT_VALUE_TYPE || a->Type > CS_128BIT_VALUE_TYPE)
                s = CS_UNSUPPORTED_TYPE;
            else {
                t->scalar_slots += a->Type == CS_128BIT_VALUE_TYPE ? 2 : 1;
                if (t->scalar_slots > 2)
                    s = CS_UNSUPPORTED;
            }
        }
        pthread_mutex_unlock(&cs_catalog_lock);
    } else if (r->ReqType == CS_COPY_AFDM) {
        const CsCopyMemRequest *c = &r->u.CopyMem;
        if (c->Bytes <= 0)
            s = CS_INVALID_ARG;
        else
            s = pin(t, c->DevMem.MemHandle, MEM);
        if (s == CS_SUCCESS && c->Type == CS_COPY_WITHIN_DEVICE)
            s = pin(t, c->u.SrcDevMem.MemHandle, MEM);
        else if (s == CS_SUCCESS &&
                 (c->Type != CS_COPY_TO_DEVICE && c->Type != CS_COPY_FROM_DEVICE))
            s = CS_INVALID_OPTION;
        else if (s == CS_SUCCESS && !c->u.HostVAddress)
            s = CS_INVALID_ARG;
        if (s == CS_SUCCESS) {
            struct cs_memory *m = (void *)cs_task_object(t, c->DevMem.MemHandle, MEM);
            if (c->DevMem.ByteOffset > m->bytes ||
                (uint64_t)c->Bytes > m->bytes - c->DevMem.ByteOffset)
                s = CS_INVALID_LENGTH;
        }
        if (s == CS_SUCCESS && c->Type == CS_COPY_WITHIN_DEVICE) {
            struct cs_memory *m = (void *)cs_task_object(t, c->u.SrcDevMem.MemHandle, MEM);
            if (c->u.SrcDevMem.ByteOffset > m->bytes ||
                (uint64_t)c->Bytes > m->bytes - c->u.SrcDevMem.ByteOffset)
                s = CS_INVALID_LENGTH;
        }
    } else {
        const CsStorageRequest *sr = &r->u.StorageIo;
        const CsBlockIo *b = &sr->u.BlockIo;
        if (b->Type != CS_STORAGE_LOAD_TYPE && b->Type != CS_STORAGE_STORE_TYPE)
            s = CS_INVALID_OPTION;
        else if (b->StorageIndex)
            s = CS_UNSUPPORTED_INDEX;
        else
            s = pin(t, sr->DevHandle, DEV);
        if (s == CS_SUCCESS)
            s = pin(t, b->DevMem.MemHandle, MEM);
        uint64_t total = 0;
        for (int i = 0; s == CS_SUCCESS && i < b->NumRanges; i++) {
            if (b->Range[i].NamespaceId != (u32)t->dev->nsid || !b->Range[i].NumBlocks)
                s = CS_INVALID_ARG;
            else if (b->Range[i].StartLba > UINT64_MAX - b->Range[i].NumBlocks)
                s = CS_INVALID_LENGTH;
            else
                total += (uint64_t)b->Range[i].NumBlocks * t->dev->lba;
        }
        if (s == CS_SUCCESS) {
            struct cs_memory *m = (void *)cs_task_object(t, b->DevMem.MemHandle, MEM);
            if (b->DevMem.ByteOffset > m->bytes || total > m->bytes - b->DevMem.ByteOffset)
                s = CS_INVALID_LENGTH;
        }
    }
    if (s != CS_SUCCESS) {
        cs_task_free(t);
        return s;
    }
    *out = t;
    return CS_SUCCESS;
}
static CS_STATUS compute(struct cs_task *t, u64 *value)
{
    const CsComputeRequest *r = &t->req->u.Compute;
    struct cs_function *f = (void *)cs_task_object(t, r->CSFId, FUNCTION);
    /* Native MR descriptors only. No argument AFDM or type description. */
    struct { uint32_t nsid, len; uint64_t id, reserved[2]; } ranges[CS_API_MAX_ARGS] = {0};
    uint64_t params[2] = {0};
    unsigned nr = 0, scalar = 0;
    for (int i = 0; i < r->NumArgs; i++) {
        const CsComputeArg *a = &r->Args[i];
        if (a->Type == CS_AFDM_TYPE) {
            struct cs_memory *m = (void *)cs_task_object(t, a->u.DevMem.MemHandle, MEM);
            ranges[nr++].id = htole64(m->id);
        } else {
            params[scalar++] = a->Type == CS_8BIT_VALUE_TYPE ? a->u.Value8
                             : a->Type == CS_16BIT_VALUE_TYPE ? a->u.Value16
                             : a->Type == CS_32BIT_VALUE_TYPE ? a->u.Value32
                             : a->Type == CS_128BIT_VALUE_TYPE ? (uint64_t)a->u.Value128
                                                              : a->u.Value64;
            if (a->Type == CS_128BIT_VALUE_TYPE)
                params[scalar++] = a->u.Value128 >> 64;
        }
    }
    if (t->reentry)
        params[1] = t->reentry;
    struct nvme_passthru_cmd64 cmd = {.opcode = 0xe1,
        .nsid = t->dev->nsid, .addr = nr ? (uintptr_t)ranges : 0,
        .data_len = nr * sizeof(ranges[0]), .cdw2 = f->index, .cdw3 = nr,
        .cdw10 = params[0], .cdw11 = params[0] >> 32,
        .cdw12 = params[1], .cdw13 = params[1] >> 32};
    CS_STATUS s = cs_io64(t->dev, false, &cmd);
    if (value)
        *value = cmd.result;
    return s == CS_SUCCESS && (int64_t)cmd.result < 0 ? CS_ERROR_IN_EXECUTION : s;
}

static CS_STATUS copy(struct cs_task *t, u64 *value)
{
    const CsCopyMemRequest *r = &t->req->u.CopyMem;
    struct cs_memory *m = (void *)cs_task_object(t, r->DevMem.MemHandle, MEM);
    if (r->Type == CS_COPY_WITHIN_DEVICE) {
        struct cs_memory *src = (void *)cs_task_object(t, r->u.SrcDevMem.MemHandle, MEM);
        /* Software fallback: original FEMU has no AFDM-to-AFDM command. */
        size_t chunk = (uint64_t)r->Bytes > CS_TRANSFER ? CS_TRANSFER : (size_t)r->Bytes;
        uint8_t *buffer = malloc(chunk);
        if (!buffer)
            return CS_OUT_OF_RESOURCES;
        uint64_t destination = r->DevMem.ByteOffset, source = r->u.SrcDevMem.ByteOffset;
        bool reverse = m->id == src->id && destination > source &&
                       destination - source < (uint64_t)r->Bytes;
        CS_STATUS s = CS_SUCCESS;
        for (uint64_t done = 0; done < (uint64_t)r->Bytes;) {
            uint32_t n = (uint64_t)r->Bytes - done > chunk ? chunk : r->Bytes - done;
            uint64_t position = reverse ? (uint64_t)r->Bytes - done - n : done;
            uint64_t offset = source + position;
            struct nvme_passthru_cmd read = {.opcode = 0xf2, .nsid = t->dev->nsid,
                .addr = (uintptr_t)buffer, .data_len = n, .cdw10 = offset,
                .cdw11 = offset >> 32, .cdw12 = n, .cdw14 = src->id};
            s = cs_io(t->dev, false, &read);
            if (s != CS_SUCCESS)
                break;
            offset = destination + position;
            struct nvme_passthru_cmd write = {.opcode = 0xf5, .nsid = t->dev->nsid,
                .addr = (uintptr_t)buffer, .data_len = n, .cdw10 = offset,
                .cdw11 = offset >> 32, .cdw12 = n, .cdw14 = m->id};
            s = cs_io(t->dev, false, &write);
            if (s != CS_SUCCESS)
                break;
            done += n;
        }
        free(buffer);
        if (value)
            *value = s == CS_SUCCESS ? r->Bytes : 0;
        return s;
    }
    for (uint64_t done = 0; done < (uint64_t)r->Bytes;) {
        uint32_t bytes = (uint64_t)r->Bytes - done > CS_TRANSFER ? CS_TRANSFER : r->Bytes - done;
        uint64_t off = r->DevMem.ByteOffset + done;
        struct nvme_passthru_cmd cmd = {.opcode = r->Type == CS_COPY_TO_DEVICE ? 0xf5 : 0xf2,
                                        .nsid = t->dev->nsid,
                                        .addr = (uintptr_t)r->u.HostVAddress + done,
                                        .data_len = bytes,
                                        .cdw10 = off,
                                        .cdw11 = off >> 32,
                                        .cdw12 = bytes,
                                        .cdw14 = m->id};
        CS_STATUS s = cs_io(t->dev, false, &cmd);
        if (s != CS_SUCCESS)
            return s;
        done += bytes;
    }
    if (value)
        *value = r->Bytes;
    return CS_SUCCESS;
}
static CS_STATUS storage(struct cs_task *t, u64 *value)
{
    const CsBlockIo *r = &t->req->u.StorageIo.u.BlockIo;
    struct cs_memory *m = (void *)cs_task_object(t, r->DevMem.MemHandle, MEM);
    uint64_t off = r->DevMem.ByteOffset;
    for (int i = 0; i < r->NumRanges; i++) {
        uint64_t lba = r->Range[i].StartLba;
        uint32_t remaining = r->Range[i].NumBlocks;
        while (remaining) {
            uint32_t n = remaining > 65536 ? 65536 : remaining;
            struct nvme_passthru_cmd cmd = {.opcode = r->Type == CS_STORAGE_STORE_TYPE ? 0xd1 : 0xd0,
                                            .nsid = t->dev->nsid,
                                            .cdw10 = lba,
                                            .cdw11 = lba >> 32,
                                            .cdw12 = n - 1,
                                            .cdw13 = m->id,
                                            .cdw14 = off,
                                            .cdw15 = off >> 32};
            CS_STATUS s = cs_io(t->dev, false, &cmd);
            if (s != CS_SUCCESS)
                return s;
            off += (uint64_t)n * t->dev->lba;
            lba += n;
            remaining -= n;
        }
    }
    if (value)
        *value = off - r->DevMem.ByteOffset;
    return CS_SUCCESS;
}
CS_STATUS cs_task_run(struct cs_task *t, u64 *value)
{
    if (value)
        *value = 0;
    return t->req->ReqType == CS_QUEUE_COMPUTE ? compute(t, value)
           : t->req->ReqType == CS_COPY_AFDM   ? copy(t, value)
                                               : storage(t, value);
}
static CS_STATUS queue_one(const CsBatchRequest *r, void *ctx, csQueueCallbackFn cb,
                           CS_EVT_HANDLE ev, CS_REQ_HANDLE *req, u64 *value)
{
    struct cs_task *t;
    CS_STATUS s = cs_task_clone(r, &t);
    return s == CS_SUCCESS ? cs_submit(t, NULL, ctx, cb, ev, req, value) : s;
}
CS_STATUS csQueueCopyMemRequest(const CsCopyMemRequest *r, void *ctx, csQueueCallbackFn cb,
                                CS_EVT_HANDLE ev, CS_REQ_HANDLE *req, u64 *value)
{
    if (!r)
        return CS_INVALID_ARG;
    CsBatchRequest b = {.ReqType = CS_COPY_AFDM};
    b.u.CopyMem = *r;
    return queue_one(&b, ctx, cb, ev, req, value);
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
    CS_STATUS s = queue_one(b, ctx, cb, ev, req, value);
    free(b);
    return s;
}
CS_STATUS csQueueStorageRequest(const CsStorageRequest *r, void *ctx, csQueueCallbackFn cb,
                                CS_EVT_HANDLE ev, CS_REQ_HANDLE *req, u64 *value)
{
    if (!r || r->Mode != CS_STORAGE_BLOCK_IO)
        return CS_UNSUPPORTED;
    if (r->u.BlockIo.NumRanges <= 0 || r->u.BlockIo.NumRanges > 4096)
        return CS_INVALID_ARG;
    size_t bytes = offsetof(CsBatchRequest, u.StorageIo.u.BlockIo.Range) +
                   (size_t)r->u.BlockIo.NumRanges * sizeof(CSBlockRange);
    if (bytes < sizeof(CsBatchRequest))
        bytes = sizeof(CsBatchRequest);
    CsBatchRequest *b = calloc(1, bytes);
    if (!b)
        return CS_OUT_OF_RESOURCES;
    b->ReqType = CS_STORAGE_IO;
    memcpy(&b->u.StorageIo, r,
           offsetof(CsStorageRequest, u.BlockIo.Range) +
               (size_t)r->u.BlockIo.NumRanges * sizeof(CSBlockRange));
    CS_STATUS s = queue_one(b, ctx, cb, ev, req, value);
    free(b);
    return s;
}
