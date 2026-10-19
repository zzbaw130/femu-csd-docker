/* SNIA CSx/CSF discovery using the FEMU transport and process-local catalog. */
#define _GNU_SOURCE
#include "femu_common.h"
#include <endian.h>
#include <glob.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
CS_STATUS femu_getCSxFromPath(const char *path, unsigned int *length, char *out)
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
    struct cs_device *d;
    unsigned workers;
    CS_STATUS s = femu_openCSx(device, NULL, (void **)&d, &workers);
    if (s != CS_SUCCESS)
        return CS_NO_MATCHING_DEVICE;
    femu_closeCSx(d);
    unsigned need = strlen(device) + 1, have = *length;
    *length = need;
    if (!out || have < need)
        return CS_INVALID_LENGTH;
    memcpy(out, device, need);
    return CS_SUCCESS;
}
CS_STATUS femu_queryCSxList(int *length, char *buffer)
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
        struct cs_device *d;
    unsigned workers;
        if (femu_openCSx(g.gl_pathv[i], NULL, (void **)&d, &workers) != CS_SUCCESS)
            continue;
        femu_closeCSx(d);
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
CS_STATUS femu_queryCSFList(const char *path, int *length, int *count, CSFUniqueId *out)
{
    if (!path || !length || !count)
        return CS_INVALID_ARG;
    char device[4096];
    unsigned n = sizeof(device);
    CS_STATUS s = femu_getCSxFromPath(path, &n, device);
    if (s != CS_SUCCESS)
        return s;
    struct cs_device *d;
    unsigned workers;
    s = femu_openCSx(device, NULL, (void **)&d, &workers);
    if (s != CS_SUCCESS)
        return s;
    struct cs_program_info *programs;
    size_t total;
    s = cs_collect(d, &programs, &total);
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
    femu_closeCSx(d);
    return s;
}
