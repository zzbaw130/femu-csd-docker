/* Planner arithmetic only. No device ABI or execution policy lives here. */
#ifndef PG_SINA_COST_MODEL_H
#define PG_SINA_COST_MODEL_H
#include "plan.h"
#include <math.h>
#include <stdbool.h>

static inline double cost_max(double a,double b) { return a>b?a:b; }
static inline double cost_min(double a,double b) { return a<b?a:b; }
static inline double cost_slots(double count,double minimum)
{
    double slots=minimum;
    while(slots<count && slots<0x1p52) slots*=2;
    return slots;
}
/* Same packed payload as device plan.c::hash_row_bytes, including NULL and
 * numeric scale bytes. PG plan_width is not this representation. */
static inline unsigned cost_hash_row(const struct qs_plan *p)
{
    unsigned bytes=0;
    for(unsigned i=0;i<p->nout;++i) {
        unsigned oid=p->outputs[i].type_oid;
        unsigned value=oid==16 || oid==21 || oid==23 || oid==1082?4:oid==20?8:16;
        bytes+=value+(value<16?1:3);
    }
    return bytes;
}
static inline double cost_hash_state(double rows,unsigned row_bytes)
{ return cost_slots(rows,128)*(row_bytes+3*sizeof(uint32_t)); }
/* compact_group: 16-byte header; group_value: one aligned cell + uint64.
 * group_entry: atomic uint + row index. Dense records fill 7/8 of slots. */
static inline unsigned cost_group_stride(const struct qp_node *n)
{
    bool integer=true;
    for(unsigned k=0;k<n->nkeys;++k) {
        unsigned type=n->expr.exprs[n->expr.keys[k]].type;
        if(type!=Q_INTEGER && type!=Q_STRING) integer=false;
    }
    bool wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
    unsigned value=(sizeof(struct qs_cell)+sizeof(uint64_t)+15)&~15U;
    unsigned bytes=16+n->expr.naggs*value+
        (wide?n->nkeys*(integer?sizeof(int64_t):sizeof(struct qs_cell)):0);
    return (bytes+15)&~15U;
}
static inline double cost_group_state(const struct qp_node *n,double groups)
{
    double capacity=cost_slots(groups*8/7,128);
    return capacity*(8+(7.0/8)*cost_group_stride(n))+128;
}
/* A wave includes the longest task, not total work / configured CU count.
 * Even balanced work pays an incomplete last wave and per-wave scheduling.
 * Serial setup/combine/reindex is charged by the caller, never divided. */
static inline double cost_parallel(double work,double tasks,unsigned workers,double task_ms)
{
    if(workers<=1 || tasks<=1) return work;
    tasks=ceil(tasks);
    return ceil(tasks/workers)*(work/tasks+task_ms);
}
/* Sort uses 1MiB runs (or smaller under its quota), 64KiB input caches and
 * bounded 2..32-way consolidation. Charge initial eviction separately from
 * full intermediate passes; the final merge only reads spilled payload. */
struct cost_sort { double state,runs,spill,comparisons,merge; unsigned fan; };
static inline struct cost_sort cost_sort_state(double rows,double width,double available)
{
    struct cost_sort s={0};
    double run_bytes=1024*1024;
    while(run_bytes>QS_SORT_IO_BYTES && run_bytes>available/4) run_bytes/=2;
    double run_rows=cost_max(1,floor(run_bytes/QS_SORT_IO_BYTES)*floor(QS_SORT_IO_BYTES/width));
    s.runs=cost_max(1,ceil(rows/run_rows));
    double payload=rows*width,metadata=s.runs*(128+sizeof(void *))+ceil(payload/QS_SORT_IO_BYTES)*8;
    s.state=payload+metadata+2*QS_SORT_IO_BYTES;
    s.fan=(unsigned)cost_min(32,cost_max(2,floor((available-metadata-2*QS_SORT_IO_BYTES-4096)/QS_SORT_IO_BYTES)));
    double resident=cost_max(0,available-metadata-run_bytes-2*QS_SORT_IO_BYTES);
    double evicted=cost_max(0,payload-resident),runs=s.runs;
    s.spill=2*evicted;
    while(evicted>0 && runs>s.fan) { s.spill+=2*payload; runs=ceil(runs/s.fan); }
    s.comparisons=rows*log2(cost_max(2,cost_min(rows,run_rows)));
    s.merge=rows*log2(cost_max(1,s.runs));
    return s;
}
#endif
