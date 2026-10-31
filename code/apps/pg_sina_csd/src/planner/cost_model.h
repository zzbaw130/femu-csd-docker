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
/* Uniform page occupancy within vector batches, with the cursor's bounded
 * cache reused between batches. A small relation fits; large random probes
 * may reread the same page after eviction. Never cap reads at table size. */
static inline double cost_index_pages(double visits,double pages,double cache,double batch)
{
    if(visits<=0 || pages<=0) return 0;
    double unique=-pages*expm1(-visits/pages);
    double full=floor(visits/batch),tail=visits-full*batch;
    double misses=(-full*pages*expm1(-batch/pages)-pages*expm1(-tail/pages))*(1-cost_min(1,cache/pages));
    return cost_max(unique,misses);
}
/* PG correlation describes how consecutive index matches share Heap pages.
 * Random matches may need one page per tuple; clustered matches share pages. */
static inline double cost_index_heap_visits(double probes,double matches,double tuples_per_page,double correlation)
{
    if(probes<=0 || matches<=0) return 0;
    double clustered=cost_max(1,matches/cost_max(1,tuples_per_page));
    double weight=correlation*correlation;
    return probes*(weight*clustered+(1-weight)*matches);
}
/* DuckDB CardinalityEstimator uses DEFAULT_SEMI_ANTI_SELECTIVITY = 5.
 * Use its 20% fallback only for degenerate residual self anti estimates. */
static inline double cost_anti_fallback(double rows) { return rows/5; }
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
struct cost_sort { double state,runs,spill,comparisons,merge,record_copies; unsigned fan,passes; };
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
    while(evicted>0 && runs>s.fan) { s.spill+=2*payload; runs=ceil(runs/s.fan); ++s.passes; }
    s.comparisons=rows*log2(cost_max(2,cost_min(rows,run_rows)));
    s.merge=rows*log2(cost_max(1,s.runs));
    s.record_copies=rows*(s.passes+1);
    return s;
}
/* Interpolate uniform occupancy and clustered keys using PG correlation^2.
 * This estimates duplicates across flushes; it is not an exact NDV. */
static inline double cost_partial_rows(double rows,double groups,double resident_groups,double correlation)
{
    if(rows<=0 || groups<=0) return 0;
    double flushes=cost_max(1,ceil(groups/cost_max(1,resident_groups)));
    double random=cost_min(rows,groups*flushes*(-expm1(-rows/(groups*flushes))));
    double clustered=cost_min(rows,groups);
    double weight=cost_min(1,correlation*correlation);
    return random*(1-weight)+clustered*weight;
}
static inline double cost_spill_io(double bytes,double read_mbps,double write_mbps)
{ return bytes/2/(read_mbps*1000)+bytes/2/(write_mbps*1000); }
/* Group keys drawn only from a join's probe side cannot form more groups
 * than probe rows, even when the join emits many matching build rows. */
static inline bool cost_probe_expression(const struct qp_plan *tree,int node,int expression,int join)
{
    const struct qp_node *n=&tree->nodes[node];
    if(expression<0 || (unsigned)expression>=n->expr.nexprs) return false;
    const struct qs_expr *e=&n->expr.exprs[expression];
    if(e->op==Q_CONST || e->op==Q_PARAMETER) return true;
    if(e->op==Q_COLUMN) {
        if(node==join) return e->column>=0 && (unsigned)e->column<tree->nodes[n->left].expr.nout;
        if(n->left<0 || e->column<0) return false;
        int child=n->left,column=e->column;
        if((unsigned)column>=tree->nodes[child].expr.nout) {
            column-=tree->nodes[child].expr.nout; child=n->right;
        }
        if(child<0 || (unsigned)column>=tree->nodes[child].expr.nout) return false;
        const struct qs_output *out=&tree->nodes[child].expr.outputs[column];
        return !out->aggregate && cost_probe_expression(tree,child,out->index,join);
    }
    if(e->op==Q_AGGREF || e->left<0) return false;
    return cost_probe_expression(tree,node,e->left,join) &&
        (e->right<0 || cost_probe_expression(tree,node,e->right,join)) &&
        (e->otherwise<0 || cost_probe_expression(tree,node,e->otherwise,join));
}
#endif
