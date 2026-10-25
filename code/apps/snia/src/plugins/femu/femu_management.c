/* SNIA CSF download, activation and device-property queries. */
#include "femu_internal.h"
#include <stdio.h>
CS_STATUS femu_CSFDownload(void *device, const CsCSFDownloadInfo *info, u32 *out)
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

CS_STATUS femu_queryDeviceProperties(void *device, CS_RESOURCE_TYPE type, int *length,
                                  CsProperties *out)
{
    if (!length)
        return CS_INVALID_ARG;
    struct cs_device *d = device;
    CS_STATUS s = CS_SUCCESS;
    struct cs_program_info *list = NULL;
    size_t count = 0, extra = 0;
    if (type == CS_CSF_TYPE || type == CS_CSEE_TYPE) {
        s = cs_collect(d, &list, &count);
        if (s != CS_SUCCESS)
            goto done;
    }
    if (type == CS_CSE_TYPE)
        extra = d->caps.workers * sizeof(ComputeResource);
    else if (type == CS_CSEE_TYPE)
        extra = sizeof(CSEEInstance);
    else if (type == CS_FDM_TYPE)
        extra = sizeof(CSEAccess);
    else if (type == CS_CSF_TYPE)
        extra = count * (sizeof(CSFInfo) + sizeof(CSFInstance));
    else if (type != CS_CSx_TYPE) {
        s = CS_UNSUPPORTED_TYPE;
        goto done;
    }
    size_t needed = sizeof(*out) + extra;
    s = cs_output(length, out, needed);
    if (s != CS_SUCCESS)
        goto done;
    memset(out, 0, needed);
    void *tail = (unsigned char *)out + sizeof(*out);
    if (type == CS_CSx_TYPE) {
        out->CSxDetails = (CSxProperties){
            .HwVersion = 0, .SwVersion = 0, .VendorId = 0x1d1d, .DeviceId = 0x1f1f};
        strcpy(out->CSxDetails.FriendlyName,
               "FEMU SNIA CS API"); /* Software batch: hardware flag stays zero. */
    } else if (type == CS_CSE_TYPE) {
        out->CSEDetails[0].NumCSEs = 1;
        CSEInfo *c = &out->CSEDetails[0].CSE[0];
        c->CSETypeToken = c->CSEId = 1;
        c->MaxRequestsPerBatch = CS_MAX_BATCH;
        c->MaxCSFParametersAllowed = CS_API_MAX_ARGS;
        c->MaxCSEEs = c->NumActivatedCSEEs = 1;
        c->NumCRs = d->caps.workers;
        /* The fixed native CSEE owns every CR. Unallocated != currently idle. */
        c->NumAvailableCRs = 0;
        c->CRs = tail;
        for (unsigned i = 0; i < d->caps.workers; i++) {
            c->CRs[i].ERId = i + 1;
            snprintf(c->CRs[i].Name, sizeof(c->CRs[i].Name), "femu-cu-%u", i);
        }
    } else if (type == CS_CSEE_TYPE) {
        CSEEProperties *p = &out->CSEEDetails[0];
        p->NumCSEEs = 1;
        CSEEInfo *c = &p->CSEE[0];
        c->Type = CS_CSEE_VENDOR_SPECIFIC;
        c->SwVersion = 0;
        strcpy(c->Name, "femu-native");
        c->CSETypeToken = 1;
        c->CSEEId = 1;
        c->MaxCSFs = 65535;
        c->NumEEIs = 1;
        c->EEInstances = tail;
        c->EEInstances[0] = (CSEEInstance){CS_ACTIVE_STATE, 1, 1};
        for (size_t i = 0; i < count; i++)
            c->NumActivatedCSFs += list[i].active != 0;
    } else if (type == CS_FDM_TYPE) {
        FDMProperties *p = &out->FDMDetails[0];
        p->NumFDMs = 1;
        p->FDM[0] = (FDMInfo){
            .FDMId = 1, .FDMSize = d->caps.fdm_bytes, .NumCSEs = 1, .CSEList = tail};
        p->FDM[0].Flags.DeviceManaged = 1;
        p->FDM[0].Flags.ClearContentsSupported = 1;
        p->FDM[0].Flags.InvalidateContentsSupported = 0;
        p->FDM[0].CSEList[0].CSEId = 1;
    } else {
        CSFProperties *p = &out->CSFDetails[0];
        p->NumCSFs = count;
        /* Start the variable-length CSF array at its actual ABI offset. */
        size_t needed2 = offsetof(CsProperties, CSFDetails[0].CSF) + count * sizeof(CSFInfo) +
                         count * sizeof(CSFInstance);
        if (needed2 > needed) {
            s = CS_DEVICE_ERROR;
            goto done;
        }
        CSFInstance *instances = (void *)((unsigned char *)p->CSF + count * sizeof(CSFInfo));
        for (size_t i = 0; i < count; i++) {
            CSFInfo *c = &p->CSF[i];
            c->GlobalId = list[i].global_id;
            memcpy(c->UniqueName, list[i].name, 32);
            c->CSFId = list[i].id;
            c->CSETypeToken = 1;
            c->NumFIs = list[i].active ? 1 : 0;
            c->FInstances = &instances[i];
            instances[i] = (CSFInstance){.State = list[i].active ? CS_ACTIVE_STATE
                                                                          : CS_INACTIVE_STATE,
                                         .EEIId = 1,
                                         .FIId = c->CSFId};
        }
    }
done:
    free(list);
    return s;
}
