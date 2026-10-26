#ifndef PG_SINA_DICTIONARY_H
#define PG_SINA_DICTIONARY_H
#include "cs_workmem.h"
#include <stdint.h>

#define QS_DICTIONARY_PAGE_BYTES 65536U
/* Internal scheduling result, not an SNIA status or a completed lookup. */
#define QS_DICTIONARY_BUSY 1
struct qs_dictionary;
struct qs_dictionary_access;
struct qs_dictionary_stats {
    uint64_t strings, bytes, stores, loads, metadata_stores, metadata_loads;
};
struct qs_dictionary *qs_dictionary_create(cs_work_context *parent);
struct qs_dictionary_access *qs_dictionary_access_create(struct qs_dictionary *dict);
/* Repeat the same operation after each NEED_* completion, passing its token.
 * The input key and access must survive until insert completes. Each access
 * uses a private 64 KiB I/O AFDM. Zero means success; negative means failure.
 * BUSY means another CU owns an unfinished dictionary operation: retry later.
 * Successful pin keeps bytes valid until unpin, including across insert/I/O. */
int64_t qs_dictionary_insert(struct qs_dictionary *,struct qs_dictionary_access *,
    const char *key,unsigned length,uint32_t *id,void *io,uint64_t completion);
int64_t qs_dictionary_pin(struct qs_dictionary *,struct qs_dictionary_access *,
    uint32_t id,const char **bytes,unsigned *length,void *io,uint64_t completion);
void qs_dictionary_unpin(struct qs_dictionary_access *);
/* Read counters after the participating CUs have stopped dictionary calls. */
void qs_dictionary_get_stats(struct qs_dictionary *,struct qs_dictionary_stats *);
/* All accesses must be idle and unpinned before close; it frees every token,
 * then the dictionary's workmem child. Abort requires quiesced CUs; the owning
 * SNIA session must reclaim outstanding (including unacknowledged) tokens. */
int64_t qs_dictionary_close(struct qs_dictionary *,struct qs_dictionary_access *,
    void *io,uint64_t completion);
void qs_dictionary_abort(struct qs_dictionary *);
#endif
