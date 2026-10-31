#ifndef PG_SINA_INTERNAL_H
#define PG_SINA_INTERNAL_H
#include "postgres.h"
#include "nodes/extensible.h"
#include "lib/stringinfo.h"
#include "portability/instr_time.h"
#include "query.h"
#include "plan.h"
extern int pg_sina_exe_path;
enum pg_sina_path { PG_SINA_HOST, PG_SINA_FILTER, PG_SINA_CSD, PG_SINA_HPCP, PG_SINA_ADAPTIVE };
const char *pg_sina_path_name(void);
bool pg_sina_hpcp_enabled(void);
extern double pg_sina_host_cost_ms, pg_sina_csd_cpu_ratio;
extern double pg_sina_internal_mbps, pg_sina_pcie_mbps, pg_sina_cache_mbps;
extern double pg_sina_internal_write_mbps;
extern double pg_sina_result_batch_ms, pg_sina_parallel_task_ms;
extern bool pg_sina_profile;
/* Existing profile switch also measures planner work; all fields are ms.
 * The pointer is query-local and restored for nested planner invocations. */
struct pg_sina_planner_profile {
    double rewrite,preview,compile,estimate,format,retain,host_cache,stmt_copy;
    double native_pg,adaptive_pg;
    double indexes,resources;
    unsigned candidates,accepted,index_calls,index_queries;
    unsigned lower_calls,lower_hits,lower_reused_nodes,serialized_plans;
};
extern struct pg_sina_planner_profile *pg_sina_planning;
static inline double pg_sina_planner_clock(void)
{
    if(!pg_sina_planning) return 0;
    instr_time now; INSTR_TIME_SET_CURRENT(now); return INSTR_TIME_GET_MILLISEC(now);
}
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
/* Query-local Path search; the pointer is saved/restored around planner hooks. */
extern struct pg_sina_path_search *pg_sina_path_search;
extern bool pg_sina_native_search, pg_sina_previewing_path;
void pg_sina_start_path_search(void);
MemoryContext pg_sina_search_context(void);
List **pg_sina_search_observations(void);
List **pg_sina_search_index_metadata(void);
List **pg_sina_search_lowering_cache(void);
struct qp_plan *pg_sina_lowering_workspace(void);
PlannedStmt *pg_sina_original_device_statement(PlannedStmt *);
bool pg_sina_prepared_device(Oid,Oid,char *,size_t);
void pg_sina_rel_paths(PlannerInfo *,RelOptInfo *,Index,RangeTblEntry *);
void pg_sina_join_paths(PlannerInfo *,RelOptInfo *,RelOptInfo *,RelOptInfo *,JoinType,JoinPathExtraData *);
void pg_sina_search_upper_paths(PlannerInfo *,UpperRelationKind,RelOptInfo *,RelOptInfo *);
Plan *pg_sina_cost_device(PlannerInfo *,Plan *,bool,List **,double *,double *,StringInfo);
double pg_sina_host_scan_cost(Oid,bool,double,double,List **);
double pg_sina_host_statement_cost(PlannedStmt *);
Plan *pg_sina_wrap_placement(Plan *,const char *,const char *);
extern const CustomScanMethods pg_sina_scan_methods;
extern const CustomScanMethods pg_sina_order_methods;
struct pg_sina_stream;
void pg_sina_upper_paths(PlannerInfo *, UpperRelationKind, RelOptInfo *, RelOptInfo *, void *);
bool pg_sina_rewrite_query(Query *q);
Plan *pg_sina_rewrite_plan(PlannerInfo *,Plan *);
Plan *pg_sina_compile_subtree(PlannerInfo *,Plan *,bool);
/* Cost-only metadata and a borrowed aligned tree, valid until the next
 * candidate. The returned CustomScan has no executable Const. */
Plan *pg_sina_lower_subtree(PlannerInfo *,Plan *,bool,struct qp_plan **);
void pg_sina_refine_groups(struct qp_plan *);
Plan *pg_sina_adaptive_plan(PlannerInfo *,Plan *);
void pg_sina_explain_placement(CustomScan *,ExplainState *);
extern const CustomScanMethods pg_sina_adaptive_methods;
Plan *pg_sina_fast_plan(PlannerInfo *,Plan *);
Plan *pg_sina_native_plan(Plan *);
Plan *pg_sina_native_wrapper(Plan *);
extern const CustomScanMethods pg_sina_plan_methods;
struct qs_extent *pg_sina_mapping(struct qs_request *request);
void pg_sina_execute(struct qs_request *,struct qs_extent *,struct qs_result *,bool,struct pg_sina_stream **);
void pg_sina_fetch(struct pg_sina_stream **,struct qs_result *);
void pg_sina_close(struct pg_sina_stream *);
Plan *pg_sina_project_scan(Plan *scope,Plan *scan);
struct qs_cell pg_sina_parameter_cell(Datum value,Oid type,bool isnull);
void pg_sina_remap_input(Plan *parent,List *old,List *projected);
#endif
