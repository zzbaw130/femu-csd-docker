/* Query data exchanged by the PG backend and device CSFs.
 * These are application records, not extra SNIA arguments or a device runtime. */
#ifndef PG_SINA_QUERY_H
#define PG_SINA_QUERY_H
#include <stdint.h>
#include <stddef.h>
#define QS_JOINS 7
#define QS_COLS 64
#define QS_EXPRS 128
#define QS_AGGS 16
#define QS_KEYS 4
#define QS_OUT 16
#define QS_GROUPS 256
#define QS_RESULT_ROWS 256
#define QS_WORKERS 4
#define QS_PAGE 8192U
#define QS_CHUNK (4U*1024*1024)
#define QS_SORT_IO_BYTES (64U*1024)
#define QS_STRINGS 32
#define QS_INDEX_READS 64
#define QS_INDEX_CACHE 128
#define QS_INDEX_WORK_BYTES (128U*1024) /* Cursor/tag state; page input stays in AFDM. */
#define QS_INDEX_WAIT 1 /* Private join continuation; never a SNIA status. */
enum qs_type { Q_DECIMAL, Q_INTEGER, Q_DATE, Q_CHAR, Q_BOOLEAN, Q_STRING, Q_PREFIX, Q_DICTIONARY };
enum qs_op { Q_COLUMN, Q_CONST, Q_ADD, Q_SUB, Q_MUL, Q_EQ, Q_NE, Q_LT, Q_LE, Q_GT, Q_GE,
             Q_AND, Q_OR, Q_NOT, Q_IS_NULL, Q_IS_NOT_NULL, Q_CASE, Q_AGGREF, Q_DIV };
enum qs_agg_kind { Q_SUM, Q_AVG, Q_COUNT, Q_COUNT_STAR };
struct qs_column { int16_t length, slot; uint8_t align, type, scale, deferred, prefix_len, trim_spaces, restricted; char prefix[32]; };
/* SQL literal dictionary: IDs 1..nstrings; 0 means another non-NULL string.
 * Only a proven equality/IN restriction permits returning/grouping a string. */
struct qs_string { uint8_t column, length; char bytes[128]; };
struct qs_expr { int16_t left, right, column, otherwise; uint8_t op, type, scale, dscale, isnull; int64_t constant; };
struct qs_agg { int16_t expr; uint8_t kind; };
struct qs_output { int16_t index; uint8_t aggregate; uint32_t type_oid; int32_t typmod; };
struct qs_order { uint8_t output, descending, nulls_first; };
struct qs_join {
    uint32_t relid, relfilenode, rti;
    unsigned nattrs;
    int build_key, probe_key, predicate;
    int residual; /* Joined-row predicate index + 1; 0 means no residual. */
    uint64_t rows, work_mem_bytes;
    uint32_t indexid, indexnode, indexroot;
    uint8_t index_key_bytes; /* PG12 ascending integer B-tree leading key. */
};
struct qs_plan {
    uint32_t relid, relfilenode;
    unsigned nattrs, ncols, nexprs, naggs, nkeys, nout, norder, nstrings;
    int predicate;
    unsigned ordered_rows; /* 1: SQL row sort; 2: internal full-row input. */
    unsigned external_aggregate;
    struct qs_join join; /* First build; its work_mem_bytes is the total build reservation. */
    unsigned njoins;
    struct qs_join extra_joins[QS_JOINS-1];
    uint64_t work_mem_bytes; /* Per execution slot, set at execution; 0 uses device capacity. */
    int64_t limit, offset;
    struct qs_column cols[QS_COLS];
    struct qs_string strings[QS_STRINGS];
    struct qs_expr exprs[QS_EXPRS];
    struct qs_agg aggs[QS_AGGS];
    int keys[QS_KEYS];
    struct qs_output outputs[QS_OUT];
    struct qs_order order[QS_OUT];
};
static inline unsigned qs_join_count(const struct qs_plan *p)
{ return p->njoins?p->njoins:p->join.relid?1:0; }
static inline const struct qs_join *qs_join_at(const struct qs_plan *p,unsigned i)
{ return i?&p->extra_joins[i-1]:&p->join; }
static inline unsigned qs_probe_attrs(const struct qs_plan *p)
{
    unsigned n=p->nattrs;
    for(unsigned i=0;i<qs_join_count(p);++i) n-=qs_join_at(p,i)->nattrs;
    return n;
}
struct qs_extent { uint64_t logical, lba, bytes; };
struct qs_request { struct qs_plan plan; char device[128]; uint32_t nsid, lba_bytes, nextents; uint64_t bytes; };
struct qs_index_read { uint32_t block; uint16_t slot, heap; };
struct qs_task {
    uint64_t state;
    uint32_t input_bytes, workers, slot; /* slot is a PG-assigned input index. */
    uint64_t scanned, matched, joined;
    uint64_t index_pages, index_tuples;
    uint32_t index_pending, index_nreads;
    struct qs_index_read index_reads[QS_INDEX_READS];
};
struct qs_join_control {
    uint64_t state, rows, spill_write_bytes, spill_read_bytes;
    uint32_t batches, current, indexed;
};
struct qs_group { uint64_t states[QS_WORKERS]; unsigned count; };
/* For a deferred numeric column, Scan stores this raw AFDM payload location
 * in its 8-byte value slot. Filter decodes only selected rows before Aggregate.
 * No device pointer escapes to the PG backend. */
struct qs_heap_ref { uint32_t offset, bytes; };
/* Column values, NULL bitmaps and numeric display scales, then selection. */
struct qs_chunk { uint32_t capacity, ncols, rows, selected; };
static inline size_t qs_chunk_bytes(unsigned capacity, unsigned ncols)
{ return sizeof(struct qs_chunk)+(size_t)capacity*ncols*8+((capacity+7)/8)*(size_t)ncols+(size_t)capacity*ncols+7+(size_t)capacity*4; }
static inline size_t qs_join_chunk_stride(const struct qs_plan *p)
{ return (qs_chunk_bytes(256,p->ncols)+7)&~(size_t)7; }
static inline int64_t *qs_values(struct qs_chunk *c) { return (int64_t *)(c+1); }
static inline unsigned char *qs_nulls(struct qs_chunk *c) { return (unsigned char *)(qs_values(c)+(size_t)c->capacity*c->ncols); }
static inline unsigned char *qs_dscales(struct qs_chunk *c) { return qs_nulls(c)+((c->capacity+7)/8)*(size_t)c->ncols; }
static inline uint32_t *qs_selection(struct qs_chunk *c)
{ return (uint32_t *)(((uintptr_t)(qs_dscales(c)+(size_t)c->capacity*c->ncols)+7)&~(uintptr_t)7); }
struct qs_cell { __int128 value; uint8_t scale, dscale, isnull; };
struct qs_row { struct qs_cell cells[QS_OUT]; };
struct qs_metrics {
    uint64_t scanned, matched, input_bytes, chunks, work_peak, fdm_bytes;
    uint64_t spill_write_bytes, spill_read_bytes, runs, spilled_runs;
    uint64_t build_rows, join_rows, build_batches;
    uint64_t index_pages, index_tuples;
    unsigned workers;
};
/* Optional per-query measurements. Each CU owns its own AFDM counters. */
enum qs_operator { QS_SCAN, QS_FILTER, QS_AGG_SINK, QS_AGG_SOURCE, QS_SORT, QS_LIMIT,
                   QS_SORT_SINK, QS_SORT_SOURCE, QS_JOIN_BUILD, QS_JOIN_LOAD, QS_JOIN_PROBE, QS_OPERATORS };
struct qs_timing { uint64_t calls, wall_ns, cpu_ns, max_wall_ns; };
struct qs_profile {
    struct qs_timing operators[QS_OPERATORS];
    uint64_t setup_ns, build_ns, pipeline_ns[2], cleanup_ns, total_ns;
};
struct qs_result { int status; unsigned rows, begin, finished; char error[160]; struct qs_metrics metrics; struct qs_profile profile; struct qs_row data[QS_RESULT_ROWS]; };
#endif
