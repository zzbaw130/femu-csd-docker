#define _GNU_SOURCE
#include "cs_workmem.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define CONTEXTS 256
#define ALIGN _Alignof(max_align_t)
typedef union block block;
union block {
    struct {
        size_t span, requested;
        block *prev, *next;
        uintptr_t owner;
        cs_work_cache *cache;
    } h;
    max_align_t align;
};
struct context_record {
    uintptr_t token;
    uintptr_t parent;
    size_t limit, used, requested, peak, allocations, clean;
};
/* Exactly one instance in the shared library's data segment. All CU threads use it. */
static struct {
    pthread_mutex_t mutex;
    void *arena;
    bool borrowed;
    size_t capacity, used, requested, peak, allocations, contexts, clean;
    uintptr_t next_token;
    struct context_record records[CONTEXTS];
} pool = {.mutex = PTHREAD_MUTEX_INITIALIZER, .next_token = 1};
static int fail(int error)
{
    errno = error;
    return -1;
}
static int initialize_locked(size_t capacity)
{
    if (pool.arena) {
        if (!capacity || capacity == pool.capacity)
            return 0;
        return fail(EALREADY);
    }
    if (!capacity) {
        const char *value = getenv("CSD_WORKMEM_SIZE_MB");
        size_t mib = 64;
        if (value) {
            char *end;
            errno = 0;
            unsigned long long n = strtoull(value, &end, 10);
            if (errno || *value < '0' || *value > '9' || *end || !n ||
                n > SIZE_MAX / (1024U * 1024U))
                return fail(EINVAL);
            mib = (size_t)n;
        }
        capacity = mib * (1024U * 1024U);
    }
    if (capacity < sizeof(block) + ALIGN || capacity % ALIGN)
        return fail(EINVAL);
    void *arena = mmap(NULL, capacity, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED)
        return -1;
    pool.arena = arena;
    pool.capacity = capacity;
    block *b = arena;
    *b = (block){.h.span = capacity};
    return 0;
}
int cs_work_attach(void *arena, size_t capacity)
{
    if (!arena || (uintptr_t)arena % ALIGN || capacity < sizeof(block) + ALIGN ||
        capacity % ALIGN)
        return fail(EINVAL);
    pthread_mutex_lock(&pool.mutex);
    if (pool.arena) {
        pthread_mutex_unlock(&pool.mutex);
        return fail(EALREADY);
    }
    pool.arena = arena;
    pool.capacity = capacity;
    pool.borrowed = true;
    *(block *)arena = (block){.h.span = capacity};
    pthread_mutex_unlock(&pool.mutex);
    return 0;
}
void cs_work_detach(void)
{
    pthread_mutex_lock(&pool.mutex);
    if (pool.borrowed) {
        pool.arena = NULL;
        pool.borrowed = false;
        pool.capacity = pool.used = pool.requested = pool.peak = 0;
        pool.allocations = pool.contexts = pool.clean = 0;
        memset(pool.records, 0, sizeof(pool.records));
    }
    pthread_mutex_unlock(&pool.mutex);
}
static struct context_record *context_locked(cs_work_context *handle)
{
    uintptr_t token = (uintptr_t)handle;
    if (token)
        for (size_t i = 0; i < CONTEXTS; i++)
            if (pool.records[i].token == token)
                return &pool.records[i];
    errno = EINVAL;
    return NULL;
}
static int may_charge(struct context_record *ctx,size_t bytes)
{
    for(;;) {
        if(bytes>ctx->limit-ctx->used) return 0;
        if(!ctx->parent) return 1;
        ctx=context_locked((void *)ctx->parent);
        if(!ctx) return 0;
    }
}
static void account(struct context_record *ctx,size_t old_used,size_t new_used,
    size_t old_requested,size_t new_requested,size_t old_allocations,size_t new_allocations)
{
    for(;;) {
        ctx->used=ctx->used-old_used+new_used;
        ctx->requested=ctx->requested-old_requested+new_requested;
        ctx->allocations=ctx->allocations-old_allocations+new_allocations;
        if(ctx->used>ctx->peak) ctx->peak=ctx->used;
        if(!ctx->parent) break;
        ctx=context_locked((void *)ctx->parent);
    }
}
static block *find_locked(struct context_record *ctx, void *ptr)
{
    for (block *b = pool.arena; b; b = b->h.next)
        if ((void *)(b + 1) == ptr) {
            if (!b->h.owner || b->h.owner != ctx->token) {
                errno = EINVAL;
                return NULL;
            }
            return b;
        }
    errno = EINVAL;
    return NULL;
}
static void split_locked(block *b, size_t span)
{
    size_t remainder = b->h.span - span;
    if (remainder < sizeof(block) + ALIGN)
        return;
    block *tail = (void *)((unsigned char *)b + span);
    *tail = (block){.h = {.span = remainder, .prev = b, .next = b->h.next}};
    if (tail->h.next)
        tail->h.next->h.prev = tail;
    b->h.next = tail;
    b->h.span = span;
}
static void merge_next_locked(block *b)
{
    block *next = b->h.next;
    if (next && !next->h.owner) {
        b->h.span += next->h.span;
        b->h.next = next->h.next;
        if (b->h.next)
            b->h.next->h.prev = b;
    }
}
static void account_clean(struct context_record *ctx, size_t added, size_t removed)
{
    pool.clean = pool.clean + added - removed;
    while (ctx) {
        ctx->clean = ctx->clean + added - removed;
        ctx = ctx->parent ? context_locked((void *)ctx->parent) : NULL;
    }
}
static void release_locked(struct context_record *ctx, block *b)
{
    if (b->h.cache) {
        account_clean(ctx, 0, b->h.span);
        __atomic_store_n(&b->h.cache->data, NULL, __ATOMIC_RELEASE);
        b->h.cache = NULL;
    }
    pool.used -= b->h.span;
    pool.requested -= b->h.requested;
    pool.allocations--;
    account(ctx,b->h.span,0,b->h.requested,0,1,0);
    b->h.owner = b->h.requested = 0;
    merge_next_locked(b);
    if (b->h.prev && !b->h.prev->h.owner)
        merge_next_locked(b->h.prev);
}
static bool reclaim_clean_locked(void)
{
    if (!pool.clean)
        return false;
    for (block *b = pool.arena; b; b = b->h.next) {
        cs_work_cache *cache = b->h.cache;
        unsigned idle = 0;
        if (cache && __atomic_compare_exchange_n(&cache->pins, &idle, UINT_MAX,
                                                 false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            release_locked(context_locked((void *)b->h.owner), b);
            __atomic_store_n(&cache->pins, 0, __ATOMIC_RELEASE);
            return true;
        }
    }
    return false;
}
static bool can_reclaim(struct context_record *ctx, size_t bytes)
{
    if (!pool.clean || bytes > pool.capacity - pool.used + pool.clean)
        return false;
    while (ctx) {
        if (bytes > ctx->limit - ctx->used + ctx->clean)
            return false;
        ctx = ctx->parent ? context_locked((void *)ctx->parent) : NULL;
    }
    return true;
}
static void *allocate_locked(struct context_record *ctx, size_t bytes)
{
    if (!bytes) {
        errno = EINVAL;
        return NULL;
    }
    if (bytes > SIZE_MAX - sizeof(block) - (ALIGN - 1)) {
        errno = ENOMEM;
        return NULL;
    }
    size_t need = sizeof(block) + (bytes + ALIGN - 1) / ALIGN * ALIGN;
    for (block *b = pool.arena; b; b = b->h.next) {
        if (b->h.owner || b->h.span < need)
            continue;
        size_t charge = b->h.span - need >= sizeof(block) + ALIGN ? need : b->h.span;
        if (!may_charge(ctx,charge))
            continue;
        split_locked(b, need);
        b->h.owner = ctx->token;
        b->h.requested = bytes;
        pool.used += b->h.span;
        pool.requested += bytes;
        pool.allocations++;
        account(ctx,0,b->h.span,0,bytes,0,1);
        if (pool.used > pool.peak)
            pool.peak = pool.used;
        if (ctx->used > ctx->peak)
            ctx->peak = ctx->used;
        return b + 1;
    }
    errno = ENOMEM;
    return NULL;
}
int cs_work_init(size_t bytes)
{
    pthread_mutex_lock(&pool.mutex);
    int result = initialize_locked(bytes);
    pthread_mutex_unlock(&pool.mutex);
    return result;
}
static cs_work_context *begin_context(cs_work_context *parent,size_t limit)
{
    pthread_mutex_lock(&pool.mutex);
    cs_work_context *handle = NULL;
    if (initialize_locked(0))
        goto out;
    struct context_record *owner=parent?context_locked(parent):NULL;
    if(parent && !owner) goto out;
    if(owner && (!limit || limit>owner->limit)) limit=owner->limit;
    if (limit > pool.capacity || pool.next_token == UINTPTR_MAX) {
        errno = limit > pool.capacity ? EINVAL : EOVERFLOW;
        goto out;
    }
    errno = ENOSPC;
    for (size_t i = 0; i < CONTEXTS; i++)
        if (!pool.records[i].token) {
            struct context_record *ctx = &pool.records[i];
            *ctx = (struct context_record){.token = pool.next_token++,
                                          .limit = limit ? limit : pool.capacity,
                                          .parent=owner?owner->token:0};
            pool.contexts++;
            handle = (void *)ctx->token;
            break;
        }
out:
    pthread_mutex_unlock(&pool.mutex);
    return handle;
}
cs_work_context *cs_work_begin_limit(size_t limit)
{ return begin_context(NULL,limit); }
cs_work_context *cs_work_begin_child(cs_work_context *parent,size_t limit)
{ if(!parent) { errno=EINVAL; return NULL; } return begin_context(parent,limit); }
cs_work_context *cs_work_begin(void)
{
    return cs_work_begin_limit(0);
}
void *cs_work_alloc(cs_work_context *handle, size_t bytes)
{
    pthread_mutex_lock(&pool.mutex);
    struct context_record *ctx = context_locked(handle);
    void *p = ctx ? allocate_locked(ctx, bytes) : NULL;
    while (!p && ctx && errno == ENOMEM && can_reclaim(ctx, bytes) && reclaim_clean_locked())
        p = allocate_locked(ctx, bytes);
    pthread_mutex_unlock(&pool.mutex);
    return p;
}
int cs_work_cache_allocate(cs_work_context *handle, cs_work_cache *cache, size_t bytes,
                           size_t pool_limit)
{
    pthread_mutex_lock(&pool.mutex);
    struct context_record *ctx = context_locked(handle);
    size_t available = pool_limit > pool.clean ? pool_limit - pool.clean : 0;
    void *p = ctx && available >= sizeof(block) + ALIGN &&
        bytes <= (available - sizeof(block)) / ALIGN * ALIGN &&
        !__atomic_load_n(&cache->data, __ATOMIC_RELAXED) ?
        allocate_locked(ctx, bytes) : NULL;
    if (p) {
        block *b = (block *)p - 1;
        if (b->h.span > available) {
            release_locked(ctx, b);
            p = NULL;
        } else {
            b->h.cache = cache;
            account_clean(ctx, b->h.span, 0);
            __atomic_store_n(&cache->data, p, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&pool.mutex);
    return p ? 0 : -1;
}
void *cs_work_cache_pin(cs_work_cache *cache)
{
    if (!__atomic_load_n(&cache->data, __ATOMIC_ACQUIRE))
        return NULL;
    unsigned pins = __atomic_load_n(&cache->pins, __ATOMIC_RELAXED);
    while (pins < UINT_MAX - 1) {
        if (__atomic_compare_exchange_n(&cache->pins, &pins, pins + 1, true,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            void *data = __atomic_load_n(&cache->data, __ATOMIC_ACQUIRE);
            if (!data)
                cs_work_cache_unpin(cache);
            return data;
        }
    }
    return NULL;
}
void cs_work_cache_unpin(cs_work_cache *cache)
{
    __atomic_sub_fetch(&cache->pins, 1, __ATOMIC_RELEASE);
}
void *cs_work_calloc(cs_work_context *ctx, size_t count, size_t bytes)
{
    if (bytes && count > SIZE_MAX / bytes) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = cs_work_alloc(ctx, count * bytes);
    if (p)
        memset(p, 0, count * bytes);
    return p;
}
int cs_work_free(cs_work_context *handle, void *ptr)
{
    pthread_mutex_lock(&pool.mutex);
    struct context_record *ctx = context_locked(handle);
    block *b = ctx && ptr ? find_locked(ctx, ptr) : NULL;
    if (b && b->h.cache) {
        errno = EINVAL;
        b = NULL;
    }
    int result = ctx && (!ptr || b) ? 0 : -1;
    if (b)
        release_locked(ctx, b);
    pthread_mutex_unlock(&pool.mutex);
    return result;
}
void *cs_work_realloc(cs_work_context *handle, void *ptr, size_t bytes)
{
    if (!ptr)
        return cs_work_alloc(handle, bytes);
    if (!bytes) {
        cs_work_free(handle, ptr);
        return NULL;
    }
    pthread_mutex_lock(&pool.mutex);
    struct context_record *ctx = context_locked(handle);
    block *b = ctx ? find_locked(ctx, ptr) : NULL;
    void *p = NULL;
    if (!b)
        goto out;
    if (b->h.cache) {
        errno = EINVAL;
        goto out;
    }
    if (bytes > SIZE_MAX - sizeof(block) - (ALIGN - 1)) {
        errno = ENOMEM;
        goto out;
    }
retry:;
    size_t old = b->h.requested;
    size_t need = sizeof(block) + (bytes + ALIGN - 1) / ALIGN * ALIGN;
    /* An adjacent free tail can grow the block without a second full copy.
     * Charge only the added span, including every parent context's limit. */
    if (need > b->h.span && b->h.next && !b->h.next->h.owner) {
        size_t combined = b->h.span + b->h.next->h.span;
        size_t charge = combined >= need && combined - need >= sizeof(block) + ALIGN ? need : combined;
        if (combined >= need && may_charge(ctx, charge - b->h.span)) {
            size_t original = b->h.span;
            merge_next_locked(b);
            split_locked(b, need);
            pool.used += b->h.span - original;
            pool.requested += bytes - old;
            account(ctx,original,b->h.span,old,bytes,0,0);
            if (pool.used > pool.peak)
                pool.peak = pool.used;
            b->h.requested = bytes;
            p = ptr;
            goto out;
        }
    }
    if (bytes <= b->h.span - sizeof(block)) {
        /* Shrink in place and return a usable tail to the shared pool. */
        size_t original = b->h.span;
        split_locked(b, need);
        if (b->h.span != original)
            merge_next_locked(b->h.next);
        pool.used -= original - b->h.span;
        account(ctx,original,b->h.span,old,bytes,0,0);
        if (bytes >= old) {
            pool.requested += bytes - old;
        } else {
            pool.requested -= old - bytes;
        }
        b->h.requested = bytes;
        p = ptr;
    } else {
        /* Conservative growth: old and new blocks both count until the copy finishes. */
        p = allocate_locked(ctx, bytes);
        if (!p && errno == ENOMEM && can_reclaim(ctx, need - b->h.span) &&
            reclaim_clean_locked()) {
            goto retry;
        }
        if (p) {
            memcpy(p, ptr, old);
            release_locked(ctx, b);
        }
    }
out:
    pthread_mutex_unlock(&pool.mutex);
    return p;
}
int cs_work_end(cs_work_context *handle)
{
    pthread_mutex_lock(&pool.mutex);
    struct context_record *ctx = context_locked(handle);
    int result = ctx ? 0 : -1;
    if(ctx) for(size_t i=0;i<CONTEXTS;++i) if(pool.records[i].parent==ctx->token) {
        pthread_mutex_unlock(&pool.mutex);
        return fail(EBUSY);
    }
    if (ctx) {
        /* Cache descriptors can live in a different allocation of this
         * context. Invalidate them before releasing any of those blocks. */
        for (block *b = pool.arena; b; b = b->h.next) {
            if (b->h.owner == ctx->token && b->h.cache &&
                __atomic_load_n(&b->h.cache->pins, __ATOMIC_ACQUIRE)) {
                pthread_mutex_unlock(&pool.mutex);
                return fail(EBUSY);
            }
        }
        for (block *b = pool.arena; b; b = b->h.next) {
            if (b->h.owner == ctx->token && b->h.cache) {
                __atomic_store_n(&b->h.cache->data, NULL, __ATOMIC_RELEASE);
                b->h.cache = NULL;
                account_clean(ctx, 0, b->h.span);
            }
        }
        /* release coalesces neighbors, so restart traversal after each owned block. */
        for (block *b = pool.arena; b;) {
            if (b->h.owner == ctx->token) {
                release_locked(ctx, b);
                b = pool.arena;
            } else
                b = b->h.next;
        }
        memset(ctx, 0, sizeof(*ctx));
        pool.contexts--;
    }
    pthread_mutex_unlock(&pool.mutex);
    return result;
}
int cs_work_get_stats(cs_work_stats *out)
{
    if (!out)
        return fail(EINVAL);
    pthread_mutex_lock(&pool.mutex);
    int result = initialize_locked(0);
    if (!result) {
        *out = (cs_work_stats){.capacity_bytes = pool.capacity, .used_bytes = pool.used,
            .requested_bytes = pool.requested, .peak_used_bytes = pool.peak,
            .free_bytes = pool.capacity - pool.used, .active_contexts = pool.contexts,
            .active_allocations = pool.allocations};
        for (block *b = pool.arena; b; b = b->h.next)
            if (!b->h.owner && b->h.span - sizeof(block) > out->largest_free_payload)
                out->largest_free_payload = b->h.span - sizeof(block);
    }
    pthread_mutex_unlock(&pool.mutex);
    return result;
}
int cs_work_get_context_stats(cs_work_context *handle, cs_work_context_stats *out)
{
    if (!out)
        return fail(EINVAL);
    pthread_mutex_lock(&pool.mutex);
    struct context_record *ctx = context_locked(handle);
    if (ctx)
        *out = (cs_work_context_stats){.limit_bytes = ctx->limit, .used_bytes = ctx->used,
            .requested_bytes = ctx->requested, .peak_used_bytes = ctx->peak,
            .allocations = ctx->allocations};
    pthread_mutex_unlock(&pool.mutex);
    return ctx ? 0 : -1;
}
size_t cs_work_available(cs_work_context *handle)
{
    pthread_mutex_lock(&pool.mutex);
    struct context_record *ctx = handle ? context_locked(handle) : NULL;
    size_t bytes = ctx ? ctx->limit - ctx->used + ctx->clean :
        handle ? 0 : pool.capacity - pool.used + pool.clean;
    pthread_mutex_unlock(&pool.mutex);
    return bytes;
}
__attribute__((destructor)) static void workmem_unload(void)
{
    /* The final dependent .so must release all contexts before unloading. */
    if (pool.arena && !pool.borrowed)
        munmap(pool.arena, pool.capacity);
}
