#include "cs_workmem.h"
int main()
{
    cs_work_context *ctx = cs_work_begin_limit(4096);
    if (!ctx)
        return 1;
    void *p = cs_work_alloc(ctx, 16);
    return p && !cs_work_end(ctx) ? 0 : 1;
}
