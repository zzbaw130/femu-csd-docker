#ifndef PG_SINA_SCAN_H
#define PG_SINA_SCAN_H
#include "dictionary.h"
struct femu_csd_args;
struct qs_scan_cursor;
/* Private native-plan scan composition. Unlike the flat-dictionary CSF,
 * this cursor retains the current heap batch/column through dictionary I/O.
 * Input pages and destination columns must stay alive until completion. */
struct qs_scan_cursor *qs_scan_cursor_create(cs_work_context *,struct qs_dictionary *);
int64_t qs_scan_columns(struct qs_scan_cursor *,struct femu_csd_args *,uint64_t completion);
#endif
