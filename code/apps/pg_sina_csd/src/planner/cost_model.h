/* Planner arithmetic only. No device ABI or execution policy lives here. */
#ifndef PG_SINA_COST_MODEL_H
#define PG_SINA_COST_MODEL_H
#include "plan.h"
#include <math.h>
#include <stdbool.h>

static inline double cost_max(double a,double b) { return a>b?a:b; }
static inline double cost_min(double a,double b) { return a<b?a:b; }
/* Overlap of the two caches is unknown: use the conservative union. The
 * caller retains CPU costs; only file-only hits pay the extra page copy. */
static inline double cost_host_cached_io(double io,double shared,double file,double copy_ratio)
{ return io*(1-cost_max(shared,file))+io*cost_max(0,file-shared)*copy_ratio; }
/* PG's abstract sequential page price need not share its CPU-to-ms scale.
 * Bound that price by the modeled NVM read, external transfer and PG buffer
 * copy. File-only hits retain their actual copy cost after this correction. */
static inline double cost_host_scan_io(double original,double storage,double link,double copy,double shared,double file)
{
    double cold=cost_min(original,storage+link+copy);
    return cold*(1-cost_max(shared,file))+cost_min(original,copy)*cost_max(0,file-shared);
}
/* Bound vectors/columns and literals carry values; they do not execute an
 * arithmetic instruction. Materialization/Heap cost is charged separately. */
static inline unsigned cost_expression_ops(const struct qs_plan *p)
{
    unsigned count=0;
    for(unsigned i=0;i<p->nexprs;++i) {
        unsigned op=p->exprs[i].op;
        /* Candidate properties use Q_AGGREF with column=-1 for an
         * unevaluated operation; real aggregate references have an index. */
        count+=op!=Q_COLUMN && op!=Q_CONST && op!=Q_PARAMETER &&
            (op!=Q_AGGREF || p->exprs[i].column<0);
    }
    return count;
}
/* Either destination may still need a page. Cache population never lowers
 * CSD internal LOAD cost: it only suppresses already unnecessary DMA. */
static inline double cost_population_fraction(bool file_enabled,bool shared_enabled,double file,double shared)
{ return cost_max(file_enabled?1-file:0,shared_enabled?1-shared:0); }
/* LOAD and compute remain serial within one task, but independent CU tasks
 * overlap. Retain one participant's fill/drain share of the shorter stage. */
static inline double cost_scan_overlap(double io,double cpu,double tasks,unsigned cus)
{
    double lanes=cost_max(1,cost_min(tasks,cus));
    return cost_min(io,cpu)*(1-1/lanes);
}
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
/* Expected candidates inspected until the first success, including the
 * no-match case. Predicate independence is the same statistical assumption
 * used for scan selectivity; unknown residuals keep the full scan. */
static inline double cost_first_match_visits(double candidates,double selectivity)
{
    if(candidates<=0 || selectivity<=0) return candidates;
    if(selectivity>=1) return cost_min(1,candidates);
    return -expm1(candidates*log1p(-selectivity))/selectivity;
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
/* Only a proven self-key probe may borrow its current scan morsel. PG's
 * physical correlation estimates the clustered fraction; groups crossing a
 * morsel boundary and uncorrelated tuples still require internal reads. */
static inline double cost_index_window_misses(double visits,double matches,double tuples_per_page,double window_pages,double correlation)
{
    double footprint=cost_max(1,matches/cost_max(1,tuples_per_page));
    double within=window_pages>0?cost_max(0,1-footprint/window_pages):0;
    return visits*(1-correlation*correlation*within);
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
/* Existence-key compaction happens only when growth would exceed the budget.
 * The initial table still allocates the largest fitting power-of-two, not
 * merely the NDV-sized payload. Missing statistics retain the general model. */
static inline double cost_hash_existence_state(double rows,double distinct,unsigned width,double available)
{
    double full=cost_hash_state(rows,width),stride=width+3*sizeof(uint32_t);
    if(full<=available || distinct<=0 || cost_hash_state(cost_min(rows,distinct),width)>available) return full;
    double capacity=1;
    while(capacity*2*stride<=available) capacity*=2;
    return cost_min(full,capacity*stride);
}
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
/* The runtime allocates complete power-of-two hash tables, keeping 7/8 of
 * their slots for dense groups. Remaining memory belongs to the spill sort,
 * rather than to a fictitious fractional hash allocation. */
static inline double cost_group_resident(const struct qp_node *n,double groups,double available)
{
    double stride=8+(7.0/8)*cost_group_stride(n),capacity=128,goal=cost_slots(groups*8/7,128);
    while(capacity<goal && capacity*2*stride+128<=available) capacity*=2;
    return cost_min(cost_group_state(n,groups),capacity*stride+128);
}
/* plan.c::sort_init borrows free parent memory, reserving two depth quotas.
 * The depth quota is a floor, not the maximum size of a child sort. */
static inline double cost_sort_memory(double available,double quota)
{ return cost_max(quota,cost_max(available/2,available-2*quota)); }
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
 * memory-bounded consolidation. Charge initial eviction separately from
 * full intermediate passes; the final merge only reads spilled payload. */
struct cost_sort { double state,runs,spill,comparisons,merge,record_copies; unsigned fan,passes; };
/* Estimate the executor's minimum working set from public buffer layouts.
 * The final 64KiB covers private run/control metadata; this is not a quota. */
static inline double cost_sort_workspace(void)
{
    return 3*1024*1024+32*QS_SORT_IO_BYTES+sizeof(struct qs_plan)+
        QS_EXPRS*(256*20+128.0)+2*sizeof(struct qs_result)+QS_SORT_IO_BYTES;
}
static inline struct cost_sort cost_sort_state(double rows,double width,double available,unsigned windows)
{
    struct cost_sort s={0};
    double run_bytes=1024*1024;
    while(run_bytes>QS_SORT_IO_BYTES && run_bytes>available/4) run_bytes/=2;
    double run_rows=cost_max(1,floor(run_bytes/QS_SORT_IO_BYTES)*floor(QS_SORT_IO_BYTES/width));
    s.runs=cost_max(1,ceil(rows/run_rows));
    double payload=rows*width,metadata=s.runs*(128+sizeof(void *))+ceil(payload/QS_SORT_IO_BYTES)*8;
    s.state=payload+metadata+2*QS_SORT_IO_BYTES;
    s.fan=(unsigned)cost_max(2,floor((available-metadata-2*QS_SORT_IO_BYTES-4096)/(cost_max(1,windows)*QS_SORT_IO_BYTES)));
    double resident=cost_max(0,available-metadata-run_bytes-2*QS_SORT_IO_BYTES);
    double evicted=cost_max(0,payload-resident),runs=s.runs;
    s.spill=2*evicted;
    while(evicted>0 && runs>s.fan) { s.spill+=2*payload; runs=ceil(runs/s.fan); ++s.passes; }
    s.comparisons=rows*log2(cost_max(2,cost_min(rows,run_rows)));
    s.merge=rows*log2(cost_max(1,s.runs));
    s.record_copies=rows*(s.passes+1);
    return s;
}
/* TopN inspects all input rows, but retains only bound rows. If that state
 * cannot fit, use the sorter's full external path; never erase real spill. */
static inline struct cost_sort cost_topn_state(double rows,double width,double available,unsigned windows,int64_t bound)
{
    double retained=cost_min(rows,(double)bound),state=retained*width+2*QS_SORT_IO_BYTES;
    if(bound<0 || state>available) return cost_sort_state(rows,width,available,windows);
    struct cost_sort s={.state=state,.runs=1,.fan=2};
    s.comparisons=rows*log2(cost_max(2,retained));
    s.merge=retained*log2(cost_max(2,retained)); s.record_copies=rows+retained;
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
/* Keys derived from one join input cannot form more groups than that
 * input has rows. The caller handles outer-join NULLs and parameterized
 * index inputs, whose row estimate describes one probe, not the full input. */
static inline bool cost_join_expression(const struct qp_plan *tree,int node,int expression,int join,bool build)
{
    const struct qp_node *n=&tree->nodes[node];
    if(expression<0 || (unsigned)expression>=n->expr.nexprs) return false;
    const struct qs_expr *e=&n->expr.exprs[expression];
    if(e->op==Q_CONST || e->op==Q_PARAMETER) return true;
    if(e->op==Q_COLUMN) {
        if(node==join) {
            unsigned width=tree->nodes[n->left].expr.nout;
            if(e->column<0 || (unsigned)e->column>=width+tree->nodes[n->right].expr.nout) return false;
            if(((unsigned)e->column<width)!=build) return true;
            /* An INNER equality key can be represented by either input.
             * Outer NULL extension and parameterized indexes are excluded. */
            if(n->kind==QP_JOIN && n->join_type==QP_INNER)
                for(unsigned k=0;k<n->nkeys;++k)
                    if(e->column==(build?n->left_keys[k]:(int)width+n->right_keys[k])) return true;
            return false;
        }
        if(n->left<0 || e->column<0) return false;
        int child=n->left,column=e->column;
        if((unsigned)column>=tree->nodes[child].expr.nout) {
            column-=tree->nodes[child].expr.nout; child=n->right;
        }
        if(child<0 || (unsigned)column>=tree->nodes[child].expr.nout) return false;
        const struct qs_output *out=&tree->nodes[child].expr.outputs[column];
        return !out->aggregate && cost_join_expression(tree,child,out->index,join,build);
    }
    if(e->op==Q_AGGREF || e->left<0) return false;
    return cost_join_expression(tree,node,e->left,join,build) &&
        (e->right<0 || cost_join_expression(tree,node,e->right,join,build)) &&
        (e->otherwise<0 || cost_join_expression(tree,node,e->otherwise,join,build));
}
#endif
