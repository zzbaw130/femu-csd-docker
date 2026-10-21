/* Optional entry instrumentation, shared by the PG query CSFs.
 * cparam1=1 adds a counter AFDM, last normally and before spill I/O for
 * reentrant entries, preserving SNIA's last-AFDM I/O convention.
 * The wrapper removes it before calling the unchanged implementation. */
#include "femu-csd-kernel.h"
#include "query.h"
#include <time.h>
typedef int64_t (*csf_function)(struct femu_csd_args *);
static uint64_t elapsed(struct timespec start,struct timespec end)
{
    return (uint64_t)(end.tv_sec-start.tv_sec)*1000000000ULL+end.tv_nsec-start.tv_nsec;
}
static int64_t measured(struct femu_csd_args *args,csf_function function,unsigned op,int reentrant)
{
    if(!args || !args->cparam1) return function(args);
    if(args->cparam1!=1 || args->numr<1 || !args->mr_len || !args->mr_addr ||
        args->numr>8 || args->numr<=reentrant ||
        args->mr_len[args->numr-1-reentrant]<(long long)sizeof(struct qs_profile)) return -1;
    struct femu_csd_args call=*args; --call.numr; call.cparam1=0;
    unsigned counter=call.numr-reentrant;
    struct qs_profile *profile=args->mr_addr[counter];
    void *addresses[8]; long long lengths[8];
    if(reentrant) {
        for(unsigned i=0,j=0;i<(unsigned)args->numr;++i) if(i!=counter) {
            addresses[j]=args->mr_addr[i]; lengths[j++]=args->mr_len[i];
        }
        call.mr_addr=addresses; call.mr_len=lengths;
    }
    struct timespec wall_start,cpu_start,cpu_end,wall_end;
    int wall_ok=!clock_gettime(CLOCK_MONOTONIC,&wall_start);
    int cpu_ok=!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_start);
    int64_t result=function(&call);
    cpu_ok&=!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_end);
    wall_ok&=!clock_gettime(CLOCK_MONOTONIC,&wall_end);
    struct qs_timing *t=&profile->operators[op]; ++t->calls;
    if(wall_ok) {
        uint64_t ns=elapsed(wall_start,wall_end); t->wall_ns+=ns;
        if(ns>t->max_wall_ns) t->max_wall_ns=ns;
    }
    if(cpu_ok) t->cpu_ns+=elapsed(cpu_start,cpu_end);
    return result;
}
#define ENTRY(name,op,reentrant) \
    extern int64_t name##_impl(struct femu_csd_args *); \
    int64_t name(struct femu_csd_args *args) { return measured(args,name##_impl,op,reentrant); }
#if defined(PROFILE_SCAN)
ENTRY(pg_sina_scan,QS_SCAN,0)
#elif defined(PROFILE_FILTER)
ENTRY(pg_sina_filter,QS_FILTER,0)
#elif defined(PROFILE_AGGREGATE)
ENTRY(pg_sina_aggregate_sink,QS_AGG_SINK,0)
ENTRY(pg_sina_aggregate_source,QS_AGG_SOURCE,0)
#elif defined(PROFILE_SORT)
ENTRY(pg_sina_sort,QS_SORT,0)
ENTRY(pg_sina_sort_sink,QS_SORT_SINK,1)
ENTRY(pg_sina_sort_source,QS_SORT_SOURCE,1)
#elif defined(PROFILE_LIMIT)
ENTRY(pg_sina_limit,QS_LIMIT,0)
#endif
