/* Device-specific validation before a configured/submitted request executes. */
#include "femu_common.h"
CS_STATUS femu_validateRequest(void *device, const CsBatchRequest *r)
{
    struct cs_device *d = device;
    if (r->ReqType == CS_QUEUE_COMPUTE) {
        const CsComputeRequest *c = &r->u.Compute;
        pthread_mutex_lock(&cs_catalog_lock);
        struct cs_catalog_entry *e = cs_catalog_find_locked(d, (uintptr_t)c->CSFId);
        CS_STATUS s = !e || !e->info.active ? CS_INVALID_CSF_ID
                      : e->info.type != 3 ? CS_UNSUPPORTED_TYPE : CS_SUCCESS;
        pthread_mutex_unlock(&cs_catalog_lock);
        unsigned scalars = 0;
        for (int i = 0; s == CS_SUCCESS && i < c->NumArgs; i++) {
            const CsComputeArg *a = &c->Args[i];
            if (a->Type == CS_AFDM_TYPE) {
                struct cs_memory *m = a->u.DevMem.MemHandle;
                if (a->u.DevMem.ByteOffset > m->bytes) s = CS_INVALID_LENGTH;
                else if (a->u.DevMem.ByteOffset) s = CS_UNSUPPORTED;
            } else if (a->Type < CS_8BIT_VALUE_TYPE || a->Type > CS_64BIT_VALUE_TYPE)
                s = CS_UNSUPPORTED_TYPE;
            else if (++scalars > 2) s = CS_UNSUPPORTED;
        }
        return s;
    }
    if (r->ReqType == CS_STORAGE_IO) {
        if (r->u.StorageIo.Mode != CS_STORAGE_BLOCK_IO) return CS_UNSUPPORTED;
        const CsBlockIo *b = &r->u.StorageIo.u.BlockIo;
        if (b->Type != CS_STORAGE_LOAD_TYPE && b->Type != CS_STORAGE_STORE_TYPE) return CS_INVALID_OPTION;
        if (b->StorageIndex) return CS_UNSUPPORTED_INDEX;
        uint64_t total = 0;
        for (int i = 0; i < b->NumRanges; i++) {
            const CSBlockRange *range = &b->Range[i];
            if (range->NamespaceId != (u32)d->nsid || !range->NumBlocks) return CS_INVALID_ARG;
            if (range->StartLba > UINT64_MAX - range->NumBlocks) return CS_INVALID_LENGTH;
            total += (uint64_t)range->NumBlocks * d->lba;
        }
        struct cs_memory *m = b->DevMem.MemHandle;
        if (b->DevMem.ByteOffset > m->bytes || total > m->bytes - b->DevMem.ByteOffset)
            return CS_INVALID_LENGTH;
    }
    return CS_SUCCESS;
}
