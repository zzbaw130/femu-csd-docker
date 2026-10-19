#ifndef CS_INTERNAL_H
#define CS_INTERNAL_H
#include "cs.h"
#include "cs_backend.h"
#include <pthread.h>
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
/* Library limits shared by the categorized APIs and request executor. */
#define CS_MAX_BATCH 64
#define CS_API_MAX_ARGS 64
/* Loaded adapter module; device objects retain it until their last use. */
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
/* Public opaque handles and their reference-counted objects. */
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
/* Event completion records and prepared requests shared by queue* APIs. */
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
struct cs_prepared_request {
    CsBatchRequest *request; /* Snapshot with bound plugin-private handles. */
    struct cs_object *pins[CS_API_MAX_ARGS + 4];
    unsigned npins;
    struct cs_device *dev;
};
struct cs_entry {
    struct cs_prepared_request *task;
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
    struct cs_prepared_request *task;
    struct cs_batch *batch;
    struct cs_event *event;
    struct cs_completion *completion;
    void *context;
    csQueueCallbackFn callback;
    atomic_bool cancelled, started;
};
/* Shared registry, protected by cs_handle_lock; csCloseCSx retires its CSF handles. */
extern struct cs_object *cs_objects;
extern pthread_mutex_t cs_handle_lock;
/* Serializes request binding against CSF configuration/download. */
extern pthread_mutex_t cs_csf_binding_lock;
struct cs_object *cs_handle_create(size_t, unsigned, void (*)(struct cs_object *));
struct cs_object *cs_handle_acquire(void *, unsigned);
void cs_handle_retain(struct cs_object *);
void cs_handle_release(struct cs_object *);
CS_STATUS cs_handle_retire(struct cs_object *);
void cs_handle_remove(struct cs_object *);
void cs_handle_restore(struct cs_object *);
static inline CS_STATUS cs_output(int *length, void *buffer, size_t needed)
{
    if (!length || needed > INT_MAX)
        return CS_INVALID_ARG;
    int have = *length;
    *length = (int)needed;
    return !buffer || have < 0 || (size_t)have < needed ? CS_INVALID_LENGTH : CS_SUCCESS;
}
bool cs_csf_in_use(struct cs_device *, unsigned);
CS_STATUS cs_request_prepare(const CsBatchRequest *, struct cs_prepared_request **);
void cs_request_release(struct cs_prepared_request *);
CS_STATUS cs_request_execute(struct cs_prepared_request *, u64 *);
CS_STATUS cs_request_execute_batch_entry(struct cs_prepared_request *, atomic_bool *, void *, u64 *);
CS_STATUS cs_request_queue(const CsBatchRequest *, void *, csQueueCallbackFn, CS_EVT_HANDLE, CS_REQ_HANDLE *, u64 *);
CS_STATUS cs_request_submit(struct cs_prepared_request *, struct cs_batch *, void *, csQueueCallbackFn, CS_EVT_HANDLE, CS_REQ_HANDLE *, u64 *);
CS_STATUS cs_batch_run(struct cs_batch *, atomic_bool *, u64 *);
#endif
