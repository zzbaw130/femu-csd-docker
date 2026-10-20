#include "femu-csd-kernel.h"
#include "query.h"
int64_t pg_sina_limit(struct femu_csd_args *a)
{
    if(!a || a->numr!=2 || a->mr_len[0]<(long long)sizeof(struct qs_result) || a->mr_len[1]<(long long)sizeof(struct qs_plan)) return -1;
    struct qs_result *out=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1];
    if(out->rows>QS_GROUPS || p->offset<0 || p->limit< -1) return -1;
    out->begin=(uint64_t)p->offset>out->rows?out->rows:(unsigned)p->offset;
    out->rows-=out->begin;
    if(p->limit>=0 && (uint64_t)p->limit<out->rows) out->rows=p->limit;
    return 0;
}
