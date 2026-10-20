#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
PG_MODULE_MAGIC;
int pg_sina_exe_path = 0;
static const struct config_enum_entry execution_paths[] = {
    {"host", 0, false}, {"csd", 1, false}, {NULL, 0, false}
};
void _PG_init(void);
void _PG_init(void)
{
    DefineCustomEnumVariable("pg_sina_csd.exe_path", "Choose native PG or supported CSD execution paths", NULL,
        &pg_sina_exe_path, 0, execution_paths, PGC_USERSET, 0, NULL, NULL, NULL);
}
PG_FUNCTION_INFO_V1(pg_sina_csd_version);
Datum pg_sina_csd_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pg_sina_csd 0.1: SNIA frozen-heap preparation"));
}
