#ifndef CS_FEMU_H
#define CS_FEMU_H
#include "cs_backend.h"
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <linux/nvme_ioctl.h>
#define CS_MAX_BATCH 64
#define CS_API_MAX_ARGS 64
#define CS_TRANSFER (1U << 20)
struct cs_device {
    int fd, admin, nsid;
    uint32_t lba;
    uint64_t blocks;
    struct cs_spill_pool *spill;
    char path[4096];
    struct { uint32_t workers; uint64_t fdm_bytes, features; } caps;
};
struct cs_memory { struct cs_device *dev; uint32_t id; uint64_t bytes; };
struct cs_task { const CsBatchRequest *req; struct cs_device *dev; unsigned scalar_slots; uint64_t reentry; };
struct cs_program_info { uint32_t id, type, active; uint64_t global_id; char name[32]; };
struct cs_catalog_entry { char path[4096]; struct cs_program_info info; struct cs_catalog_entry *next; };
extern pthread_mutex_t cs_catalog_lock;
void cs_catalog_clear(void);
struct cs_catalog_entry *cs_catalog_find_locked(struct cs_device *, unsigned);
struct cs_catalog_entry *cs_catalog_reserve_locked(struct cs_device *, unsigned);
CS_STATUS cs_program(struct cs_device *, unsigned, struct cs_program_info *);
CS_STATUS cs_collect(struct cs_device *, struct cs_program_info **, size_t *);
int cs_transport_open(struct cs_device *, const char *);
void cs_transport_close(struct cs_device *);
CS_STATUS cs_io(struct cs_device *, bool, struct nvme_passthru_cmd *);
CS_STATUS cs_io64(struct cs_device *, bool, struct nvme_passthru_cmd64 *);
int cs_profile_load(struct cs_device *, const char *);
int cs_spill_region(struct cs_device *, uint64_t *, uint64_t *);
void cs_spill_detach(struct cs_device *);
void cs_spill_scope_end(struct cs_device *, void *, bool);
CS_STATUS cs_task_run(struct cs_task *, u64 *);
CS_STATUS femu_queueComputeRequest(void *, const CsComputeRequest *, u64 *);
CS_STATUS femu_queueCopyMemRequest(void *, const CsCopyMemRequest *, u64 *);
CS_STATUS femu_queueStorageRequest(void *, const CsStorageRequest *, u64 *);
CS_STATUS femu_executeBatchEntry(void *, const CsBatchRequest *, bool (*)(void *), void *, void *, u64 *);
CS_STATUS femu_validateRequest(void *, const CsBatchRequest *);
CS_STATUS femu_openCSx(const char *, void *, void **, unsigned *);
void femu_closeCSx(void *);
CS_STATUS femu_queryCSxList(int *, char *);
CS_STATUS femu_getCSxFromPath(const char *, unsigned *, char *);
CS_STATUS femu_queryCSFList(const char *, int *, int *, CSFUniqueId *);
CS_STATUS femu_getCSFId(void *, const char *, u64, int *, int *, CSFIdinfo *);
CS_STATUS femu_queryDeviceProperties(void *, CS_RESOURCE_TYPE, int *, CsProperties *);
CS_STATUS femu_CSFDownload(void *, const CsCSFDownloadInfo *, u32 *);
CS_STATUS femu_config(void *, int *, const CsConfigInfo *, CsConfigData *);
CS_STATUS femu_allocMem(void *, int, const CsMemFlags *, void **, u64 *, CS_MEM_PTR *);
CS_STATUS femu_freeMem(void *, void *, const CsMemFlags *);
CS_STATUS femu_initMem(void *, void *, unsigned long, int, const CsMemFlags *);
/* Local output-size helper; no dependency on the public library internals. */
static inline CS_STATUS cs_output(int *length, void *buffer, size_t needed)
{
    if (!length || needed > INT32_MAX) return CS_INVALID_ARG;
    int have = *length; *length = (int)needed;
    return !buffer || have < 0 || (size_t)have < needed ? CS_INVALID_LENGTH : CS_SUCCESS;
}
#endif
