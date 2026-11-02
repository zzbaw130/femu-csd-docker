#ifndef PG_SINA_INTERNAL_H
#define PG_SINA_INTERNAL_H
#include "postgres.h"
#include "nodes/extensible.h"
#include "lib/stringinfo.h"
#include "portability/instr_time.h"
#include <time.h>
#include "query.h"
#include "plan.h"
#include "cs.h"
#include "storage/relfilenode.h"
/* Connection-local main-fork residency. Counts are derived from page bits,
 * never from bytes submitted for cache population. */
struct pg_sina_cache_entry {
    RelFileNode node;
    Oid relid;
    uint64 blocks,file_total,shared_pages,file_pages;
    unsigned char *shared_bits,*file_bits;
    unsigned generation,file_generation;
    bool file_known;
};
void pg_sina_cache_init(void);
/* Publish only an already completed observation; never wait for the worker. */
void pg_sina_cache_poll(void);
const struct pg_sina_cache_entry *pg_sina_cache_lookup(RelFileNode);
void pg_sina_cache_buffer(int buffer);
extern int pg_sina_exe_path;
enum pg_sina_path { PG_SINA_HOST, PG_SINA_FILTER, PG_SINA_CSD, PG_SINA_HPCP, PG_SINA_ADAPTIVE };
const char *pg_sina_path_name(void);
bool pg_sina_hpcp_enabled(void);
extern bool pg_sina_dcp;
void pg_sina_dcp_register(const struct qs_request *,const struct qs_extent *);
void pg_sina_dcp_start(CS_DEV_HANDLE);
/* Whole slots are safe when callbacks advance tasks independently; otherwise
 * bound foreground work. The return value replaces an idle wait. */
bool pg_sina_dcp_poll(bool whole_slot);
void pg_sina_dcp_finish(void);
extern double pg_sina_host_cost_ms, pg_sina_csd_cpu_ratio;
extern double pg_sina_internal_mbps, pg_sina_pcie_mbps, pg_sina_cache_mbps;
extern double pg_sina_internal_read_iops;
extern double pg_sina_internal_write_mbps;
extern double pg_sina_result_batch_ms, pg_sina_parallel_task_ms;
extern bool pg_sina_profile;
/* Existing profile switch also measures planner work; all fields are ms.
 * The pointer is query-local and restored for nested planner invocations. */
struct pg_sina_planner_profile {
    double rewrite,preview,properties,compile,estimate,format,retain,host_cache,stmt_copy;
    double native_pg,adaptive_pg;
    double indexes,resources;
    unsigned candidates,accepted,index_calls,index_queries;
    unsigned property_nodes,encoded_nodes,serialized_plans,fast_records;
    double cpu,resource_cpu;
    double resource_open,resource_properties,resource_load,resource_compute,resource_unload,resource_close;
    double download,activate,function_id,deactivate,unload;
    double prepare_copy,prepare_normalize,prepare_prune,prepare_algorithms;
    double describe,mapping,refine;
    double cost_reach,cost_buffers,cost_dictionary,cost_operators,cost_finish;
    double retain_copy,retain_free,retain_publish,freeze,hook_other,native_core,adaptive_core,cache_probe;
    unsigned cache_probes,cache_stats_calls,cache_mincore_calls;
    unsigned schema_reads,schema_reuses,preview_borrows,prune_reuses;
    double cache_cpu,cache_metadata,cache_files,cache_residency,cache_count;
    uint64_t cache_pages;
    unsigned open_major_faults,open_minor_faults;
};
extern struct pg_sina_planner_profile *pg_sina_planning;
static inline double pg_sina_planner_clock(void)
{
    if(!pg_sina_planning) return 0;
    instr_time now; INSTR_TIME_SET_CURRENT(now); return INSTR_TIME_GET_MILLISEC(now);
}
static inline double pg_sina_planner_cpu(void)
{
    if(!pg_sina_planning) return 0;
    struct timespec now; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&now);
    return now.tv_sec*1000.0+now.tv_nsec/1e6;
}
/* Already attributed exclusive work. Inclusive index/resource counters are
 * deliberately excluded; freeze is otherwise part of the old PG remainder. */
static inline double pg_sina_planner_attributed(void)
{
    const struct pg_sina_planner_profile *p=pg_sina_planning;
    return p?p->preview+p->properties+p->compile+p->estimate+p->format+p->retain+
        p->host_cache+p->stmt_copy+p->freeze:0;
}
extern int pg_sina_max_memory;
/* Per-backend device capacities, populated before finalizing an offloaded plan.
 * Workmem is private compute memory; fdm_bytes is only the I/O buffer pool. */
extern struct pg_sina_resources {
    unsigned cus;
    uint64_t fdm_bytes,workmem_bytes;
} pg_sina_resources;
void pg_sina_init_device(void);
void pg_sina_read_resources(const char *device);
CS_DEV_HANDLE pg_sina_device(const char *);
const char *pg_sina_device_path(void);
void pg_sina_release_device(void);
CS_CSF_ID pg_sina_function(const char *);
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
List **pg_sina_search_scan_schemas(void);
struct qp_plan *pg_sina_cost_workspace(void);
enum { PG_SINA_GROUP_FILTER=1U<<8, PG_SINA_LEFT_COUNT=1U<<9, PG_SINA_DISTINCT=1U<<10, PG_SINA_DISTINCT_SORT=1U<<11 };
/* Query-local choice, never a wire field: low bits select relational rewrites;
 * high bits freeze the node/access algorithms and index residual presence
 * checked by the final encoder (residuals affect probe early-exit costs). */
static inline unsigned pg_sina_physical_choice(const struct qp_node *n)
{ return ((n->kind+1)<<16)|(n->hash<<24)|(n->sorted_input<<25)|(n->ordered_index<<26)|
    (n->kind==QP_INDEX_JOIN && n->join_qual>=0?1U<<30:0); }
unsigned *pg_sina_candidate_choices(void);
unsigned pg_sina_encoding_choice(unsigned);
bool pg_sina_encoding_matches(unsigned,const struct qp_node *);
PlannedStmt *pg_sina_original_device_statement(PlannedStmt *);
bool pg_sina_prepared_device(Oid,Oid,char *,size_t);
void pg_sina_rel_paths(PlannerInfo *,RelOptInfo *,Index,RangeTblEntry *);
void pg_sina_join_paths(PlannerInfo *,RelOptInfo *,RelOptInfo *,RelOptInfo *,JoinType,JoinPathExtraData *);
void pg_sina_search_upper_paths(PlannerInfo *,UpperRelationKind,RelOptInfo *,RelOptInfo *);
Plan *pg_sina_cost_device(PlannerInfo *,Plan *,bool,List **,double *,double *);
List *pg_sina_selected_costs(PlannerInfo *,Plan *,bool,bool,Node *);
void pg_sina_cost_host_path(PlannerInfo *,Path *,Oid,List **,bool);
void pg_sina_refine_host_anti(PlannerInfo *,RelOptInfo *,RelOptInfo *,JoinType,JoinPathExtraData *);
void pg_sina_cost_host_lookup(PlannerInfo *,Path *,JoinPathExtraData *,List **);
Path *pg_sina_reprice_index_join(PlannerInfo *,NestPath *,JoinPathExtraData *,List **);
double pg_sina_host_join_cpu(PlannerInfo *,NestPath *,JoinPathExtraData *);
double pg_sina_host_scan_cost(Plan *,Oid,double,List **);
double pg_sina_host_statement_cost(PlannedStmt *);
double pg_sina_host_parallel_cost(PlannedStmt *);
Plan *pg_sina_wrap_placement(Plan *,const char *,const char *);
extern const CustomScanMethods pg_sina_scan_methods;
extern const CustomScanMethods pg_sina_order_methods;
struct pg_sina_stream;
void pg_sina_upper_paths(PlannerInfo *, UpperRelationKind, RelOptInfo *, RelOptInfo *, void *);
bool pg_sina_rewrite_query(Query *q);
Plan *pg_sina_rewrite_plan(PlannerInfo *,Plan *);
Plan *pg_sina_compile_subtree(PlannerInfo *,Plan *,bool);
static inline bool pg_sina_is_scan(Plan *plan)
{ return IsA(plan,SeqScan) || IsA(plan,IndexScan) || IsA(plan,IndexOnlyScan); }
/* Validate and collect only the HOST InitPlan values used by this scan. */
bool pg_sina_scan_parameters(PlannerInfo *,Plan *,List **);
Plan *pg_sina_prepare_candidate(PlannerInfo *,Plan *,bool);
Plan *pg_sina_require_output_order(PlannerInfo *,Plan *);
/* Cost properties and a borrowed workspace, valid until the next candidate.
 * Candidate Const holds binary cost metadata, never an executable program. */
Plan *pg_sina_describe_candidate(PlannerInfo *,Plan *,bool,struct qp_plan **);
Plan *pg_sina_candidate_scan(Plan *,bool,bool);
void pg_sina_finish_device_plans(Plan *);
void pg_sina_refine_groups(struct qp_plan *);
Plan *pg_sina_adaptive_plan(PlannerInfo *,Plan *);
void pg_sina_explain_placement(CustomScan *,ExplainState *);
extern const CustomScanMethods pg_sina_adaptive_methods;
Plan *pg_sina_fast_plan(PlannerInfo *,Plan *);
Plan *pg_sina_fast_properties(PlannerInfo *,Plan *,struct qs_plan **);
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
