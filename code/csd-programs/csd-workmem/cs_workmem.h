#ifndef CSD_WORKMEM_H
#define CSD_WORKMEM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Device-side, cooperative allocator. No Guest/NVMe pointers or FDM allocations. */
typedef struct cs_work_context cs_work_context;
/* Optional clean storage. Keep the zero-initialized descriptor alive until its
 * context ends. A pin covers only the caller's immediate access. */
typedef struct cs_work_cache { void *data; unsigned pins; } cs_work_cache;
typedef struct cs_work_stats {
    size_t capacity_bytes, used_bytes, requested_bytes, peak_used_bytes;
    size_t free_bytes, largest_free_payload, active_contexts, active_allocations;
} cs_work_stats;
typedef struct cs_work_context_stats {
    size_t limit_bytes, used_bytes, requested_bytes, peak_used_bytes, allocations;
} cs_work_context_stats;
/* Initialize once. 0 selects CSD_WORKMEM_SIZE_MB (default 64 MiB).
 * A different capacity after initialization fails with EALREADY. */
int cs_work_init(size_t capacity_bytes);
/* The device owns this preallocated arena and keeps it mapped until detach. */
int cs_work_attach(void *arena, size_t capacity_bytes);
void cs_work_detach(void);
cs_work_context *cs_work_begin(void);
/* 0 means the whole shared pool; a limit never reserves memory exclusively. */
cs_work_context *cs_work_begin_limit(size_t limit_bytes);
/* A child charges both its own limit and the parent's query-wide limit.
 * End children before their parent; ending a parent with children is EBUSY. */
cs_work_context *cs_work_begin_child(cs_work_context *parent,size_t limit_bytes);
void *cs_work_alloc(cs_work_context *ctx, size_t bytes);
void *cs_work_calloc(cs_work_context *ctx, size_t count, size_t bytes);
void *cs_work_realloc(cs_work_context *ctx, void *ptr, size_t bytes);
int cs_work_cache_allocate(cs_work_context *ctx, cs_work_cache *cache, size_t bytes,
                           size_t pool_limit);
void *cs_work_cache_pin(cs_work_cache *cache);
void cs_work_cache_unpin(cs_work_cache *cache);
/* Budget headroom including evictable pages. NULL queries the shared pool.
 * The allocator still checks physical contiguity and pins on allocation. */
size_t cs_work_available(cs_work_context *ctx);
/* Invalid/foreign pointers return -1 with errno; NULL is accepted for a valid ctx. */
int cs_work_free(cs_work_context *ctx, void *ptr);
/* Bulk release all outstanding allocations. The handle becomes invalid. */
int cs_work_end(cs_work_context *ctx);
int cs_work_get_stats(cs_work_stats *out);
int cs_work_get_context_stats(cs_work_context *ctx, cs_work_context_stats *out);
#ifdef __cplusplus
}
#endif
#endif
