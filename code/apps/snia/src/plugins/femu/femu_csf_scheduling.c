/* SNIA CSF identifiers and native compute submission; maps MR and scalar arguments. */
#include "femu_internal.h"
#include <endian.h>
CS_STATUS femu_compute(struct cs_device *device, const CsComputeRequest *r,
                         u64 reentry, u64 *value)
{
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
    if (reentry)
        params[1] = reentry;
    struct nvme_passthru_cmd64 cmd = {.opcode = 0xe1,
        .nsid = device->nsid, .addr = nr ? (uintptr_t)ranges : 0,
        .data_len = nr * sizeof(ranges[0]), .cdw2 = function_id, .cdw3 = nr,
        .cdw10 = params[0], .cdw11 = params[0] >> 32,
        .cdw12 = params[1], .cdw13 = params[1] >> 32};
    CS_STATUS s = cs_io64(device, false, &cmd);
    if (value)
        *value = cmd.result;
    return s == CS_SUCCESS && (int64_t)cmd.result < 0 ? CS_ERROR_IN_EXECUTION : s;
}

CS_STATUS femu_queueComputeRequest(void *device, const CsComputeRequest *request, u64 *value)
{
    return femu_compute(device, request, 0, value);
}

CS_STATUS femu_getCSFId(void *device, const char *name, u64 global, int *length, int *count,
                     CSFIdinfo *out)
{
    if (!length || !count || (!name && !global) || (name && global))
        return CS_INVALID_ARG;
    if (name && (!*name || strnlen(name, 32) == 32))
        return CS_INVALID_CSF_NAME;
    struct cs_device *d = device;
    struct cs_program_info *list;
    size_t total;
    CS_STATUS s = cs_collect(d, &list, &total);
    if (s != CS_SUCCESS) {
        return s;
    }
    size_t matches = 0;
    for (size_t i = 0; i < total; i++)
        if (list[i].active &&
            (name ? !strncmp(name, list[i].name, 32) : global == list[i].global_id))
            matches++;
    *count = matches;
    if (!matches) {
        s = name ? CS_INVALID_CSF_NAME : CS_INVALID_GLOBAL_ID;
        goto done;
    }
    size_t needed = matches * (sizeof(*out) + sizeof(FDMAccess));
    s = cs_output(length, out, needed);
    if (s != CS_SUCCESS)
        goto done;
    memset(out, 0, needed);
    FDMAccess *access = (void *)(out + matches);
    size_t n = 0;
    for (size_t i = 0; i < total; i++)
        if (list[i].active &&
            (name ? !strncmp(name, list[i].name, 32) : global == list[i].global_id)) {
            out[n].CSFId = (void *)(uintptr_t)list[i].id;
            out[n].Count = 1;
            out[n].NumFDMs = 1;
            out[n].FDMList = &access[n];
            access[n].FDMId = 1;
            access[n].Flags.DeviceManaged = 1;
            access[n].Flags.ClearContentsSupported = 1;
            access[n].Flags.InvalidateContentsSupported = 0;
            n++;
        }
done:
    free(list);
    return s;
}
