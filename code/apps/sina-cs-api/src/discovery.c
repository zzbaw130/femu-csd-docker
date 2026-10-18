#define _GNU_SOURCE
#include "internal.h"
#include <endian.h>
#include <glob.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
CS_STATUS csGetCSxFromPath(const char *path, unsigned int *length, char *out)
{
    if (!path || !length)
        return CS_INVALID_ARG;
    char resolved[4096], device[128];
    unsigned c, n;
    int end = 0;
    if (!realpath(path, resolved))
        return CS_INVALID_PATH;
    if (sscanf(resolved, "/dev/nvme%un%u%n", &c, &n, &end) == 2 && n &&
        (!resolved[end] || resolved[end] == 'p'))
        snprintf(device, sizeof(device), "/dev/nvme%un%u", c, n);
    else {
        struct stat st;
        if (stat(resolved, &st))
            return CS_INVALID_PATH;
        dev_t dev = S_ISBLK(st.st_mode) ? st.st_rdev : st.st_dev;
        char sys[128], target[4096];
        snprintf(sys, sizeof(sys), "/sys/dev/block/%u:%u", major(dev), minor(dev));
        if (!realpath(sys, target))
            return CS_NO_MATCHING_DEVICE;
        const char *base = strrchr(target, '/');
        base = base ? base + 1 : target;
        end = 0;
        if (sscanf(base, "nvme%un%u%n", &c, &n, &end) != 2 || !n || (base[end] && base[end] != 'p'))
            return CS_NO_MATCHING_DEVICE;
        snprintf(device, sizeof(device), "/dev/nvme%un%u", c, n);
    }
    CS_DEV_HANDLE h;
    CS_STATUS s = csOpenCSx(device, NULL, &h);
    if (s != CS_SUCCESS)
        return CS_NO_MATCHING_DEVICE;
    csCloseCSx(h);
    unsigned need = strlen(device) + 1, have = *length;
    *length = need;
    if (!out || have < need)
        return CS_INVALID_LENGTH;
    memcpy(out, device, need);
    return CS_SUCCESS;
}
CS_STATUS csQueryCSxList(int *length, char *buffer)
{
    if (!length)
        return CS_INVALID_ARG;
    glob_t g = {0};
    char *list = calloc(1, 1);
    size_t bytes = 1;
    if (!list)
        return CS_OUT_OF_RESOURCES;
    int rc = glob("/dev/nvme*n*", 0, NULL, &g);
    if (rc && rc != GLOB_NOMATCH) {
        free(list);
        return CS_DEVICE_ERROR;
    }
    for (size_t i = 0; i < g.gl_pathc; i++) {
        CS_DEV_HANDLE h;
        if (csOpenCSx(g.gl_pathv[i], NULL, &h) != CS_SUCCESS)
            continue;
        csCloseCSx(h);
        size_t n = strlen(g.gl_pathv[i]), extra = n + (bytes > 1);
        char *next = realloc(list, bytes + extra);
        if (!next) {
            globfree(&g);
            free(list);
            return CS_OUT_OF_RESOURCES;
        }
        list = next;
        if (bytes > 1)
            list[bytes++ - 1] = ',';
        memcpy(list + bytes - 1, g.gl_pathv[i], n + 1);
        bytes += n;
    }
    globfree(&g);
    CS_STATUS s = cs_output(length, buffer, bytes);
    if (s == CS_SUCCESS)
        memcpy(buffer, list, bytes);
    free(list);
    return s;
}
CS_STATUS cs_collect(struct cs_device *d, struct cs_program_info **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    unsigned next = 1;
    while (next <= 65535) {
        struct cs_program_info p;
        CS_STATUS s = cs_program(d, next, &p);
        if (s != CS_SUCCESS) {
            free(*out);
            *out = NULL;
            return s;
        }
        unsigned id = p.id;
        if (!id)
            return CS_SUCCESS;
        if (id < next || id > 65535) {
            free(*out);
            *out = NULL;
            return CS_DEVICE_ERROR;
        }
        void *arr = realloc(*out, (*count + 1) * sizeof(p));
        if (!arr) {
            free(*out);
            *out = NULL;
            return CS_OUT_OF_RESOURCES;
        }
        *out = arr;
        (*out)[(*count)++] = p;
        next = id + 1;
    }
    return CS_SUCCESS;
}
CS_STATUS csQueryCSFList(const char *path, int *length, int *count, CSFUniqueId *out)
{
    if (!path || !length || !count)
        return CS_INVALID_ARG;
    char device[4096];
    unsigned n = sizeof(device);
    CS_STATUS s = csGetCSxFromPath(path, &n, device);
    if (s != CS_SUCCESS)
        return s;
    CS_DEV_HANDLE h;
    s = csOpenCSx(device, NULL, &h);
    if (s != CS_SUCCESS)
        return s;
    struct cs_device *d = (void *)cs_get(h, DEV);
    struct cs_program_info *programs;
    size_t total;
    s = cs_collect(d, &programs, &total);
    cs_put(&d->base);
    if (s == CS_SUCCESS) {
        size_t active = 0;
        for (size_t i = 0; i < total; i++)
            active += programs[i].active != 0;
        *count = active;
        s = cs_output(length, out, active * sizeof(*out));
        if (s == CS_SUCCESS) {
            active = 0;
            for (size_t i = 0; i < total; i++)
                if (programs[i].active) {
                    memcpy(out[active].UniqueName, programs[i].name, 32);
                    out[active++].GlobalId = programs[i].global_id;
                }
        }
        free(programs);
    }
    csCloseCSx(h);
    return s;
}
static void function_destroy(struct cs_object *o)
{
    struct cs_function *f = (void *)o;
    cs_put(&f->dev->base);
}
CS_STATUS csGetCSFId(CS_DEV_HANDLE h, const char *name, u64 global, int *length, int *count,
                     CSFIdinfo *out)
{
    if (!length || !count || (!name && !global) || (name && global))
        return CS_INVALID_ARG;
    if (name && (!*name || strnlen(name, 32) == 32))
        return CS_INVALID_CSF_NAME;
    struct cs_device *d = (void *)cs_get(h, DEV);
    if (!d)
        return CS_INVALID_HANDLE;
    struct cs_program_info *list;
    size_t total;
    CS_STATUS s = cs_collect(d, &list, &total);
    if (s != CS_SUCCESS) {
        cs_put(&d->base);
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
            struct cs_function *f = (void *)cs_new(sizeof(*f), FUNCTION, function_destroy);
            if (!f) {
                s = CS_OUT_OF_RESOURCES;
                break;
            }
            cs_hold(&d->base);
            f->dev = d;
            f->index = list[i].id;
            f->type = list[i].type;
            out[n].CSFId = (void *)f->base.id;
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
    cs_put(&d->base);
    return s;
}
CS_STATUS csQueryDeviceProperties(CS_DEV_HANDLE h, CS_RESOURCE_TYPE type, int *length,
                                  CsProperties *out)
{
    if (!length)
        return CS_INVALID_ARG;
    struct cs_device *d = (void *)cs_get(h, DEV);
    if (!d)
        return CS_INVALID_HANDLE;
    CS_STATUS s = CS_SUCCESS;
    /* The original device has no CU availability query. Do not invent a value. */
    if (type == CS_CSE_TYPE) {
        cs_put(&d->base);
        return CS_UNSUPPORTED;
    }
    struct cs_program_info *list = NULL;
    size_t count = 0, extra = 0;
    if (type == CS_CSF_TYPE || type == CS_CSEE_TYPE) {
        s = cs_collect(d, &list, &count);
        if (s != CS_SUCCESS)
            goto done;
    }
    if (type == CS_CSEE_TYPE)
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
    cs_put(&d->base);
    return s;
}
CS_STATUS csQueryDeviceStatistics(CS_DEV_HANDLE h, CS_STAT_TYPE type, void *id, CsStatsInfo *out)
{
    if (!out)
        return CS_INVALID_ARG;
    struct cs_device *d = (void *)cs_get(h, DEV);
    if (!d)
        return CS_INVALID_HANDLE;
    (void)type;
    (void)id;
    cs_put(&d->base);
    return CS_UNSUPPORTED;
}
