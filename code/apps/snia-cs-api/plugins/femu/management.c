#include "femu.h"
CS_STATUS femu_download(void *device, const CsCSFDownloadInfo *info, u32 *out)
{
    if (!info || !out)
        return CS_INVALID_ARG;
    if (info->CSEEId != 1 || !info->Index || info->Index > 65535)
        return CS_UNSUPPORTED_INDEX;
    struct cs_device *d = device;
    CS_STATUS s;
    pthread_mutex_lock(&cs_catalog_lock);
    struct cs_catalog_entry *e = cs_catalog_find_locked(d, info->Index);
    if ((e && e->info.active)) {
        s = CS_HANDLE_IN_USE;
        goto done;
    }
    if (info->common.Unload) {
        if (!e || !e->info.type) {
            s = CS_INVALID_CSF_ID;
            goto done;
        }
        struct nvme_passthru_cmd cmd = {
            .opcode = 0x22, .nsid = d->nsid, .cdw10 = info->Index | (1U << 24)};
        s = cs_io(d, true, &cmd);
        if (s == CS_SUCCESS) {
            memset(&e->info, 0, sizeof(e->info));
            e->info.id = info->Index;
        }
    } else {
        if (info->Type != CS_CSF_VENDOR_SPECIFIC) {
            s = CS_UNSUPPORTED_TYPE;
            goto done;
        }
        if (info->common.Length <= 2 || info->common.Length > 4096 || !info->common.DataBuffer ||
            !info->common.UniqueName[0] || strnlen(info->common.UniqueName, 32) == 32) {
            s = CS_INVALID_ARG;
            goto done;
        }
        /* Vendor binding: filename\0symbol\0 in the configured program directory. */
        const char *data = info->common.DataBuffer;
        size_t first = strnlen(data, info->common.Length);
        if (first == (size_t)info->common.Length || !first || memchr(data, '/', first) ||
            !memchr(data + first + 1, 0, info->common.Length - first - 1)) {
            s = CS_INVALID_ARG;
            goto done;
        }
        e = cs_catalog_reserve_locked(d, info->Index);
        if (!e) {
            s = CS_OUT_OF_RESOURCES;
            goto done;
        }
        struct nvme_passthru_cmd cmd = {.opcode = 0x25,
            .nsid = d->nsid, .addr = (uintptr_t)data, .data_len = info->common.Length,
            .cdw10 = info->Index | (3U << 16), .cdw11 = info->common.Length,
            .cdw12 = info->GlobalId, .cdw13 = info->GlobalId >> 32,
            .cdw14 = info->common.Length};
        s = cs_io(d, true, &cmd);
        /* Original load may replace a slot before failing. Never retain stale metadata. */
        memset(&e->info, 0, sizeof(e->info));
        e->info.id = info->Index;
        if (s == CS_SUCCESS) {
            e->info.type = 3;
            e->info.global_id = info->GlobalId;
            memcpy(e->info.name, info->common.UniqueName, 32);
        }
    }
    if (s == CS_SUCCESS)
        *out = info->Index;
done:
    pthread_mutex_unlock(&cs_catalog_lock);
    return s;
}

CS_STATUS femu_config(void *device, int *length, const CsConfigInfo *info, CsConfigData *out)
{
    if (!info || !length)
        return CS_INVALID_ARG;
    struct cs_device *d = device;
    CS_STATUS s;
    pthread_mutex_lock(&cs_catalog_lock);
    if (info->Type != CS_CSF_ACTIVATE) {
        s = CS_UNSUPPORTED;
        goto done;
    }
    const CSFConfig *c = &info->u.CSFActivateInfo;
    if (c->EEIId != 1 || c->NumCRs) {
        s = CS_UNSUPPORTED;
        goto done;
    }
    if (!c->CSFId || c->CSFId > 65535 ||
        (c->State != CS_ACTIVE_STATE && c->State != CS_INACTIVE_STATE)) {
        s = CS_INVALID_ARG;
        goto done;
    }
    struct cs_catalog_entry *e = cs_catalog_find_locked(d, c->CSFId);
    if (!e || !e->info.type) {
        s = CS_INVALID_CSF_ID;
        goto done;
    }
    s = cs_output(length, out, sizeof(*out));
    if (s != CS_SUCCESS)
        goto done;
    struct nvme_passthru_cmd cmd = {
        .opcode = 0x23, .nsid = d->nsid, .cdw10 = c->CSFId | ((uint32_t)c->State << 16)};
    s = cs_io(d, true, &cmd);
    if (s == CS_SUCCESS) {
        e->info.active = c->State == CS_ACTIVE_STATE;
        memset(out, 0, sizeof(*out));
        out->Data = (CsActivationInfo){c->State, c->CSFId};
    }
done:
    pthread_mutex_unlock(&cs_catalog_lock);
    return s;
}
