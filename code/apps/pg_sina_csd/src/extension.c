#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(pg_sina_csd_version);
Datum pg_sina_csd_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pg_sina_csd 0.1: SNIA frozen-heap preparation"));
}
