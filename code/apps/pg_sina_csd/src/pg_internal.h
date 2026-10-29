#ifndef PG_SINA_INTERNAL_H
#define PG_SINA_INTERNAL_H
#include "postgres.h"
#include "nodes/extensible.h"
#include "query.h"
#include "plan.h"
extern int pg_sina_exe_path;
enum pg_sina_path { PG_SINA_HOST, PG_SINA_FILTER, PG_SINA_CSD, PG_SINA_HPCP, PG_SINA_ADAPTIVE };
const char *pg_sina_path_name(void);
bool pg_sina_hpcp_enabled(void);
extern double pg_sina_host_cost_ms, pg_sina_csd_cpu_ratio;
extern double pg_sina_internal_mbps, pg_sina_pcie_mbps, pg_sina_cache_mbps;
extern double pg_sina_result_batch_ms, pg_sina_parallel_task_ms;
extern bool pg_sina_profile;
extern int pg_sina_max_memory;
/* Per-backend device capacities, populated before finalizing an offloaded plan.
 * Workmem is private compute memory; fdm_bytes is only the I/O buffer pool. */
extern struct pg_sina_resources {
    unsigned cus;
    uint64_t fdm_bytes,workmem_bytes;
} pg_sina_resources;
void pg_sina_read_resources(const char *device);
unsigned pg_sina_program_index(unsigned index);
static inline uint64_t pg_sina_query_memory(void)
{
    uint64_t physical=pg_sina_resources.workmem_bytes;
    return pg_sina_max_memory?Min(physical,(uint64_t)pg_sina_max_memory*1024):physical;
}
extern bool pg_sina_compiling_plan;
extern const CustomScanMethods pg_sina_scan_methods;
extern const CustomScanMethods pg_sina_order_methods;
struct pg_sina_stream;
void pg_sina_upper_paths(PlannerInfo *, UpperRelationKind, RelOptInfo *, RelOptInfo *, void *);
bool pg_sina_rewrite_query(Query *q);
Plan *pg_sina_rewrite_plan(PlannerInfo *,Plan *);
Plan *pg_sina_compile_subtree(PlannerInfo *,Plan *,bool);
Plan *pg_sina_adaptive_plan(PlannerInfo *,Plan *);
void pg_sina_explain_placement(CustomScan *,ExplainState *);
extern const CustomScanMethods pg_sina_adaptive_methods;
Plan *pg_sina_fast_plan(PlannerInfo *,Plan *);
Plan *pg_sina_native_plan(Plan *);
extern const CustomScanMethods pg_sina_plan_methods;
struct qs_extent *pg_sina_mapping(struct qs_request *request);
void pg_sina_execute(struct qs_request *,struct qs_extent *,struct qs_result *,bool,struct pg_sina_stream **);
void pg_sina_fetch(struct pg_sina_stream **,struct qs_result *);
void pg_sina_close(struct pg_sina_stream *);
Plan *pg_sina_project_scan(Plan *scope,Plan *scan);
void pg_sina_remap_input(Plan *parent,List *old,List *projected);
#endif
