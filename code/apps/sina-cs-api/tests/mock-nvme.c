/* Emulates only the ORIGINAL FEMU commands. Unknown extensions fail tests. */
#define _GNU_SOURCE
#include "internal.h"
#include <dlfcn.h>
#include <stdio.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *memory[4096];
static uint64_t sizes[4096], used;
static unsigned next_id = 1;
static struct {
    bool active;
    void *library;
    int64_t (*entry)(struct femu_csd_args *);
} programs[1024];
atomic_uint mock_active, mock_peak;
int cs_transport_open(struct cs_device *d, const char *name)
{
    if (strcmp(name, "/dev/nvme0n1"))
        return -1;
    d->nsid = 1;
    d->lba = 512;
    strcpy(d->path, name);
    return 0;
}
void cs_transport_close(struct cs_device *d)
{
    (void)d;
}
CS_STATUS cs_io(struct cs_device *d, bool admin, struct nvme_passthru_cmd *c)
{
    (void)d;
    (void)admin;
    pthread_mutex_lock(&lock);
    CS_STATUS s = CS_SUCCESS;
    void *data = (void *)(uintptr_t)c->addr;
    if (c->opcode == 0xb0) {
        uint64_t bytes = c->cdw10 | ((uint64_t)c->cdw11 << 32);
        if (bytes > (64U << 20) - used || next_id >= 4096)
            s = CS_NOT_ENOUGH_MEMORY;
        else {
            memory[next_id] = calloc(1, bytes);
            sizes[next_id] = bytes;
            used += bytes;
            c->result = next_id++;
        }
    } else if (c->opcode == 0xc0) {
        unsigned id = c->cdw10;
        if (id >= 4096 || !memory[id])
            s = CS_UNKNOWN_MEMORY;
        else {
            used -= sizes[id];
            sizes[id] = 0;
            free(memory[id]);
            memory[id] = NULL;
        }
    } else if (c->opcode == 0xf5 || c->opcode == 0xf2) {
        uint64_t off = c->cdw10 | ((uint64_t)c->cdw11 << 32);
        unsigned id = c->cdw14;
        if (id >= 4096 || off > sizes[id] || c->data_len > sizes[id] - off)
            s = CS_INVALID_LENGTH;
        else if (c->opcode == 0xf5)
            memcpy((char *)memory[id] + off, data, c->data_len);
        else
            memcpy(data, (char *)memory[id] + off, c->data_len);
    } else if (c->opcode == 0x25) {
        unsigned id = c->cdw10 & 65535;
        const char *file = data, *symbol = file + strlen(file) + 1;
        (void)file;
        if (programs[id].library)
            dlclose(programs[id].library);
        programs[id].active = false;
        programs[id].library = dlopen("../../csd-programs/sina-cs-scale.so", RTLD_NOW | RTLD_LOCAL);
        programs[id].entry = programs[id].library ? dlsym(programs[id].library, symbol) : NULL;
        if (!programs[id].entry)
            s = CS_LOAD_ERROR;
    } else if (c->opcode == 0x23)
        programs[c->cdw10 & 65535].active = c->cdw10 >> 16;
    else if (c->opcode == 0x22) {
        unsigned id = c->cdw10 & 65535;
        programs[id].entry = NULL;
        programs[id].active = false;
        if (programs[id].library)
            dlclose(programs[id].library);
        programs[id].library = NULL;
    } else if (c->opcode == 0xd0) {
        uint32_t n = (c->cdw12 & 65535) + 1;
        unsigned id = c->cdw13;
        uint64_t off = c->cdw14 | ((uint64_t)c->cdw15 << 32);
        memset((char *)memory[id] + off, 0xa5, n * 512);
        c->result = n * 512;
    } else if (c->opcode == 0xe1) {
        unsigned nr = c->cdw3, id = c->cdw2 & 65535;
        void *addresses[CS_API_MAX_ARGS + 1];
        long long lengths[CS_API_MAX_ARGS + 1];
        /* Linux ioctl cannot carry arbitrary CDW4/5 via metadata address. */
        if (!nr || nr > CS_API_MAX_ARGS + 1 || c->data_len != nr * 32 || c->metadata ||
            c->metadata_len || c->cdw15 || !programs[id].entry || !programs[id].active) {
            s = CS_INVALID_ARG;
            goto done;
        }
        for (unsigned i = 0; i < nr; i++) {
            uint32_t ns, length;
            uint64_t memid;
            memcpy(&ns, (char *)data + i * 32, 4);
            memcpy(&length, (char *)data + i * 32 + 4, 4);
            memcpy(&memid, (char *)data + i * 32 + 8, 8);
            if (ns || memid >= 4096 || !memory[memid] || length > sizes[memid]) {
                s = CS_INVALID_LENGTH;
                goto done;
            }
            addresses[i] = memory[memid];
            lengths[i] = length ? length : sizes[memid];
        }
        struct femu_csd_args ka = {.numr = nr, .mr_addr = addresses, .mr_len = lengths,
            .cparam1 = c->cdw10 | ((uint64_t)c->cdw11 << 32),
            .cparam2 = c->cdw12 | ((uint64_t)c->cdw13 << 32)};
        int64_t (*entry)(struct femu_csd_args *) = programs[id].entry;
        pthread_mutex_unlock(&lock);
        unsigned active = atomic_fetch_add(&mock_active, 1) + 1;
        unsigned peak = atomic_load(&mock_peak);
        while (active > peak && !atomic_compare_exchange_weak(&mock_peak, &peak, active)) {
        }
        int64_t result = entry(&ka);
        atomic_fetch_sub(&mock_active, 1);
        pthread_mutex_lock(&lock);
        c->result = result > UINT32_MAX ? UINT32_MAX : (uint32_t)result;
    } else {
        fprintf(stderr, "unexpected non-original opcode 0x%x\n", c->opcode);
        abort();
    }
done:
    pthread_mutex_unlock(&lock);
    return s;
}
uint64_t mock_used_bytes(void)
{
    pthread_mutex_lock(&lock);
    uint64_t value = used;
    pthread_mutex_unlock(&lock);
    return value;
}
