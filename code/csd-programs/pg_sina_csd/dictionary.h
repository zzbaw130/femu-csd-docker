#ifndef PG_SINA_DICTIONARY_H
#define PG_SINA_DICTIONARY_H
#include "cs_workmem.h"
#include <stdint.h>

#define QS_DICTIONARY_PAGE_BYTES 65536U
/* Internal scheduling result, not an SNIA status or a completed lookup. */
#define QS_DICTIONARY_BUSY 1
struct qs_dictionary;
struct qs_dictionary_access;
struct qs_dictionary_text;
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
/* Give one unpinned cache page back to the shared query budget. Used when a
 * row operator's allocation fails while the dictionary still owns cache RAM. */
int64_t qs_dictionary_evict(struct qs_dictionary *,struct qs_dictionary_access *,
    void *io,uint64_t completion);
/* Read counters after the participating CUs have stopped dictionary calls. */
void qs_dictionary_get_stats(struct qs_dictionary *,struct qs_dictionary_stats *);
int qs_dictionary_valid_id(const struct qs_dictionary *,uint32_t id);
/* All accesses must be idle and unpinned before close; it frees every token,
 * then the dictionary's workmem child. Abort requires quiesced CUs; the owning
 * SNIA session must reclaim outstanding (including unacknowledged) tokens. */
int64_t qs_dictionary_close(struct qs_dictionary *,struct qs_dictionary_access *,
    void *io,uint64_t completion);
void qs_dictionary_abort(struct qs_dictionary *);
/* Private expression I/O state: at most two borrowed strings plus one
 * insertion. Retain the input/selection until the expression finishes.
 * Resume pending I/O before reevaluating; release after each completed text
 * operation. This keeps byte references valid without pinning a whole batch. */
struct qs_dictionary_text *qs_dictionary_text_create(struct qs_dictionary *);
int64_t qs_dictionary_text_resume(struct qs_dictionary_text *,void *io,uint64_t completion);
int64_t qs_dictionary_text_get(struct qs_dictionary_text *,uint32_t id,
    const char **bytes,unsigned *length,void *io);
int64_t qs_dictionary_text_insert(struct qs_dictionary_text *,const char *bytes,
    unsigned length,uint32_t *id,void *io);
int64_t qs_dictionary_text_request(const struct qs_dictionary_text *);
int qs_dictionary_text_release(struct qs_dictionary_text *);
#endif
