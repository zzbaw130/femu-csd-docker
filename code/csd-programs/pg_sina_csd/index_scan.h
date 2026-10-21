#ifndef PG_SINA_INDEX_SCAN_H
#define PG_SINA_INDEX_SCAN_H
#include "query.h"
struct femu_csd_args;
int qs_index_init(const struct qs_plan *,struct qs_join_control *);
void qs_index_reset(struct qs_join_control *,unsigned);
void qs_index_close(struct qs_join_control *);
int64_t qs_index_probe(unsigned,struct qs_chunk *,const struct qs_plan *,struct qs_task *,struct qs_join_control *,struct femu_csd_args *,int);
int64_t qs_join_consume(unsigned,struct qs_chunk *,const struct qs_plan *,struct qs_task *,struct qs_join_control *,struct femu_csd_args *,int);
int qs_scan_tuple(const struct qs_plan *,const unsigned char *,unsigned,unsigned,unsigned,struct qs_chunk *,unsigned,struct qs_dict *);
#endif
