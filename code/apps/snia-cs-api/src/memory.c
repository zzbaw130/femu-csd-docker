#include "internal.h"
#include <endian.h>
static void memory_destroy(struct cs_object *o)
{
    struct cs_memory *m = (void *)o;
    cs_put(&m->dev->base);
}
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
CS_STATUS csAllocMem(CS_DEV_HANDLE h, int bytes, const CsMemFlags *f, CS_MEM_HANDLE *out,
                     CS_MEM_PTR *va)
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
    struct cs_device *d = (void *)cs_get(h, DEV);
    if (!d)
        return CS_INVALID_HANDLE;
    uint64_t size = ((uint64_t)bytes + 4095) & ~UINT64_C(4095);
    struct nvme_passthru_cmd cmd = {
        .opcode = 0xb0, .nsid = d->nsid, .cdw10 = (uint32_t)size, .cdw11 = size >> 32};
    CS_STATUS s = cs_io(d, false, &cmd);
    if (s != CS_SUCCESS) {
        cs_put(&d->base);
        return s;
    }
    struct cs_memory *m = (void *)cs_new(sizeof(*m), MEM, memory_destroy);
    if (!m) {
        struct nvme_passthru_cmd drop = {.opcode = 0xc0, .nsid = d->nsid, .cdw10 = cmd.result};
        cs_io(d, false, &drop);
        cs_put(&d->base);
        return CS_OUT_OF_RESOURCES;
    }
    m->dev = d;
    m->id = cmd.result;
    m->bytes = size;
    s = cs_fill(m, 0, size, f);
    if (s != CS_SUCCESS) {
        CS_MEM_HANDLE temp = (void *)m->base.id;
        CsMemFlags none = {0};
        csFreeMem(temp, &none);
        return s;
    }
    *out = (void *)m->base.id;
    return CS_SUCCESS;
}
CS_STATUS csInitMem(CS_MEM_HANDLE h, unsigned long off, int bytes, const CsMemFlags *f)
{
    if (!f || bytes <= 0)
        return CS_INVALID_ARG;
    struct cs_memory *m = (void *)cs_get(h, MEM);
    if (!m)
        return CS_INVALID_HANDLE;
    CS_STATUS s = cs_fill(m, off, bytes, f);
    cs_put(&m->base);
    return s;
}
CS_STATUS csFreeMem(CS_MEM_HANDLE h, const CsMemFlags *f)
{
    if (!f)
        return CS_INVALID_ARG;
    if (f->Flags & ~3U)
        return CS_INVALID_OPTION;
    struct cs_memory *m = (void *)cs_get(h, MEM);
    if (!m)
        return CS_INVALID_HANDLE;
    /* Retire first so no new request can acquire the allocation while freeing. */
    CS_STATUS s = cs_retire(&m->base);
    if (s != CS_SUCCESS) {
        cs_put(&m->base);
        return CS_MEMORY_IN_USE;
    }
    s = cs_fill(m, 0, m->bytes, f);
    if (s != CS_SUCCESS) {
        cs_restore(&m->base);
        cs_put(&m->base);
        return s;
    }
    struct nvme_passthru_cmd cmd = {.opcode = 0xc0, .nsid = m->dev->nsid, .cdw10 = m->id};
    CS_STATUS freed = cs_io(m->dev, false, &cmd);
    if (freed != CS_SUCCESS)
        cs_restore(&m->base);
    cs_put(&m->base);
    return freed;
}
