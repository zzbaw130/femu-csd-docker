#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/plancache.h"
#include "optimizer/planner.h"
#include "pg_internal.h"
PG_MODULE_MAGIC;
int pg_sina_exe_path = 0;
static create_upper_paths_hook_type previous_upper_hook;
static void upper_hook(PlannerInfo *root,UpperRelationKind stage,RelOptInfo *input,RelOptInfo *output,void *extra)
{
    if(previous_upper_hook) previous_upper_hook(root,stage,input,output,extra);
    pg_sina_upper_paths(root,stage,input,output,extra);
}
static void assign_path(int value,void *extra)
{
    if(value!=pg_sina_exe_path) ResetPlanCache();
}
static const struct config_enum_entry execution_paths[] = {
    {"host", 0, false}, {"csd", 1, false}, {NULL, 0, false}
};
void _PG_init(void);
void _PG_init(void)
{
    DefineCustomEnumVariable("pg_sina_csd.exe_path", "Choose native PG or supported CSD execution paths", NULL,
        &pg_sina_exe_path, 0, execution_paths, PGC_USERSET, 0, NULL, assign_path, NULL);
    RegisterCustomScanMethods(&pg_sina_scan_methods);
    previous_upper_hook=create_upper_paths_hook; create_upper_paths_hook=upper_hook;
}
PG_FUNCTION_INFO_V1(pg_sina_csd_version);
Datum pg_sina_csd_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pg_sina_csd 0.1: SNIA vector query execution"));
}
