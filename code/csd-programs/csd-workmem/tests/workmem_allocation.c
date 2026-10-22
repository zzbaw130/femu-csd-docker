#include "cs_workmem.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define CAPACITY (128U << 10)
static void *stress(void *unused)
{
    (void)unused;
    cs_work_context *ctx = cs_work_begin();
    assert(ctx);
    for (unsigned i = 0; i < 3000; i++) {
        size_t n = 1 + (i * 71 % 2048);
        unsigned char *p = cs_work_alloc(ctx, n);
        assert(p && (uintptr_t)p % alignof(max_align_t) == 0);
        memset(p, i % 251, n);
        for (size_t j = 0; j < n; j++)
            assert(p[j] == i % 251);
        assert(!cs_work_free(ctx, p));
    }
    assert(!cs_work_end(ctx));
    return NULL;
}
int main(void)
{
    assert(cs_work_init(1) == -1 && errno == EINVAL);
    assert(!cs_work_init(CAPACITY));
    assert(!cs_work_init(CAPACITY));
    assert(cs_work_init(CAPACITY * 2) == -1 && errno == EALREADY);
    cs_work_stats stats;
    assert(!cs_work_get_stats(&stats) && stats.capacity_bytes == CAPACITY);
    size_t full = stats.largest_free_payload;
    cs_work_context *a = cs_work_begin(), *b = cs_work_begin();
    assert(a && b);
    assert(!cs_work_alloc(a, 0) && errno == EINVAL);
    assert(!cs_work_calloc(a, SIZE_MAX, 2) && errno == ENOMEM);
    assert(!cs_work_alloc(a, SIZE_MAX) && errno == ENOMEM);
    void *all = cs_work_alloc(a, full);
    assert(all);
    assert(!cs_work_alloc(b, 1) && errno == ENOMEM);
    assert(cs_work_free(b, all) == -1 && errno == EINVAL);
    assert(!cs_work_free(a, all));
    assert(cs_work_free(a, all) == -1);
    void *x = cs_work_alloc(a, 1024), *y = cs_work_alloc(b, 2048), *z = cs_work_alloc(a, 4096);
    assert(x && y && z);
    assert(!cs_work_free(b, y));
    assert(!cs_work_end(a));
    assert(!cs_work_get_stats(&stats) && !stats.used_bytes && stats.largest_free_payload == full);
    assert(!cs_work_alloc(a, 1) && errno == EINVAL); /* stale context */
    a = cs_work_begin_limit(4096);
    assert(a && !cs_work_alloc(a, 4096)); /* Block metadata counts against the context. */
    unsigned char *p = cs_work_calloc(a, 64, 1);
    assert(p);
    for (unsigned i = 0; i < 64; i++)
        assert(p[i] == 0);
    memset(p, 0x67, 64);
    unsigned char *q = cs_work_realloc(a, p, 1024);
    assert(q);
    for (unsigned i = 0; i < 64; i++)
        assert(q[i] == 0x67);
    assert(!cs_work_realloc(a, q, CAPACITY) && errno == ENOMEM);
    assert(q[0] == 0x67); /* Failed realloc preserves the old allocation. */
    assert(cs_work_realloc(a, q, 32) == q);
    cs_work_context_stats local;
    assert(!cs_work_get_context_stats(a, &local) && local.requested_bytes == 32);
    assert(!cs_work_realloc(a, q, 0));
    assert(!cs_work_end(a));
    assert(!cs_work_end(b));
    a=cs_work_begin_limit(16*1024);
    cs_work_context *c1=cs_work_begin_child(a,12*1024),*c2=cs_work_begin_child(a,12*1024);
    assert(a && c1 && c2);
    void *p1=cs_work_alloc(c1,8*1024),*p2=cs_work_alloc(c2,6*1024);
    assert(p1 && p2 && !cs_work_alloc(c2,4*1024));
    assert(!cs_work_get_context_stats(a,&local) && local.requested_bytes==14*1024 && local.allocations==2);
    assert(cs_work_end(a)==-1 && errno==EBUSY);
    assert(cs_work_realloc(c1,p1,1024)==p1);
    assert(!cs_work_get_context_stats(a,&local) && local.requested_bytes==7*1024);
    assert(!cs_work_alloc(c1,12*1024));
    assert(!cs_work_end(c1) && !cs_work_end(c2) && !cs_work_end(a));
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; i++)
        assert(!pthread_create(&threads[i], NULL, stress, NULL));
    for (unsigned i = 0; i < 8; i++)
        assert(!pthread_join(threads[i], NULL));
    cs_work_context *contexts[256];
    for (unsigned i = 0; i < 256; i++) {
        contexts[i] = cs_work_begin();
        assert(contexts[i]);
    }
    assert(!cs_work_begin() && errno == ENOSPC);
    for (unsigned i = 0; i < 256; i++)
        assert(!cs_work_end(contexts[i]));
    assert(!cs_work_get_stats(&stats) && !stats.used_bytes && !stats.active_contexts &&
           !stats.active_allocations && stats.largest_free_payload == full &&
           stats.peak_used_bytes <= CAPACITY);
    puts("PASS: bounded arena, alignment, limits, coalescing, context cleanup, realloc, 8-thread stress");
}
