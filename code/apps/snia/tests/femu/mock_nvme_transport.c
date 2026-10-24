/* Emulates the FEMU commands used by this backend, including internal STORE. */
#define _GNU_SOURCE
#include "femu_internal.h"
#include "csd-graph.h"
#include <endian.h>
#include <dlfcn.h>
#include <glob.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *memory[4096];
static uint64_t sizes[4096], used;
static unsigned next_id = 1;
static uint8_t nvm[32U << 20];
static bool nvm_initialized;
static unsigned next_session=1,next_spill=1;
static uint64_t hidden_used,graph_commands;
static struct { void *data; uint64_t bytes,creator; unsigned session; } spills[16384];
static CS_STATUS mock_graph(struct cs_device *,struct nvme_passthru_cmd64 *);
static struct {
    bool active;
    void *library;
    int64_t (*entry)(struct femu_csd_args *);
} programs[1024];
int cs_transport_open(struct cs_device *d, const char *name)
{
    if (strcmp(name, "/dev/nvme0n1"))
        return -1;
    d->nsid = 1;
    d->lba = 512;
    d->blocks = sizeof(nvm) / 512;
    strcpy(d->path, name);
    return 0;
}
void cs_transport_close(struct cs_device *d)
{
    (void)d;
}
CS_STATUS cs_io64(struct cs_device *d, bool admin, struct nvme_passthru_cmd64 *c)
{
    (void)d;
    (void)admin;
    if(c->opcode==FEMU_CSD_EXEC_GRAPH) return mock_graph(d,c);
    pthread_mutex_lock(&lock);
    CS_STATUS s = CS_SUCCESS;
    void *data = (void *)(uintptr_t)c->addr;
    if(c->opcode==FEMU_CSD_GRAPH_SESSION) {
        if(c->cdw10==FEMU_GRAPH_OPEN) c->result=next_session++;
        else if(c->cdw10==FEMU_GRAPH_CLOSE || c->cdw10==FEMU_GRAPH_ROLLBACK) {
            uint64_t cookie=c->cdw12|((uint64_t)c->cdw13<<32);
            for(unsigned i=1;i<next_spill;++i) if(spills[i].data && spills[i].session==c->cdw11 &&
                (c->cdw10==FEMU_GRAPH_CLOSE || spills[i].creator==cookie)) {
                hidden_used-=spills[i].bytes; free(spills[i].data); spills[i].data=NULL;
            }
        }
    } else if (admin && c->opcode == 0xc0) {
        uint64_t values[] = {1, 64U << 20, used, 4, 3};
        if (c->cdw10 >= 5)
            s = CS_INVALID_ARG;
        else
            c->result = values[c->cdw10];
    } else if (c->opcode == 0x09) {
        const uint32_t *desc = data;
        uint64_t first;
        memcpy(&first, desc + 2, 8);
        if (c->data_len != 16 || c->cdw11 != 4 || first > sizeof(nvm) / 512 ||
            desc[1] > sizeof(nvm) / 512 - first)
            s = CS_INVALID_LENGTH;
        else
            memset(nvm + first * 512, 0, (uint64_t)desc[1] * 512);
    } else if (c->opcode == 0xb0) {
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
        char library[4096];
        snprintf(library, sizeof(library), "%s/%s", CS_TEST_PROGRAM_DIR, file);
        programs[id].library = dlopen(library, RTLD_NOW | RTLD_LOCAL);
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
    } else if (c->opcode == 0xd0 || c->opcode == 0xd1) {
        uint32_t n = (c->cdw12 & 65535) + 1;
        unsigned id = c->cdw13;
        uint64_t off = c->cdw14 | ((uint64_t)c->cdw15 << 32);
        uint64_t lba = c->cdw10 | ((uint64_t)c->cdw11 << 32);
        if (c->addr || c->data_len || id >= 4096 || !memory[id] ||
            off > sizes[id] || (uint64_t)n * 512 > sizes[id] - off ||
            lba > sizeof(nvm) / 512 || n > sizeof(nvm) / 512 - lba) {
            s = CS_INVALID_LENGTH;
            goto done;
        }
        if (!nvm_initialized) {
            memset(nvm, 0xa5, sizeof(nvm));
            nvm_initialized = true;
        }
        if (c->opcode == 0xd1)
            memcpy(nvm + lba * 512, (char *)memory[id] + off, n * 512);
        else
            memcpy((char *)memory[id] + off, nvm + lba * 512, n * 512);
        c->result = n * 512;
    } else if (c->opcode == 0xe1) {
        unsigned nr = c->cdw3, id = c->cdw2 & 65535;
        void *addresses[CS_API_MAX_ARGS + 1];
        long long lengths[CS_API_MAX_ARGS + 1];
        /* Linux ioctl cannot carry arbitrary CDW4/5 via metadata address. */
        if (nr > CS_API_MAX_ARGS || c->data_len != nr * 32 || c->metadata ||
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
        int64_t result = entry(&ka);
        pthread_mutex_lock(&lock);
        c->result = (uint64_t)result;
    } else {
        fprintf(stderr, "unexpected non-original opcode 0x%x\n", c->opcode);
        abort();
    }
done:
    pthread_mutex_unlock(&lock);
    return s;
}
static CS_STATUS mock_graph(struct cs_device *d,struct nvme_passthru_cmd64 *c)
{
    struct femu_graph_header *h=(void *)(uintptr_t)c->addr;
    struct femu_graph_node *nodes=(void *)(h+1);
    unsigned count=le32toh(h->count);
    uint64_t done=0,all=UINT64_MAX>>(64-count);
    CS_STATUS status=CS_SUCCESS;
    pthread_mutex_lock(&lock); ++graph_commands; pthread_mutex_unlock(&lock);
    while(done!=all && status==CS_SUCCESS) {
        uint64_t before=done;
        for(unsigned i=0;i<count && status==CS_SUCCESS;++i) {
            struct femu_graph_node *n=&nodes[i];
            if((done&(UINT64_C(1)<<i)) || (le64toh(n->dependencies)&~done)) continue;
            if(le32toh(n->operation)==FEMU_GRAPH_EXEC) {
                struct nvme_passthru_cmd64 exec={.opcode=0xe1,.nsid=d->nsid,
                    .addr=(uintptr_t)h+le32toh(n->payload),.data_len=le32toh(n->count)*32,
                    .cdw2=le32toh(n->program),.cdw3=le32toh(n->count),
                    .cdw10=le64toh(n->cparam1),.cdw11=le64toh(n->cparam1)>>32,
                    .cdw12=le64toh(n->cparam2),.cdw13=le64toh(n->cparam2)>>32};
                for(;;) {
                    status=cs_io64(d,false,&exec);
                    c->result=exec.result;
                    if((int64_t)exec.result<0) status=CS_ERROR_IN_EXECUTION;
                    uint64_t action=exec.result>>60,payload=exec.result&CS_IO_VALUE_MASK;
                    if(status!=CS_SUCCESS || action<1 || action>3) break;
                    uint64_t mrid;
                    memcpy(&mrid,(void *)(uintptr_t)(exec.addr+(exec.cdw3-1)*32+8),8);
                    pthread_mutex_lock(&lock);
                    unsigned token=payload;
                    if(action==1) {
                        if(!payload || payload>sizes[mrid] || payload>(28U<<20)-hidden_used || next_spill>=16384) status=CS_NOT_ENOUGH_MEMORY;
                        else {
                            token=next_spill++; spills[token].data=malloc(payload);
                            spills[token].bytes=payload; spills[token].session=le64toh(h->session);
                            spills[token].creator=le64toh(h->cookie);
                            hidden_used+=payload; memcpy(spills[token].data,memory[mrid],payload);
                        }
                    } else if(token>=next_spill || !spills[token].data || spills[token].session!=le64toh(h->session)) status=CS_INVALID_ID;
                    else if(action==2) memcpy(memory[mrid],spills[token].data,spills[token].bytes);
                    else { hidden_used-=spills[token].bytes; free(spills[token].data); spills[token].data=NULL; }
                    pthread_mutex_unlock(&lock);
                    if(status!=CS_SUCCESS) break;
                    exec.cdw12=token; exec.cdw13=0;
                }
            } else {
                struct femu_graph_range *ranges=(void *)((char *)h+le32toh(n->payload));
                uint64_t off=le64toh(n->afdm_offset),total=0;
                for(unsigned j=0;j<le32toh(n->count) && status==CS_SUCCESS;++j) {
                    uint64_t lba=le64toh(ranges[j].lba),remaining=le32toh(ranges[j].blocks);
                    while(remaining && status==CS_SUCCESS) {
                        unsigned blocks=remaining>65536?65536:remaining;
                        struct nvme_passthru_cmd64 io={.opcode=le32toh(n->operation)==FEMU_GRAPH_STORE?0xd1:0xd0,
                            .nsid=d->nsid,.cdw10=lba,.cdw11=lba>>32,.cdw12=blocks-1,
                            .cdw13=le32toh(n->afdm),.cdw14=off,.cdw15=off>>32};
                        status=cs_io64(d,false,&io); lba+=blocks; remaining-=blocks;
                        off+=(uint64_t)blocks*d->lba; total+=(uint64_t)blocks*d->lba;
                    }
                }
                c->result=total;
            }
            done|=UINT64_C(1)<<i;
        }
        if(before==done) status=CS_INVALID_ARG;
    }
    if(status!=CS_SUCCESS) {
        pthread_mutex_lock(&lock);
        for(unsigned i=1;i<next_spill;++i) if(spills[i].data && spills[i].session==le64toh(h->session) && spills[i].creator==le64toh(h->cookie)) {
            hidden_used-=spills[i].bytes; free(spills[i].data); spills[i].data=NULL;
        }
        pthread_mutex_unlock(&lock);
    }
    return status;
}
__attribute__((visibility("default"))) uint64_t mock_graph_commands(void)
{ pthread_mutex_lock(&lock); uint64_t v=graph_commands; pthread_mutex_unlock(&lock); return v; }
__attribute__((visibility("default"))) uint64_t mock_hidden_used(void)
{ pthread_mutex_lock(&lock); uint64_t v=hidden_used; pthread_mutex_unlock(&lock); return v; }
__attribute__((visibility("default"))) uint64_t mock_used_bytes(void)
{
    pthread_mutex_lock(&lock);
    uint64_t value = used;
    pthread_mutex_unlock(&lock);
    return value;
}

CS_STATUS cs_io(struct cs_device *d, bool admin, struct nvme_passthru_cmd *c)
{
    struct nvme_passthru_cmd64 wide = {0};
    memcpy(&wide, c, offsetof(struct nvme_passthru_cmd, result));
    CS_STATUS s = cs_io64(d, admin, &wide);
    c->result = (uint32_t)wide.result;
    return s;
}
int __wrap_glob(const char *pattern, int flags, int (*error)(const char *, int), glob_t *out)
{
    (void)flags; (void)error;
    static char *paths[] = {"/dev/nvme0n1", NULL};
    if (strcmp(pattern, "/dev/nvme*n*")) return GLOB_NOMATCH;
    out->gl_pathc = 1; out->gl_pathv = paths; return 0;
}
void __wrap_globfree(glob_t *out) { (void)out; }
