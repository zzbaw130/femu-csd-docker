#include "femu.h"
#include <endian.h>
static CS_STATUS compute(struct cs_task *t, u64 *value)
{
    const CsComputeRequest *r = &t->req->u.Compute;
    uint32_t function_id = (uint32_t)(uintptr_t)r->CSFId;
    /* Native MR descriptors only. No argument AFDM or type description. */
    struct { uint32_t nsid, len; uint64_t id, reserved[2]; } ranges[CS_API_MAX_ARGS] = {0};
    uint64_t params[2] = {0};
    unsigned nr = 0, scalar = 0;
    for (int i = 0; i < r->NumArgs; i++) {
        const CsComputeArg *a = &r->Args[i];
        if (a->Type == CS_AFDM_TYPE) {
            struct cs_memory *m = (void *)a->u.DevMem.MemHandle;
            ranges[nr++].id = htole64(m->id);
        } else {
            params[scalar++] = a->Type == CS_8BIT_VALUE_TYPE ? a->u.Value8
                             : a->Type == CS_16BIT_VALUE_TYPE ? a->u.Value16
                             : a->Type == CS_32BIT_VALUE_TYPE ? a->u.Value32
                             : a->u.Value64;
        }
    }
    if (t->reentry)
        params[1] = t->reentry;
    struct nvme_passthru_cmd64 cmd = {.opcode = 0xe1,
        .nsid = t->dev->nsid, .addr = nr ? (uintptr_t)ranges : 0,
        .data_len = nr * sizeof(ranges[0]), .cdw2 = function_id, .cdw3 = nr,
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
    struct cs_memory *m = (void *)r->DevMem.MemHandle;
    if (r->Type == CS_COPY_WITHIN_DEVICE) {
        struct cs_memory *src = (void *)r->u.SrcDevMem.MemHandle;
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
    struct cs_memory *m = (void *)r->DevMem.MemHandle;
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
