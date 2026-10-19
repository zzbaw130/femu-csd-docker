#ifndef CS_INTERNAL_H
#define CS_INTERNAL_H
#include "cs.h"
#include "cs_backend.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define CS_MAX_BATCH 64
#define CS_API_MAX_ARGS 64
struct cs_plugin {
    void *library;
    const struct cs_backend_ops *ops;
    cs_backend_destroy_fn destroy;
    char *path;
    unsigned refs;
    struct cs_plugin *next;
};
CS_STATUS cs_plugins_snapshot(struct cs_plugin ***, size_t *);
void cs_plugin_put(struct cs_plugin *);
CS_STATUS cs_plugin_register(const CsPluginRequest *);
CS_STATUS cs_plugin_deregister(const CsPluginRequest *);
enum cs_kind { DEV = 1, MEM, FUNCTION, EVENT, BATCH, REQUEST };
struct cs_object {
    uintptr_t id;
    unsigned kind, refs;
    void (*destroy)(struct cs_object *);
    struct cs_object *next;
};
struct cs_device {
    struct cs_object base;
    struct cs_plugin *plugin;
    void *private_data;
    unsigned workers;
    char path[4096];
    void *context;
};
struct cs_memory {
    struct cs_object base;
    struct cs_device *dev;
    void *private_data;
    u64 bytes;
};
struct cs_function {
    struct cs_object base;
    struct cs_device *dev;
    void *private_data;
    u32 id;
};
struct cs_completion {
    void *context;
    u64 value;
    CS_STATUS status;
    struct cs_completion *next;
};
struct cs_event { struct cs_object base; unsigned pending; struct cs_completion *head, *tail; };
struct cs_task {
    CsBatchRequest *req, *backend_req;
    struct cs_object *pins[CS_API_MAX_ARGS + 4];
    unsigned npins;
    struct cs_device *dev;
};
struct cs_entry { struct cs_task *task; uint64_t dependencies; };
struct cs_batch { struct cs_object base; CS_BATCH_MODE mode; unsigned capacity; struct cs_entry entries[CS_MAX_BATCH]; };
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
/* Serializes request binding against CSF configuration/download. */
extern pthread_mutex_t cs_binding_lock;
struct cs_object *cs_new(size_t, unsigned, void (*)(struct cs_object *));
struct cs_object *cs_get(void *, unsigned);
void cs_hold(struct cs_object *);
void cs_put(struct cs_object *);
CS_STATUS cs_retire(struct cs_object *);
void cs_unlink(struct cs_object *);
void cs_restore(struct cs_object *);
CS_STATUS cs_output(int *, void *, size_t);
CS_STATUS cs_device_close(CS_DEV_HANDLE);
bool cs_function_busy(struct cs_device *, unsigned);
CS_STATUS cs_task_clone(const CsBatchRequest *, struct cs_task **);
void cs_task_free(struct cs_task *);
CS_STATUS cs_task_run(struct cs_task *, u64 *);
CS_STATUS cs_task_run_batch(struct cs_task *, atomic_bool *, void *, u64 *);
CS_STATUS cs_queue_one(const CsBatchRequest *, void *, csQueueCallbackFn, CS_EVT_HANDLE, CS_REQ_HANDLE *, u64 *);
CS_STATUS cs_submit(struct cs_task *, struct cs_batch *, void *, csQueueCallbackFn, CS_EVT_HANDLE, CS_REQ_HANDLE *, u64 *);
CS_STATUS cs_batch_alloc(CS_BATCH_MODE, int, CS_BATCH_HANDLE *);
CS_STATUS cs_batch_free(CS_BATCH_HANDLE);
CS_STATUS cs_batch_configure(CS_BATCH_HANDLE, CS_BATCH_CONFIG_TYPE, const CsBatchRequest *, CS_BATCH_INDEX, CS_BATCH_INDEX, CS_BATCH_INDEX *);
CS_STATUS cs_batch_resize(CS_BATCH_HANDLE, int);
CS_STATUS cs_batch_queue(CS_BATCH_HANDLE, void *, csQueueCallbackFn, CS_EVT_HANDLE, CS_REQ_HANDLE *, u64 *);
CS_STATUS cs_batch_run(struct cs_batch *, atomic_bool *, u64 *);
#endif
