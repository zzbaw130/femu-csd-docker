/* SNIA AFDM allocation, initialization, freeing and memory-copy requests. */
#include "femu_internal.h"
CS_STATUS cs_fill(struct cs_memory *m, uint64_t offset, uint64_t bytes, const CsMemFlags *f)
{
    if (!f || f->Flags & ~(CS_FDM_CLEAR | CS_FDM_FILL))
        return CS_INVALID_OPTION;
    if (offset > m->bytes || bytes > m->bytes - offset)
        return CS_INVALID_LENGTH;
    if (!bytes || !f->Flags)
        return CS_SUCCESS;
    size_t chunk = bytes > CS_TRANSFER ? CS_TRANSFER : (size_t)bytes;
    uint8_t *buffer = malloc(chunk);
    if (!buffer)
        return CS_OUT_OF_RESOURCES;
    uint32_t pattern = f->Flags & CS_FDM_FILL ? f->FillValue : 0;
    for (size_t i = 0; i < chunk; i++)
        buffer[i] = pattern >> ((i % 4) * 8);
    CS_STATUS s = CS_SUCCESS;
    for (uint64_t done = 0; done < bytes;) {
        uint32_t n = bytes - done > chunk ? chunk : bytes - done;
        uint64_t off = offset + done;
        struct nvme_passthru_cmd cmd = {.opcode = 0xf5, .nsid = m->dev->nsid,
            .addr = (uintptr_t)buffer, .data_len = n, .cdw10 = off,
            .cdw11 = off >> 32, .cdw12 = n, .cdw14 = m->id};
        s = cs_io(m->dev, false, &cmd);
        if (s != CS_SUCCESS)
            break;
        done += n;
    }
    free(buffer);
    return s;
}
CS_STATUS femu_allocMem(void *device, int bytes, const CsMemFlags *f, void **out,
                     u64 *allocated, CS_MEM_PTR *va)
{
    if (!out || !f || bytes <= 0)
        return CS_INVALID_ARG;
    *out = NULL;
    if (va) {
        *va = NULL;
        return CS_COULD_NOT_MAP_MEMORY;
    }
    if (f->FDMId != 1)
        return CS_INVALID_FDM_ID;
    if (f->Flags & ~3U)
        return CS_INVALID_OPTION;
    struct cs_device *d = device;
    uint64_t size = ((uint64_t)bytes + 4095) & ~UINT64_C(4095);
    struct nvme_passthru_cmd cmd = {
        .opcode = 0xb0, .nsid = d->nsid, .cdw10 = (uint32_t)size, .cdw11 = size >> 32};
    CS_STATUS s = cs_io(d, false, &cmd);
    if (s != CS_SUCCESS) {
        return s;
    }
    struct cs_memory *m = calloc(1, sizeof(*m));
    if (!m) {
        struct nvme_passthru_cmd drop = {.opcode = 0xc0, .nsid = d->nsid, .cdw10 = cmd.result};
        cs_io(d, false, &drop);
        return CS_OUT_OF_RESOURCES;
    }
    m->dev = d;
    m->id = cmd.result;
    m->bytes = size;
    s = cs_fill(m, 0, size, f);
    if (s != CS_SUCCESS) {
        void *temp = m;
        CsMemFlags none = {0};
        femu_freeMem(d, temp, &none);
        return s;
    }
    *out = m;
    *allocated = size;
    return CS_SUCCESS;
}
CS_STATUS femu_initMem(void *device, void *memory, unsigned long off, int bytes, const CsMemFlags *f)
{
    (void)device;
    return cs_fill(memory, off, bytes, f);
}
CS_STATUS femu_freeMem(void *device, void *memory, const CsMemFlags *f)
{
    (void)device;
    struct cs_memory *m = memory;
    CS_STATUS s = cs_fill(m, 0, m->bytes, f);
    if (s != CS_SUCCESS) return s;
    struct nvme_passthru_cmd cmd = {.opcode = 0xc0, .nsid = m->dev->nsid, .cdw10 = m->id};
    s = cs_io(m->dev, false, &cmd);
    if (s == CS_SUCCESS) free(m);
    return s;
}

CS_STATUS femu_queueCopyMemRequest(void *backend, const CsCopyMemRequest *r, u64 *value)
{
    struct cs_device *device = backend;
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
            struct nvme_passthru_cmd read = {.opcode = 0xf2, .nsid = device->nsid,
                .addr = (uintptr_t)buffer, .data_len = n, .cdw10 = offset,
                .cdw11 = offset >> 32, .cdw12 = n, .cdw14 = src->id};
            s = cs_io(device, false, &read);
            if (s != CS_SUCCESS)
                break;
            offset = destination + position;
            struct nvme_passthru_cmd write = {.opcode = 0xf5, .nsid = device->nsid,
                .addr = (uintptr_t)buffer, .data_len = n, .cdw10 = offset,
                .cdw11 = offset >> 32, .cdw12 = n, .cdw14 = m->id};
            s = cs_io(device, false, &write);
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
                                        .nsid = device->nsid,
                                        .addr = (uintptr_t)r->u.HostVAddress + done,
                                        .data_len = bytes,
                                        .cdw10 = off,
                                        .cdw11 = off >> 32,
                                        .cdw12 = bytes,
                                        .cdw14 = m->id};
        CS_STATUS s = cs_io(device, false, &cmd);
        if (s != CS_SUCCESS)
            return s;
        done += bytes;
    }
    if (value)
        *value = r->Bytes;
    return CS_SUCCESS;
}
