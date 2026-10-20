/* SNIA block storage I/O mapped to internal NVM <-> AFDM commands. */
#include "femu_internal.h"
CS_STATUS femu_queueStorageRequest(void *backend, const CsStorageRequest *request, u64 *value)
{
    struct cs_device *device = backend;
    const CsBlockIo *r = &request->u.BlockIo;
    struct cs_memory *m = (void *)r->DevMem.MemHandle;
    uint64_t off = r->DevMem.ByteOffset;
    for (int i = 0; i < r->NumRanges; i++) {
        uint64_t lba = r->Range[i].StartLba;
        uint32_t remaining = r->Range[i].NumBlocks;
        while (remaining) {
            uint32_t n = remaining > 65536 ? 65536 : remaining;
            struct nvme_passthru_cmd cmd = {.opcode = r->Type == CS_STORAGE_STORE_TYPE ? 0xd1 : 0xd0,
                                            .nsid = device->nsid,
                                            .cdw10 = lba,
                                            .cdw11 = lba >> 32,
                                            .cdw12 = n - 1,
                                            .cdw13 = m->id,
                                            .cdw14 = off,
                                            .cdw15 = off >> 32};
            CS_STATUS s = cs_io(device, false, &cmd);
            if (s != CS_SUCCESS)
                return s;
            off += (uint64_t)n * device->lba;
            lba += n;
            remaining -= n;
        }
    }
    if (value)
        *value = off - r->DevMem.ByteOffset;
    return CS_SUCCESS;
}
