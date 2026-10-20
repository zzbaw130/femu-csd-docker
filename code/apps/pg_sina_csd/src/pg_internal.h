#ifndef PG_SINA_INTERNAL_H
#define PG_SINA_INTERNAL_H
#include "postgres.h"
#include "nodes/extensible.h"
#include "query.h"
#include <sys/types.h>
extern int pg_sina_exe_path;
extern const CustomScanMethods pg_sina_scan_methods;
void pg_sina_upper_paths(PlannerInfo *, UpperRelationKind, RelOptInfo *, RelOptInfo *, void *);
struct pg_sina_worker { int fd; pid_t pid; };
struct qs_extent *pg_sina_mapping(struct qs_request *request);
void pg_sina_execute(struct qs_request *,struct qs_extent *,struct qs_result *,struct pg_sina_worker *);
void pg_sina_close(void *);
#endif
