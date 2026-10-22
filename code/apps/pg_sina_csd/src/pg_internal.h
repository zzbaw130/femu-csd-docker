#ifndef PG_SINA_INTERNAL_H
#define PG_SINA_INTERNAL_H
#include "postgres.h"
#include "nodes/extensible.h"
#include "query.h"
extern int pg_sina_exe_path;
extern bool pg_sina_profile;
extern int pg_sina_max_memory;
extern const CustomScanMethods pg_sina_scan_methods;
extern const CustomScanMethods pg_sina_order_methods;
struct pg_sina_stream;
void pg_sina_upper_paths(PlannerInfo *, UpperRelationKind, RelOptInfo *, RelOptInfo *, void *);
bool pg_sina_rewrite_query(Query *q);
struct qs_extent *pg_sina_mapping(struct qs_request *request);
void pg_sina_execute(struct qs_request *,struct qs_extent *,struct qs_result *,bool,struct pg_sina_stream **);
void pg_sina_fetch(struct pg_sina_stream **,struct qs_result *);
void pg_sina_close(struct pg_sina_stream *);
#endif
