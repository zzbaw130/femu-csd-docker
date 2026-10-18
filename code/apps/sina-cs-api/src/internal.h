#ifndef CS_INTERNAL_H
#define CS_INTERNAL_H
#include "cs.h"
#include "cs_kernel.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <linux/nvme_ioctl.h>
#define CS_MAX_BATCH 64
#define CS_TRANSFER (1U << 20)
enum cs_kind { DEV = 1, MEM, FUNCTION, EVENT, BATCH, REQUEST };
struct cs_object {
    uintptr_t id;
    unsigned kind, refs;
    void (*destroy)(struct cs_object *);
    struct cs_object *next;
};
struct cs_device {
    struct cs_object base;
    int fd, admin, nsid;
    uint32_t lba;
    char path[4096];
    struct cs_profile {
        uint32_t workers;
        uint64_t fdm_bytes;
    } caps;
    void *context;
};
struct cs_memory {
    struct cs_object base;
    struct cs_device *dev;
    uint32_t id;
    uint64_t bytes;
};
struct cs_function {
    struct cs_object base;
    struct cs_device *dev;
    uint32_t index, type;
};
struct cs_completion {
    void *context;
    u64 value;
    CS_STATUS status;
    struct cs_completion *next;
};
struct cs_event {
    struct cs_object base;
    unsigned pending;
    struct cs_completion *head, *tail;
};
struct cs_task {
    CsBatchRequest *req;
    struct cs_object *pins[CS_API_MAX_ARGS + 4];
    unsigned npins;
    struct cs_device *dev;
};
struct cs_entry {
    struct cs_task *task;
    uint64_t dependencies;
};
struct cs_batch {
    struct cs_object base;
    CS_BATCH_MODE mode;
    unsigned capacity;
    struct cs_entry entries[CS_MAX_BATCH];
};
struct cs_request {
    struct cs_object base;
    struct cs_task *task;
    struct cs_batch *batch;
    struct cs_event *event;
    struct cs_completion *completion;
    void *context;
    csQueueCallbackFn callback;
    atomic_bool cancelled, started;
};
extern pthread_mutex_t cs_lock;
struct cs_object *cs_new(size_t bytes, unsigned kind, void (*destroy)(struct cs_object *));
struct cs_object *cs_get(void *handle, unsigned kind);
void cs_hold(struct cs_object *o);
void cs_put(struct cs_object *o);
CS_STATUS cs_retire(struct cs_object *o);
void cs_unlink(struct cs_object *o);
void cs_restore(struct cs_object *o);
CS_STATUS cs_output(int *length, void *buffer, size_t needed);
int cs_transport_open(struct cs_device *d, const char *name);
void cs_transport_close(struct cs_device *d);
CS_STATUS cs_io(struct cs_device *d, bool admin, struct nvme_passthru_cmd *cmd);
int cs_profile_load(struct cs_device *, const char *);
/* Host catalog: only programs loaded through this library in this process. */
struct cs_program_info {
    uint32_t id, type, active;
    uint64_t global_id;
    char name[32];
};
struct cs_catalog_entry {
    char path[4096];
    struct cs_program_info info;
    struct cs_catalog_entry *next;
};
extern pthread_mutex_t cs_catalog_lock;
struct cs_catalog_entry *cs_catalog_find_locked(struct cs_device *, unsigned);
struct cs_catalog_entry *cs_catalog_reserve_locked(struct cs_device *, unsigned);
CS_STATUS cs_collect(struct cs_device *, struct cs_program_info **, size_t *);
CS_STATUS cs_program(struct cs_device *d, unsigned start, struct cs_program_info *p);
CS_STATUS cs_task_clone(const CsBatchRequest *req, struct cs_task **out);
void cs_task_free(struct cs_task *task);
CS_STATUS cs_task_run(struct cs_task *task, u64 *value);
CS_STATUS cs_batch_run(struct cs_batch *b, atomic_bool *cancelled, u64 *value);
CS_STATUS cs_submit(struct cs_task *task, struct cs_batch *batch, void *context,
                    csQueueCallbackFn callback, CS_EVT_HANDLE event, CS_REQ_HANDLE *request,
                    u64 *value);
struct cs_object *cs_task_object(struct cs_task *t, void *handle, unsigned kind);
CS_STATUS cs_fill(struct cs_memory *m, uint64_t offset, uint64_t bytes, const CsMemFlags *f);
bool cs_function_busy(struct cs_device *, unsigned);
#endif
