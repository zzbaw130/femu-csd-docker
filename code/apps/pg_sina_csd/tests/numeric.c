/* Test-only PG entry point for the device-cell to native Numeric boundary. */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "catalog/pg_type_d.h"
#include "query.h"
PG_MODULE_MAGIC;
extern Datum pg_sina_decimal(struct qs_cell);
extern struct qs_cell pg_sina_parameter_cell(Datum,Oid,bool);
PG_FUNCTION_INFO_V1(pg_sina_numeric_test);
static void check_numeric(const char *text,unsigned scale,unsigned display,const char *expected)
{
    bool negative=*text=='-';
    if(negative) ++text;
    unsigned __int128 value=0;
    while(*text) { value=value*10+(*text-'0'); ++text; }
    struct qs_cell cell={.value=(__int128)(negative?0-value:value),
        .scale=scale,.dscale=display};
    char *actual=DatumGetCString(DirectFunctionCall1(numeric_out,pg_sina_decimal(cell)));
    if(strcmp(actual,expected)) elog(ERROR,"native Numeric mismatch: %s != %s",actual,expected);
}
Datum pg_sina_numeric_test(PG_FUNCTION_ARGS)
{
    const char *parameters[]={"0.0000","12345.6789012345678901","-12345.6789012345678901",
        "170141183460469231731687303715884105727","-170141183460469231731687303715884105728"};
    for(unsigned i=0;i<sizeof(parameters)/sizeof(*parameters);++i) {
        Datum numeric=DirectFunctionCall3(numeric_in,CStringGetDatum(parameters[i]),ObjectIdGetDatum(InvalidOid),Int32GetDatum(-1));
        struct qs_cell cell=pg_sina_parameter_cell(numeric,NUMERICOID,false);
        char *actual=DatumGetCString(DirectFunctionCall1(numeric_out,pg_sina_decimal(cell)));
        if(strcmp(actual,parameters[i])) elog(ERROR,"HOST parameter numeric mismatch: %s",actual);
    }
    if(!pg_sina_parameter_cell((Datum)0,NUMERICOID,true).isnull) elog(ERROR,"HOST NULL parameter lost");
    check_numeric("0",6,2,"0.00");
    check_numeric("-1",4,2,"0.00");
    check_numeric("10000",0,0,"10000");
    check_numeric("100000000",8,8,"1.00000000");
    check_numeric("12345",2,2,"123.45");
    check_numeric("-12345",2,2,"-123.45");
    check_numeric("1234500",4,2,"123.45");
    check_numeric("1234567",4,2,"123.45");
    check_numeric("12345",2,4,"123.45");
    check_numeric("1",7,7,"0.0000001");
    check_numeric("1",38,38,"0.00000000000000000000000000000000000001");
    check_numeric("170141183460469231731687303715884105727",0,0,"170141183460469231731687303715884105727");
    check_numeric("-170141183460469231731687303715884105728",1,1,"-17014118346046923173168730371588410572.8");
    check_numeric("170141183460469231731687303715884105727",38,38,"1.70141183460469231731687303715884105727");
    PG_RETURN_BOOL(true);
}
