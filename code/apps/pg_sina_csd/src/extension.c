#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/plancache.h"
#include "optimizer/planner.h"
#include "pg_internal.h"
PG_MODULE_MAGIC;
int pg_sina_exe_path = 0;
bool pg_sina_profile = false;
int pg_sina_max_memory = 0; /* KiB; 0 uses the device workmem capacity. */
bool pg_sina_compiling_plan = false;
static create_upper_paths_hook_type previous_upper_hook;
static planner_hook_type previous_planner_hook;
static PlannedStmt *hooked_planner(Query *parse,int cursorOptions,ParamListInfo boundParams)
{
    Query *query=parse;
    if(pg_sina_exe_path==1) {
        Query *candidate=copyObject(parse);
        if(pg_sina_rewrite_query(candidate)) query=candidate;
    }
    PlannedStmt *plan=previous_planner_hook?previous_planner_hook(query,cursorOptions,boundParams):standard_planner(query,cursorOptions,boundParams);
    plan->planTree=pg_sina_native_plan(plan->planTree);
    return plan;
}
static void upper_hook(PlannerInfo *root,UpperRelationKind stage,RelOptInfo *input,RelOptInfo *output,void *extra)
{
    if(previous_upper_hook) previous_upper_hook(root,stage,input,output,extra);
    if(!pg_sina_compiling_plan) pg_sina_upper_paths(root,stage,input,output,extra);
}
static void assign_path(int value,void *extra)
{
    if(value!=pg_sina_exe_path) ResetPlanCache();
}
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
    {"host", 0, false}, {"csd", 1, false}, {NULL, 0, false}
};
void _PG_init(void);
void _PG_init(void)
{
    DefineCustomEnumVariable("pg_sina_csd.exe_path", "Choose native PG or supported CSD execution paths", NULL,
        &pg_sina_exe_path, 0, execution_paths, PGC_USERSET, 0, NULL, assign_path, NULL);
    DefineCustomBoolVariable("pg_sina_csd.profile", "Collect device operator timings for EXPLAIN ANALYZE", NULL,
        &pg_sina_profile, false, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("pg_sina_csd.max_memory", "Maximum device work memory per query", NULL,
        &pg_sina_max_memory, 0, 0, MAX_KILOBYTES, PGC_USERSET, GUC_UNIT_KB, check_memory, assign_memory, NULL);
    RegisterCustomScanMethods(&pg_sina_scan_methods);
    RegisterCustomScanMethods(&pg_sina_order_methods);
    RegisterCustomScanMethods(&pg_sina_plan_methods);
    previous_upper_hook=create_upper_paths_hook; create_upper_paths_hook=upper_hook;
    previous_planner_hook=planner_hook; planner_hook=hooked_planner;
}
PG_FUNCTION_INFO_V1(pg_sina_csd_version);
Datum pg_sina_csd_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pg_sina_csd 0.2: SNIA vector query execution"));
}
