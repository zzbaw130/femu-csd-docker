#ifndef PG_SINA_INDEX_SCAN_H
#define PG_SINA_INDEX_SCAN_H
#include "query.h"
struct femu_csd_args;
int qs_index_init(const struct qs_plan *,struct qs_join_control *);
/* Forward leaf walk, returning heap columns in index order. */
int64_t qs_index_scan(struct qs_chunk *,struct qs_task *,struct qs_join_control *,void *,unsigned,unsigned,int);
void qs_index_reset(struct qs_join_control *,unsigned);
/* Stop keys whose source-row bits were accepted by a semi/anti consumer. */
void qs_index_skip(struct qs_join_control *,unsigned,uint64_t);
void qs_index_close(struct qs_join_control *);
/* Borrow a currently retained scan AFDM; pages outside it use the index cache. */
void qs_index_heap_window(struct qs_join_control *,unsigned,const void *,uint32_t,uint32_t);
void qs_index_dictionary(struct qs_join_control *,struct qs_dict *);
int64_t qs_index_probe(unsigned,struct qs_chunk *,const struct qs_plan *,struct qs_task *,struct qs_join_control *,struct femu_csd_args *,int);
int64_t qs_join_consume(unsigned,struct qs_chunk *,const struct qs_plan *,struct qs_task *,struct qs_join_control *,struct femu_csd_args *,int);
int qs_scan_tuple(const struct qs_plan *,const unsigned char *,unsigned,unsigned,unsigned,struct qs_chunk *,unsigned,struct qs_dict *);
#endif
