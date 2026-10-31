#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/plancache.h"
#include "optimizer/planner.h"
#include "optimizer/paths.h"
#include <stdlib.h>
#include <float.h>
#include "pg_internal.h"
#include "miscadmin.h"
#include "access/parallel.h"
PG_MODULE_MAGIC;
int pg_sina_exe_path = 0;
bool pg_sina_profile = false;
int pg_sina_max_memory = 0; /* KiB; 0 uses the device workmem capacity. */
bool pg_sina_compiling_plan = false;
struct pg_sina_planner_profile *pg_sina_planning;
static create_upper_paths_hook_type previous_upper_hook;
static planner_hook_type previous_planner_hook;
static set_rel_pathlist_hook_type previous_rel_hook;
static set_join_pathlist_hook_type previous_join_hook;
static PlannedStmt *plan_query(Query *query,int cursorOptions,ParamListInfo boundParams)
{
    PlannedStmt *plan=previous_planner_hook?previous_planner_hook(query,cursorOptions,boundParams):standard_planner(query,cursorOptions,boundParams);
    plan->planTree=pg_sina_native_plan(plan->planTree);
    return plan;
}
static PlannedStmt *hooked_planner(Query *volatile parse,int cursorOptions,ParamListInfo boundParams)
{
    /* SPI mapping lookups are PG queries too, not new placement searches. */
    if(pg_sina_compiling_plan || parse->commandType!=CMD_SELECT || parse->hasModifyingCTE)
        return plan_query(parse,cursorOptions,boundParams);
    struct pg_sina_planner_profile counters={0},*saved_profile=pg_sina_planning;
    pg_sina_planning=pg_sina_profile && pg_sina_exe_path==PG_SINA_ADAPTIVE?&counters:NULL;
    double started=pg_sina_planner_clock(),mark=started,cpu_started=pg_sina_planner_cpu();
    Query *volatile query=parse;
    struct pg_sina_path_search *saved=pg_sina_path_search;
    bool native_search=pg_sina_native_search;
    PlannedStmt *volatile plan=NULL;
    PG_TRY();
    {
        if(pg_sina_exe_path>=PG_SINA_CSD) {
            Query *candidate=copyObject(parse);
            if(pg_sina_rewrite_query(candidate)) query=candidate;
        }
        if(pg_sina_planning) counters.rewrite=pg_sina_planner_clock()-mark;
        PlannedStmt *host=NULL,*original_device=NULL; double host_cost=0;
        if(pg_sina_exe_path==PG_SINA_ADAPTIVE) {
            pg_sina_start_path_search();
            pg_sina_native_search=true;
            double attributed=pg_sina_planner_attributed(),hooks=counters.hook_other;
            mark=pg_sina_planner_clock();
            host=plan_query(copyObject(parse),cursorOptions,boundParams);
            if(pg_sina_planning) {
                counters.native_pg=pg_sina_planner_clock()-mark;
                counters.native_core=counters.native_pg-(pg_sina_planner_attributed()-attributed)-(counters.hook_other-hooks);
            }
            mark=pg_sina_planner_clock();
            original_device=pg_sina_original_device_statement(host);
            if(pg_sina_planning) counters.stmt_copy=pg_sina_planner_clock()-mark;
            mark=pg_sina_planner_clock();
            host_cost=pg_sina_host_statement_cost(host);
            if(pg_sina_planning) counters.host_cache=pg_sina_planner_clock()-mark;
            pg_sina_native_search=false;
        }
        double attributed=pg_sina_planner_attributed(),hooks=counters.hook_other;
        mark=pg_sina_planner_clock();
        plan=plan_query(query,cursorOptions,boundParams);
        if(pg_sina_planning) {
            counters.adaptive_pg=pg_sina_planner_clock()-mark;
            counters.adaptive_core=counters.adaptive_pg-(pg_sina_planner_attributed()-attributed)-(counters.hook_other-hooks);
        }
        if(host) {
            double candidate=plan->planTree->total_cost*pg_sina_host_cost_ms;
            double original_cost=original_device?original_device->planTree->total_cost*pg_sina_host_cost_ms:DBL_MAX;
            if(original_device && original_cost<=candidate) plan=original_device;
            if(host_cost<Min(candidate,original_cost)) plan=host;
            if(IsA(plan->planTree,CustomScan) && ((CustomScan *)plan->planTree)->methods==&pg_sina_adaptive_methods) {
                List *info=((CustomScan *)plan->planTree)->custom_private;
                const char *original=original_device?psprintf("%.3fms",original_cost):"unsupported";
                lfirst(list_head(info))=makeString(psprintf("%s; original HOST=%.3fms original CSD=%s shared search=%.3fms",
                    strVal(linitial(info)),host_cost,original,candidate));
            }
            pg_sina_finish_device_plans(plan->planTree);
            ListCell *cell;
            foreach(cell,plan->subplans) pg_sina_finish_device_plans(lfirst(cell));
        }
    }
    PG_CATCH();
    { pg_sina_path_search=saved; pg_sina_native_search=native_search; pg_sina_planning=saved_profile; PG_RE_THROW(); }
    PG_END_TRY();
    pg_sina_path_search=saved; pg_sina_native_search=native_search;
    if(pg_sina_planning) {
        double total=pg_sina_planner_clock()-started;
        counters.cpu=pg_sina_planner_cpu()-cpu_started;
        double other=total-counters.rewrite-counters.preview-counters.properties-counters.compile-counters.estimate-
            counters.format-counters.retain-counters.host_cache-counters.stmt_copy;
        pg_sina_planning=saved_profile;
        ereport(NOTICE,(errmsg("SQLerate planner total_ms=%.3f pg_other_ms=%.3f rewrite_ms=%.3f preview_ms=%.3f properties_ms=%.3f compile_ms=%.3f estimate_ms=%.3f format_ms=%.3f retain_ms=%.3f host_cache_ms=%.3f stmt_copy_ms=%.3f native_pg_inclusive_ms=%.3f adaptive_pg_inclusive_ms=%.3f candidates=%u accepted=%u index_calls=%u index_queries=%u index_inclusive_ms=%.3f resources_inclusive_ms=%.3f property_nodes=%u encoded_nodes=%u serialized_plans=%u fast_records=%u",
            total,other,counters.rewrite,counters.preview,counters.properties,counters.compile,counters.estimate,counters.format,
            counters.retain,counters.host_cache,counters.stmt_copy,counters.native_pg,counters.adaptive_pg,
            counters.candidates,counters.accepted,counters.index_calls,counters.index_queries,counters.indexes,counters.resources,
            counters.property_nodes,counters.encoded_nodes,counters.serialized_plans,counters.fast_records)));
        ereport(NOTICE,(errmsg("SQLerate planner breakdown cpu_ms=%.3f resource_cpu_ms=%.3f resource_open_ms=%.3f resource_properties_ms=%.3f resource_load_ms=%.3f resource_compute_ms=%.3f resource_unload_ms=%.3f resource_close_ms=%.3f download_ms=%.3f activate_ms=%.3f function_id_ms=%.3f deactivate_ms=%.3f unload_ms=%.3f prepare_copy_ms=%.3f prepare_normalize_ms=%.3f prepare_prune_ms=%.3f prepare_algorithms_ms=%.3f describe_ms=%.3f mapping_ms=%.3f refine_ms=%.3f cost_reach_ms=%.3f cost_buffers_ms=%.3f cost_dictionary_ms=%.3f cost_operators_ms=%.3f cost_finish_ms=%.3f retain_copy_ms=%.3f retain_free_ms=%.3f retain_publish_ms=%.3f freeze_ms=%.3f hook_other_ms=%.3f native_core_ms=%.3f adaptive_core_ms=%.3f cache_probe_ms=%.3f cache_probes=%u open_major_faults=%u open_minor_faults=%u schema_reads=%u schema_reuses=%u",
            counters.cpu,counters.resource_cpu,counters.resource_open,counters.resource_properties,counters.resource_load,
            counters.resource_compute,counters.resource_unload,counters.resource_close,counters.download,counters.activate,
            counters.function_id,counters.deactivate,counters.unload,counters.prepare_copy,counters.prepare_normalize,
            counters.prepare_prune,counters.prepare_algorithms,counters.describe,counters.mapping,counters.refine,
            counters.cost_reach,counters.cost_buffers,counters.cost_dictionary,counters.cost_operators,counters.cost_finish,
            counters.retain_copy,counters.retain_free,counters.retain_publish,counters.freeze,counters.hook_other,
            counters.native_core,counters.adaptive_core,counters.cache_probe,counters.cache_probes,counters.open_major_faults,counters.open_minor_faults,counters.schema_reads,counters.schema_reuses)));
        ereport(NOTICE,(errmsg("SQLerate cache observation cpu_ms=%.3f metadata_ms=%.3f files_ms=%.3f residency_ms=%.3f count_ms=%.3f pages=%llu cachestat_calls=%u mincore_calls=%u",
            counters.cache_cpu,counters.cache_metadata,counters.cache_files,counters.cache_residency,counters.cache_count,
            (unsigned long long)counters.cache_pages,counters.cache_stats_calls,counters.cache_mincore_calls)));
    }
    pg_sina_planning=saved_profile;
    return plan;
}
static void upper_hook(PlannerInfo *root,UpperRelationKind stage,RelOptInfo *input,RelOptInfo *output,void *extra)
{
    if(previous_upper_hook) previous_upper_hook(root,stage,input,output,extra);
    if(!pg_sina_compiling_plan) {
        double mark=pg_sina_planner_clock(),attributed=pg_sina_planner_attributed();
        if(pg_sina_exe_path==PG_SINA_ADAPTIVE) pg_sina_search_upper_paths(root,stage,input,output);
        else pg_sina_upper_paths(root,stage,input,output,extra);
        if(pg_sina_planning) pg_sina_planning->hook_other+=pg_sina_planner_clock()-mark-(pg_sina_planner_attributed()-attributed);
    }
}
static void rel_hook(PlannerInfo *root,RelOptInfo *rel,Index rti,RangeTblEntry *rte)
{
    if(previous_rel_hook) previous_rel_hook(root,rel,rti,rte);
    bool measure=pg_sina_planning && !pg_sina_compiling_plan;
    double mark=measure?pg_sina_planner_clock():0,attributed=measure?pg_sina_planner_attributed():0;
    pg_sina_rel_paths(root,rel,rti,rte);
    if(measure) pg_sina_planning->hook_other+=pg_sina_planner_clock()-mark-(pg_sina_planner_attributed()-attributed);
}
static void join_hook(PlannerInfo *root,RelOptInfo *joinrel,RelOptInfo *outerrel,RelOptInfo *innerrel,
                      JoinType jointype,JoinPathExtraData *extra)
{
    if(previous_join_hook) previous_join_hook(root,joinrel,outerrel,innerrel,jointype,extra);
    bool measure=pg_sina_planning && !pg_sina_compiling_plan;
    double mark=measure?pg_sina_planner_clock():0,attributed=measure?pg_sina_planner_attributed():0;
    pg_sina_join_paths(root,joinrel,outerrel,innerrel,jointype,extra);
    if(measure) pg_sina_planning->hook_other+=pg_sina_planner_clock()-mark-(pg_sina_planner_attributed()-attributed);
}
static void assign_path(int value,void *extra)
{
    if(value!=pg_sina_exe_path) ResetPlanCache();
    setenv("SNIA_FEMU_HPCP",value==PG_SINA_HPCP || value==PG_SINA_ADAPTIVE?"1":"0",1);
}
const char *pg_sina_path_name(void)
{
    static const char *names[]={"host-only","filter-only","csd-without-hpcp","csd-with-hpcp","SQLerate"};
    return names[pg_sina_exe_path];
}
bool pg_sina_hpcp_enabled(void)
{ return pg_sina_exe_path==PG_SINA_HPCP || pg_sina_exe_path==PG_SINA_ADAPTIVE; }
static void assign_memory(int value,void *extra)
{
    if(value!=pg_sina_max_memory) ResetPlanCache();
}
static bool check_memory(int *value,void **extra,GucSource source)
{
    if(*value && *value<4*1024) {
        GUC_check_errdetail("Use 0 for automatic device capacity, or at least 4 MB."); return false;
    }
    return true;
}
static const struct config_enum_entry execution_paths[] = {
    {"host-only",PG_SINA_HOST,false}, {"filter-only",PG_SINA_FILTER,false},
    {"csd-without-hpcp",PG_SINA_CSD,false}, {"csd-with-hpcp",PG_SINA_HPCP,false},
    {"SQLerate",PG_SINA_ADAPTIVE,false}, {NULL, 0, false}
};
double pg_sina_host_cost_ms=0.02,pg_sina_csd_cpu_ratio=0.35;
double pg_sina_parallel_task_ms=0.02;
double pg_sina_internal_mbps=5000,pg_sina_pcie_mbps=3500,pg_sina_cache_mbps=10000;
double pg_sina_internal_write_mbps=1000;
double pg_sina_result_batch_ms=1;
static void assign_cost(double value,void *extra) { ResetPlanCache(); }
void _PG_init(void);
void _PG_init(void)
{
    DefineCustomEnumVariable("pg_sina_csd.exe_path", "Choose SQLerate placement and HPCP policy", NULL,
        &pg_sina_exe_path, 0, execution_paths, PGC_USERSET, 0, NULL, assign_path, NULL);
    DefineCustomBoolVariable("pg_sina_csd.profile", "Collect planner and device operator timings", NULL,
        &pg_sina_profile, false, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("pg_sina_csd.max_memory", "Maximum device work memory per query", NULL,
        &pg_sina_max_memory, 0, 0, MAX_KILOBYTES, PGC_USERSET, GUC_UNIT_KB, check_memory, assign_memory, NULL);
    DefineCustomRealVariable("pg_sina_csd.host_cost_ms","Milliseconds per PG cost unit",NULL,
        &pg_sina_host_cost_ms,0.02,0.000001,1000,PGC_USERSET,0,NULL,assign_cost,NULL);
    DefineCustomRealVariable("pg_sina_csd.csd_cpu_ratio","Device vector CPU cost relative to native PG before CU scaling",NULL,
        &pg_sina_csd_cpu_ratio,0.35,0.001,1000,PGC_USERSET,0,NULL,assign_cost,NULL);
    DefineCustomRealVariable("pg_sina_csd.parallel_task_ms","Scheduling cost per parallel task wave in milliseconds",NULL,
        &pg_sina_parallel_task_ms,0.02,0,1000,PGC_USERSET,0,NULL,assign_cost,NULL);
    DefineCustomRealVariable("pg_sina_csd.internal_mbps","Aggregate internal NVM/FDM bandwidth in decimal MB/s",NULL,
        &pg_sina_internal_mbps,5000,1,1000000,PGC_USERSET,0,NULL,assign_cost,NULL);
    DefineCustomRealVariable("pg_sina_csd.internal_write_mbps","Internal spill write bandwidth in decimal MB/s",NULL,
        &pg_sina_internal_write_mbps,1000,1,1000000,PGC_USERSET,0,NULL,assign_cost,NULL);
    DefineCustomRealVariable("pg_sina_csd.pcie_mbps","Shared PCIe bandwidth per direction in decimal MB/s",NULL,
        &pg_sina_pcie_mbps,3500,1,1000000,PGC_USERSET,0,NULL,assign_cost,NULL);
    DefineCustomRealVariable("pg_sina_csd.cache_mbps","Native host cached scan bandwidth in decimal MB/s",NULL,
        &pg_sina_cache_mbps,10000,1,1000000,PGC_USERSET,0,NULL,assign_cost,NULL);
    DefineCustomRealVariable("pg_sina_csd.result_batch_ms","Delivery/materialization cost per device result batch",NULL,
        &pg_sina_result_batch_ms,1,0,1000,PGC_USERSET,0,NULL,assign_cost,NULL);
    if(process_shared_preload_libraries_in_progress)
        elog(ERROR,"load pg_sina_csd per connection using session_preload_libraries, not shared_preload_libraries");
    if(!IsParallelWorker()) pg_sina_init_device();
    RegisterCustomScanMethods(&pg_sina_scan_methods);
    RegisterCustomScanMethods(&pg_sina_order_methods);
    RegisterCustomScanMethods(&pg_sina_plan_methods);
    RegisterCustomScanMethods(&pg_sina_adaptive_methods);
    previous_upper_hook=create_upper_paths_hook; create_upper_paths_hook=upper_hook;
    previous_planner_hook=planner_hook; planner_hook=hooked_planner;
    previous_rel_hook=set_rel_pathlist_hook; set_rel_pathlist_hook=rel_hook;
    previous_join_hook=set_join_pathlist_hook; set_join_pathlist_hook=join_hook;
}
PG_FUNCTION_INFO_V1(pg_sina_csd_version);
Datum pg_sina_csd_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pg_sina_csd 0.2: SNIA vector query execution"));
}
