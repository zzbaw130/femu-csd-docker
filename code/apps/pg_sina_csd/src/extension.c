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
PG_MODULE_MAGIC;
int pg_sina_exe_path = 0;
bool pg_sina_profile = false;
int pg_sina_max_memory = 0; /* KiB; 0 uses the device workmem capacity. */
bool pg_sina_compiling_plan = false;
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
    Query *volatile query=parse;
    if(pg_sina_exe_path>=PG_SINA_CSD) {
        Query *candidate=copyObject(parse);
        if(pg_sina_rewrite_query(candidate)) query=candidate;
    }
    struct pg_sina_path_search *saved=pg_sina_path_search;
    bool native_search=pg_sina_native_search;
    PlannedStmt *volatile plan=NULL;
    PG_TRY();
    {
        PlannedStmt *host=NULL,*original_device=NULL; double host_cost=0;
        if(pg_sina_exe_path==PG_SINA_ADAPTIVE) {
            pg_sina_start_path_search();
            pg_sina_native_search=true;
            host=plan_query(copyObject(parse),cursorOptions,boundParams);
            original_device=pg_sina_original_device_statement(host);
            host_cost=pg_sina_host_statement_cost(host);
            pg_sina_start_path_search();
            pg_sina_native_search=false;
        }
        plan=plan_query(query,cursorOptions,boundParams);
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
        }
    }
    PG_CATCH();
    { pg_sina_path_search=saved; pg_sina_native_search=native_search; PG_RE_THROW(); }
    PG_END_TRY();
    pg_sina_path_search=saved; pg_sina_native_search=native_search;
    return plan;
}
static void upper_hook(PlannerInfo *root,UpperRelationKind stage,RelOptInfo *input,RelOptInfo *output,void *extra)
{
    if(previous_upper_hook) previous_upper_hook(root,stage,input,output,extra);
    if(!pg_sina_compiling_plan) {
        if(pg_sina_exe_path==PG_SINA_ADAPTIVE) pg_sina_search_upper_paths(root,stage,input,output);
        else pg_sina_upper_paths(root,stage,input,output,extra);
    }
}
static void rel_hook(PlannerInfo *root,RelOptInfo *rel,Index rti,RangeTblEntry *rte)
{
    if(previous_rel_hook) previous_rel_hook(root,rel,rti,rte);
    pg_sina_rel_paths(root,rel,rti,rte);
}
static void join_hook(PlannerInfo *root,RelOptInfo *joinrel,RelOptInfo *outerrel,RelOptInfo *innerrel,
                      JoinType jointype,JoinPathExtraData *extra)
{
    if(previous_join_hook) previous_join_hook(root,joinrel,outerrel,innerrel,jointype,extra);
    pg_sina_join_paths(root,joinrel,outerrel,innerrel,jointype,extra);
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
    DefineCustomBoolVariable("pg_sina_csd.profile", "Collect device operator timings for EXPLAIN ANALYZE", NULL,
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
