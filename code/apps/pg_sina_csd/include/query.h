/* Query data exchanged by the PG backend and device CSFs.
 * These are application records, not extra SNIA arguments or a device runtime. */
#ifndef PG_SINA_QUERY_H
#define PG_SINA_QUERY_H
#include <stdint.h>
#include <stddef.h>
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
enum qs_type { Q_DECIMAL, Q_INTEGER, Q_DATE, Q_CHAR, Q_BOOLEAN, Q_STRING, Q_PREFIX };
enum qs_op { Q_COLUMN, Q_CONST, Q_ADD, Q_SUB, Q_MUL, Q_EQ, Q_NE, Q_LT, Q_LE, Q_GT, Q_GE,
             Q_AND, Q_OR, Q_NOT, Q_IS_NULL, Q_IS_NOT_NULL, Q_CASE, Q_AGGREF, Q_DIV };
enum qs_agg_kind { Q_SUM, Q_AVG, Q_COUNT, Q_COUNT_STAR };
struct qs_column { int16_t length, slot; uint8_t align, type, scale, deferred, prefix_len; char prefix[32]; };
struct qs_expr { int16_t left, right, column, otherwise; uint8_t op, type, scale, dscale, isnull; int64_t constant; };
struct qs_agg { int16_t expr; uint8_t kind; };
struct qs_output { int16_t index; uint8_t aggregate; uint32_t type_oid; int32_t typmod; };
struct qs_order { uint8_t output, descending, nulls_first; };
struct qs_join {
    uint32_t relid, relfilenode, rti;
    unsigned nattrs;
    int build_key, probe_key, predicate;
    uint64_t rows, work_mem_bytes;
};
struct qs_plan {
    uint32_t relid, relfilenode;
    unsigned nattrs, ncols, nexprs, naggs, nkeys, nout, norder;
    int predicate;
    unsigned ordered_rows; /* 1: SQL row sort; 2: internal full-row input. */
    unsigned external_aggregate;
    struct qs_join join;
    uint64_t work_mem_bytes; /* Per execution slot, set at execution; 0 uses device capacity. */
    int64_t limit, offset;
    struct qs_column cols[QS_COLS];
    struct qs_expr exprs[QS_EXPRS];
    struct qs_agg aggs[QS_AGGS];
    int keys[QS_KEYS];
    struct qs_output outputs[QS_OUT];
    struct qs_order order[QS_OUT];
};
struct qs_extent { uint64_t logical, lba, bytes; };
struct qs_request { struct qs_plan plan; char device[128]; uint32_t nsid, lba_bytes, nextents; uint64_t bytes; };
struct qs_task { uint64_t state; uint32_t input_bytes, workers; uint64_t scanned, matched, joined; };
struct qs_join_control {
    uint64_t state, rows, spill_write_bytes, spill_read_bytes;
    uint32_t batches, current;
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
