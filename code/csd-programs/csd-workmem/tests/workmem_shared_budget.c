#include "cs_workmem.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
struct api {
    cs_work_context *(*begin)(void);
    void *(*alloc)(cs_work_context *, size_t);
    int (*end)(cs_work_context *);
    int (*stats)(cs_work_stats *);
};
int main(int argc, char **argv)
{
    assert(argc == 3 && !setenv("CSD_WORKMEM_SIZE_MB", "16", 1));
    void *a = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL), *b = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (!a || !b) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    struct api apis[2];
    void *handles[] = {a, b};
    for (unsigned i = 0; i < 2; i++) {
        apis[i].begin = dlsym(handles[i], "workmem_test_begin");
        apis[i].alloc = dlsym(handles[i], "workmem_test_alloc");
        apis[i].end = dlsym(handles[i], "workmem_test_end");
        apis[i].stats = dlsym(handles[i], "workmem_test_stats");
        assert(apis[i].begin && apis[i].alloc && apis[i].end && apis[i].stats);
    }
    cs_work_context *x = apis[0].begin(), *y = apis[1].begin(), *z = apis[1].begin();
    assert(x && y && z);
    assert(apis[0].alloc(x, 6U << 20));
    assert(apis[1].alloc(y, 6U << 20));
    assert(!apis[1].alloc(z, 6U << 20) && errno == ENOMEM);
    cs_work_stats first, second;
    assert(!apis[0].stats(&first) && !apis[1].stats(&second));
    assert(first.capacity_bytes == (16U << 20) && first.used_bytes == second.used_bytes &&
           first.requested_bytes == (12U << 20) && first.active_contexts == 3);
    assert(!apis[0].end(x));
    assert(apis[1].alloc(z, 6U << 20));
    assert(!apis[1].end(y) && !apis[1].end(z));
    assert(!apis[0].stats(&first) && !first.used_bytes && !first.active_contexts);
    dlclose(a);
    dlclose(b);
    puts("PASS: two RTLD_LOCAL operator libraries share one budget; cross-library release restores capacity");
}
