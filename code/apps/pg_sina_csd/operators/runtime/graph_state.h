#ifndef PG_SINA_GRAPH_STATE_H
#define PG_SINA_GRAPH_STATE_H

#include "cs_csf.h"
#include "cs_workmem.h"
#include "plan.h"
#include "query_vector.h"
#include "index_scan.h"
#include "scan.h"

#include <limits.h>
#include <stdatomic.h>
#include <time.h>

#define QP_WAIT 1
#define QP_BATCH_END 4
enum { GROUP_LOCAL, GROUP_SHARED, GROUP_PARTITION };
#define GROUP_TAG_MASK 0x7fff0000U
#define GROUP_LOCK (1U<<31)

struct execution;
struct scan_packet;
struct projection_batch;
typedef int64_t (*graph_projection_reader)(struct execution *,struct projection_batch *);
typedef void (*graph_query_initializer)(struct execution *,size_t);
struct graph_projection_ops {
    int (*prepare)(struct execution *,unsigned,int);
    int (*retry)(struct execution *,unsigned,struct qs_cell *);
};
extern const struct graph_projection_ops graph_project_ops;
struct sorted_rows {
    void (*close)(struct sorted_rows *);
    const char *(*lookup_text)(const struct qs_result *,uint32_t,unsigned *);
    struct qs_plan plan;
    struct qs_task task;
    struct qs_group group;
    struct qs_result *input,*output;
    const struct qs_result *batch; /* Borrowed CU output, retained across spill. */
    unsigned batch_worker;
    struct projection_batch *columns; /* Borrowed producer vectors, retained across spill. */
    unsigned position;
    int initialized, complete, drained, input_pending, producer_status,finishing_sink,sink_finished;
    unsigned char *prefix;
    unsigned prefix_rows,prefix_position;
    struct qs_metrics metrics;
};
static inline void graph_close_sort(struct sorted_rows *sort)
{
    if(sort->close) sort->close(sort);
}
struct spool_block {
    uint64_t token;
    unsigned char *data;
};
struct spool {
    unsigned char *tail,*cache;
    struct spool_block *pages;
    unsigned blocks, capacity, used, loaded, block_bytes;
    uint64_t rows;
};
static inline unsigned spool_block_bytes(const struct spool *p)
{ return p->block_bytes?p->block_bytes:QS_SORT_IO_BYTES; }

/* Each Sink owns its partition streams; a Source claims a complete partition
 * across all Sinks. Equal keys therefore never need a shared update lock. */
struct radix_group {
    struct spool *streams;
    unsigned partitions,workers,sources;
    size_t resident_reserve;
    atomic_uint next;
    atomic_size_t source_growth;
    bool bounded_source;
    struct operator_state *serial;
};
#define HASH_PARTITIONS 4
#define HASH_RELOAD_BLOCK_ROWS 32768U
/* Bounded radix partitions use the existing SNIA spill protocol. A skewed
 * partition is probed in build batches, without sorting or rereading Heap. */
struct external_hash {
    struct spool build[HASH_PARTITIONS],probe[HASH_PARTITIONS];
    unsigned char *prefix;
    uint64_t prefix_rows,prefix_at,build_at,probe_at;
    atomic_uint_fast64_t prefix_next;
    int prefix_parallel,partition_attempted,probe_parallel,probe_parallel_saved;
    unsigned phase,partition,direct;
    unsigned cleanup_partition;
};
struct pending_projection {
    struct qs_cell input[QS_COLS],aggregates[QS_AGGS];
    int has_input,has_aggregates,filtered,inline_only;
    struct qs_eval evaluation; /* Completed subexpressions survive dictionary I/O. */
};
struct join_condition_state {
    struct qs_eval evaluation;
    struct qs_cell input[QS_COLS];
    int active;
};
struct accumulator { struct qs_cell sum,extreme,last; uint64_t count; int has_last; };
/* Scalar aggregate arguments must finish before any accumulator changes. */
struct aggregate_input {
    struct qs_eval evaluation;
    struct qs_cell values[QS_AGGS];
    unsigned argument,aggregate;
    int active,ready;
};
/* sum holds the total for SUM/AVG, or the selected value for MIN/MAX. */
struct group_value { struct qs_cell sum; uint64_t count; };
struct compact_group { int64_t key; unsigned key_flags; struct group_value values[]; };
/* Hash slots publish indices into dense records, not whole aggregate states. */
struct group_entry { atomic_uint state; unsigned row; };
/* One unfinished hash update per CU. Never retain a bucket lock across I/O.
 * The input and old/new values stay here until the complete update commits. */
struct group_update_state {
    struct operator_state *owner;
    struct group_entry *entry;
    struct compact_group *group;
    struct group_value before[QS_AGGS],after[QS_AGGS],input[QS_AGGS];
    uint64_t hash;
    unsigned node,aggregate,occupied;
    int active,shared;
};
struct group_batch_input {
    struct group_value values[QS_AGGS];
    struct qs_cell keys[QS_KEYS];
    unsigned end,aggregate;
    int phase;
};
struct set_key { int64_t value; unsigned occupied; };
/* A spilled set is immutable after initialization. Each consumer owns its
 * read cache and the active expression batch, while tokens/tail are shared. */
struct set_probe {
    struct set_probe *next;
    const struct qs_expr *expression;
    struct spool pages;
    struct qs_cell keys[QS_VECTOR_ROWS],values[QS_VECTOR_ROWS];
    unsigned rows[QS_VECTOR_ROWS],count,block;
    int ready;
};
/* Private per-CU expression state; referenced input columns stay in AFDM. */
struct projection_batch {
    int (*finish_vectors)(struct execution *,struct projection_batch *);
    struct qs_plan plan;
    graph_projection_reader read_input;
    struct qs_vector *vectors;
    uint8_t needed[QS_EXPRS];
    unsigned scan,count,position,input_count;
    unsigned aggregate_position,aggregate_row;
    struct group_batch_input *group_input;
    struct qs_cell *cells;
    const struct qs_cell *borrowed_cells;
    const uint32_t *borrowed_selection;
    int64_t error;
    unsigned error_node;
    int joined,sorted,group_source,keep_window,lazy_text,filtered_scan,input_edge;
    int retry,materializing;
    unsigned *input_cursor; /* Advance the producer only after evaluation succeeds. */
    bool stream_input_drained;
    unsigned joined_row,joined_column;
    unsigned materialized_column;
    unsigned deferred_numbers; /* Probe payload output slots, never join keys. */
    struct qs_vector_input retry_input;
    struct qs_chunk *retry_chunk;
    const uint32_t *retry_selection;
    unsigned retry_first,retry_count;
    int group_keys[QS_KEYS],group_values[QS_AGGS];
    struct { unsigned node; int expression; } filters[QP_NODES];
    unsigned nfilters;
};
static inline int projection_pending(const struct projection_batch *b)
{ return b && (b->position<b->count || b->input_count || b->error); }
struct hash_probe_batch {
    uint32_t matches[QS_VECTOR_ROWS];
    uint8_t errors[QS_VECTOR_ROWS];
    int ready;
};
struct hash_qual_batch {
    struct projection_batch expr;
    uint32_t rows[QS_VECTOR_ROWS];
    uint32_t cursor;
};
struct merge_qual_batch {
    struct projection_batch expr;
    uint64_t begin;
};
struct index_qual_batch {
    struct projection_batch expr;
    int status[QS_VECTOR_ROWS];
    unsigned preparing,prepared;
    uint32_t selection[QS_VECTOR_ROWS];
};
struct operator_state {
    graph_projection_reader read_projection;
    struct projection_batch **published_batch;
    int64_t (*read_row)(struct execution *,unsigned,struct qs_cell *);
    int64_t (*read_chunk)(struct execution *,unsigned);
    int64_t (*decode_chunk)(struct execution *,unsigned,unsigned,bool);
    void (*close)(struct operator_state *);
    void (*metrics)(struct operator_state *,struct qs_metrics *,struct qs_profile *);
    int (*admit_source)(const struct execution *,unsigned,bool);
    size_t source_workspace; /* bytes required while a published Source is active */
    unsigned source_workers;
    void (*start_source)(struct execution *,unsigned);
    void (*finish_source)(struct execution *,unsigned);
    void (*finish_wave)(struct execution *,unsigned);
    struct sorted_rows left_sort,right_sort;
    struct qs_cell left[QS_OUT],right[QS_OUT],first[QS_OUT],keys[QS_KEYS];
    struct accumulator accum[QS_AGGS];
    struct aggregate_input *aggregate_input;
    unsigned aggregate_position;
    struct spool group;
    struct spool *published_rows;
    uint64_t position, skipped, emitted;
    unsigned scan_position, scan_input, scan_bytes,scan_raw,scan_raw_bytes;
    struct qs_scan_cursor *scan_cursor;
    int defer_strings;
    struct qs_plan *scan_filter;
    const uint32_t *scan_hash_buckets;
    unsigned scan_hash_mask,scan_hash_slot;
    bool scan_hash_checked;
    struct qs_dict *filter_dictionary; /* Immutable literals for raw-Heap filtering. */
    int scan_filter_checked,projection_checked;
    struct projection_batch *projection, *block_input_projection;
    struct projection_batch *source_projection;
    int source_checked;
    unsigned projection_deferred,source_deferred;
    int phase, have_left, have_right, input_done, group_ready, matched, finished;
    struct join_condition_state *join_condition;
    unsigned merge_key;
    unsigned merge_ordered; /* left/right input order verified; bit 2 marks checked. */
    struct merge_qual_batch *merge_qual;
    int merge_qual_checked;
    struct qs_plan *index_plan;
    struct projection_batch *index_projection;
    struct index_qual_batch *index_qual;
    int index_qual_checked;
    struct qs_join_control index_control;
    struct qs_task index_task;
    struct qs_result *index_output;
    struct qs_cell *index_left;
    uint64_t index_matches[(QS_INDEX_KEYS+63)/64];
    unsigned index_count, index_unmatched;
    unsigned index_position;
    int index_resume, index_flush,index_active,index_projected;
    unsigned index_consume_position;
    unsigned char *hash_data;
    unsigned char **hash_blocks; /* Direct reload fallback for fragmented workmem. */
    atomic_uint hash_rows;
    unsigned hash_capacity,hash_match,hash_phase,hash_buckets,hash_cache_slots;
    bool hash_defer_index;
    uint64_t *hash_cache;
    int hash_matched,hash_fallback,hash_shared,hash_null_right,hash_output_pending;
    unsigned hash_payload; /* Columns still borrowed from the probe vector. */
    struct hash_probe_batch *hash_probe;
    unsigned graph_input_position, graph_probe_first, graph_probe_count;
    bool graph_input_held, graph_wave_input, graph_wave_drained, stream_inlet;
    struct hash_qual_batch *hash_qual;
    int hash_qual_checked,hash_qual_rejected;
    struct external_hash *external;
    struct compact_group *groups;
    struct group_entry *group_entries;
    unsigned group_capacity,group_flush,group_phase,group_stride,group_integer;
    atomic_uint *group_count;
    struct operator_state *group_local,**group_locals;
    unsigned group_local_count,group_local_slot,group_local_row;
    struct radix_group *group_radix;
    unsigned radix_partition,radix_writer;
    uint64_t radix_row;
    int radix_source;
    int group_partial;
    unsigned group_batch_kept;
    struct set_key *set;
    unsigned set_capacity,set_count;
    int set_null,set_ready;
    struct spool set_pages;
    unsigned set_convert;
    int set_external;
    struct set_probe *set_probe;
    int64_t (*lookup_spilled_set)(struct execution *,unsigned,const struct qs_expr *,
        const struct qs_cell *,const unsigned *,unsigned,struct qs_cell **);
    struct pending_projection *project_retry;
};
struct output_string { uint32_t id; uint16_t row,column; };
enum graph_emit_kind { GRAPH_EMIT_SOURCE, GRAPH_EMIT_PROBE, GRAPH_EMIT_FEED, GRAPH_EMIT_SORT, GRAPH_EMIT_AGGREGATE };
struct execution {
    const struct graph_projection_ops *projection_ops;
    int64_t (*read_ordered_index)(struct execution *,unsigned);
    cs_work_context *context;
    struct qp_plan *plan;
    struct qp_control *control;
    struct qs_dict *dictionary;
    struct qs_dictionary *paged;
    struct qs_index_cache *index_cache;
    struct qs_dictionary_access *initial_access;
    unsigned *literal_ids;
    unsigned imported,literal_node,literal_string;
    int dictionary_ready,operators_closed,closing,buffered;
    unsigned buffered_rows,buffered_finished;
    int64_t initial_wait,dictionary_wait;
    struct qs_dictionary_text *text;
    struct group_update_state *group_update;
    int64_t text_wait;
    unsigned output_column;
    int output_pending;
    unsigned host_building,host_pending,host_column,host_var_bytes;
    unsigned host_source_rows,host_source_worker;
    const struct qs_cell *host_source;
    unsigned char *host_columns[QS_OUT],*host_nulls[QS_OUT];
    unsigned host_widths[QS_OUT];
    int raw_projection;
    struct qs_cell host_row[QS_OUT];
    uint64_t host_values[QS_OUT];
    unsigned char host_var[QS_OUT*264];
    struct output_string *output_order;
    unsigned output_count,output_position,output_exported;
    int output_active,output_status;
    struct operator_state *nodes;
    const struct qs_result *hash_input;
    unsigned hash_input_node, hash_input_position;
    unsigned graph_source_node;
    enum graph_emit_kind graph_source_kind;
    struct projection_batch *stream_edge;
    unsigned stream_edge_node;
    bool graph_source_pending;
    /* One scalar projection can wait per CU. Reserve its continuation before
     * the dictionary cache fills the query budget, rather than during a miss. */
    struct pending_projection projection_retry;
    struct qs_cell params[QP_PARAMS];
    unsigned char *arena;
    void *io;
    size_t io_bytes, sort_limit;
    struct sorted_rows *pending_sort;
    struct sorted_rows *sort_sink; /* CU-local state; runs are owned by the query. */
    bool block_graph, block_chain, block_row_pending, block_done;
    bool resident_dictionary,stream_waves,stream_block;
    bool stream_scanning,stream_filter_retry,stream_source_window;
    unsigned stream_bytes,stream_offset;
    bool block_sink_started, block_sink_done;
    unsigned block_write, block_input_node, block_native_deferred;
    int block_native_checked;
    struct projection_batch *block_native_projection;
    struct spool block_edges[2], *block_input;
    uint64_t block_edge_read;
    struct qs_cell block_row[QS_OUT];
    struct spool block_rows, block_overflow;
    struct qs_cell *block_cells;
    uint64_t block_read, block_overflow_read;
    struct spool *pending_spool;
    unsigned pending_block;
    int pending_store;
    int expression_wait;
    unsigned pending_nvm;
    int64_t late_wait;
    unsigned late_node,late_valid;
    uint64_t late_block;
    unsigned char *late_page; /* One private, charged page; never borrowed FDM. */
    struct qs_metrics metrics;
    struct qs_profile profile;
    uint64_t local_profile_cpu, pipeline_start, pipeline_tick;
    unsigned group_source_profiling;
    uint64_t wait_started,wait_ns[QP_WAITS],wait_calls[QP_WAITS],finished_ns;
    unsigned wait_kind;
    uint64_t sink_ns,sink_rows;
    bool profiling, host_scan, scan_drained;
    struct femu_csd_args *args;
    unsigned initializer,initializer_phase;
    unsigned worker, parallel_position[QS_WORKERS];
    uint64_t parallel_done;
    int parallel_started,parallel_combining;
    unsigned parallel_combine_worker;
    unsigned parallel_node, root, used_workers;
    int wave_source;
    struct qs_chunk *published_chunk;
    struct scan_packet *scan_packets, *scan_packets_tail;
    bool scan_output_finished;
    /* One-based parent receiving rows directly from parallel workers. */
    unsigned hash_sink,group_sink,aggregate_sink,group_combine,set_sink;
    uint64_t sink_returned_rows;
    unsigned partition_sink,partition_merged,partition_merge_row;
    uint64_t partition_prefix_at,partition_prefix_end;
    struct spool *partitions;
    bool group_shard,group_merge_full;
    atomic_uint_fast64_t next_offset;
    atomic_uint active_cus, peak_cus;
    struct execution *owner, *workers[QS_WORKERS];
    struct qs_result *outputs;
    int partition_join,source_node;
    struct spool source;
    uint64_t source_at,source_end;
};

/* Published state stays with the query. Cursors stay in the worker. */
static inline const struct operator_state *graph_state_owner(const struct execution *s,unsigned id)
{ return &(s->owner?s->owner:s)->nodes[id]; }

struct blocking_timer { uint64_t wall,cpu,children; };

struct hash_key_layout { unsigned count,offsets[QS_KEYS],widths[QS_KEYS]; };

static inline unsigned hash_value_bytes(const struct qs_plan *p,unsigned column)
{
    switch(p->outputs[column].type_oid) {
    case 16:case 21:case 23:case 1082:return 4;
    case 20:return 8;
    default:return 16;
    }
}

static inline unsigned hash_row_bytes(const struct qs_plan *p)
{
    unsigned bytes=0; for(unsigned i=0;i<p->nout;++i) {
        unsigned value=hash_value_bytes(p,i); bytes+=value+(value<16?1:3);
    }
    return bytes;
}

static inline void hash_load(struct qs_cell *to,const unsigned char *from,const struct qs_plan *p)
{
    for(unsigned i=0;i<p->nout;++i) {
        unsigned bytes=hash_value_bytes(p,i);
        if(bytes==4) { int32_t v; memcpy(&v,from,4); to[i].value=v; }
        else if(bytes==8) { int64_t v; memcpy(&v,from,8); to[i].value=v; }
        else memcpy(&to[i].value,from,16);
        if(bytes<16) { to[i].scale=to[i].dscale=0; to[i].isnull=from[bytes]; from+=bytes+1; }
        else { to[i].scale=from[bytes]; to[i].dscale=from[bytes+1]; to[i].isnull=from[bytes+2]; from+=bytes+3; }
    }
}

static inline unsigned char *hash_row(const struct operator_state *o,unsigned row,unsigned bytes)
{
    if(o->hash_blocks)
        return o->hash_blocks[row/HASH_RELOAD_BLOCK_ROWS]+(size_t)(row%HASH_RELOAD_BLOCK_ROWS)*bytes;
    return o->hash_data+(size_t)row*bytes;
}

static inline uint32_t *hash_chain(const struct operator_state *o,unsigned bytes)
{
    return (void *)(o->hash_data+(o->hash_blocks?0:(size_t)o->hash_capacity*bytes));
}

static inline uint64_t mix_hash(uint64_t x)
{ x^=x>>33; x*=UINT64_C(0xff51afd7ed558ccd); x^=x>>33; x*=UINT64_C(0xc4ceb9fe1a85ec53); return x^(x>>33); }

static inline uint64_t now_ns(clockid_t clock)
{ struct timespec t; clock_gettime(clock,&t); return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }

static inline size_t input_offset(const struct execution *s,const struct qp_node *n)
{ return qp_input_offset(s->plan,n,s->worker); }

static inline size_t vector_offset(const struct execution *s,const struct qp_node *n)
{ return s->worker>1?n->parallel_vector+(s->worker-2)*s->plan->parallel_stride:n->vector_offset; }

static inline int64_t timed_call(struct execution *s,int64_t (*call)(struct femu_csd_args *),struct femu_csd_args *a,unsigned op)
{
    if(!s->profiling) return call(a);
    uint64_t wall=now_ns(CLOCK_MONOTONIC),cpu=now_ns(CLOCK_THREAD_CPUTIME_ID);
    int64_t status=call(a); struct qs_timing *t=&s->profile.operators[op];
    uint64_t elapsed=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
    ++t->calls; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-wall; t->cpu_ns+=elapsed;
    s->local_profile_cpu+=elapsed;
    return status;
}

static inline struct blocking_timer blocking_begin(struct execution *s)
{
    return (struct blocking_timer){now_ns(CLOCK_MONOTONIC),now_ns(CLOCK_THREAD_CPUTIME_ID),s->local_profile_cpu};
}

static inline void blocking_end(struct execution *s,unsigned op,struct blocking_timer begin)
{
    uint64_t elapsed=now_ns(CLOCK_THREAD_CPUTIME_ID)-begin.cpu,children=s->local_profile_cpu-begin.children;
    uint64_t own=elapsed>children?elapsed-children:0;
    struct qs_timing *t=&s->profile.operators[op]; ++t->calls;
    t->cpu_ns+=own; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-begin.wall;
    s->local_profile_cpu+=own;
}

int projection_init(struct execution *s,unsigned id,int source);
int64_t projection_fill(struct execution *s,unsigned id,int source);
int prepare_hash_wave(struct execution *s,unsigned hash_sink,unsigned root,int source,bool explicit_sink);
int64_t hash_build(struct execution *s,unsigned id);
void external_advance(struct execution *s,unsigned id);
int64_t external_probe(struct execution *s,unsigned id,struct qs_cell *row);
void hash_release(cs_work_context *ctx,struct operator_state *o);
void hash_key_layout(struct hash_key_layout *layout,const struct qp_node *n,const struct qs_plan *p,int right);
int keys_equal(struct execution *s,const struct qp_node *n,const struct qs_cell *left,const struct qs_cell *right);
int keys_null(const struct qp_node *n,const struct qs_cell *left);
uint64_t row_hash(const struct qp_node *n,const struct qs_cell *row,int right);
uint64_t hash_key(uint64_t hash,struct qs_cell v);
int join_condition(struct execution *s,unsigned id,const struct qs_cell *values,struct qs_cell *match);
int64_t hash_build_input(struct execution *s,unsigned child,struct qs_cell *row);
int materialize_vector(struct execution *s,struct qs_vector *v,unsigned count);
int64_t projection_next(struct execution *s,unsigned id,struct qs_cell *row,int source);
int64_t scan_chunk_next(struct execution *s,unsigned id);
int64_t scan_next(struct execution *s,unsigned id,struct qs_cell *row);
int64_t hash_next(struct execution *s,unsigned id,struct qs_cell *row);
#ifndef QP_SPLIT_RUNTIME
int64_t join_next(struct execution *s,unsigned id,struct qs_cell *row);
#endif
int64_t ordered_index_next(struct execution *s,unsigned id);
int graph_open_index(const struct qs_plan *,struct operator_state *);
void ordered_index_metrics(struct operator_state *,struct qs_metrics *,struct qs_profile *);
void graph_index_init(struct execution *s,size_t budget);
int compare_cell(struct execution *s,struct qs_cell a,struct qs_cell b,unsigned type);
int text_status(struct execution *s,int64_t status);
struct operator_state *group_worker_local(struct execution *s,unsigned id);
int64_t radix_flush(struct execution *s,unsigned id);
uint64_t group_values_hash(const struct qp_node *n,const struct qs_cell *keys);
int group_text_extreme(const struct execution *s,const struct qp_node *n);
int group_equal(struct execution *s,const struct qp_node *n,const struct qs_cell *a,const struct qs_cell *b);
int64_t next_row(struct execution *s,unsigned id,struct qs_cell *row);

int64_t scan_decode(struct execution *s,unsigned id,unsigned bytes,bool eager);
int scan_filter_init(struct execution *s,const struct qs_plan *p,struct operator_state *o);
int64_t scan_filter_chunk(struct execution *s,unsigned id);
int64_t hash_match_next(struct execution *s,unsigned id,int boundary);
int hash_probe_projection(struct execution *s,unsigned id,int source);
int hash_left_payload(struct execution *s,unsigned id);
int64_t group_sink_vectors(struct execution *s,unsigned id,struct qs_result *out,unsigned capacity);
int aggregate_sink_vectors(struct execution *s,unsigned id);

int group_reduce_run(const struct qp_node *n,const struct projection_batch *b,
    const uint16_t *rows,unsigned count,struct group_value *values);
void projection_append(struct projection_batch *b,const struct qs_cell *values);
int projection_execute(struct projection_batch *b,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,struct qs_vector_input input);
int64_t merge_match_next(struct execution *s,unsigned id,struct qs_cell *values);
int64_t index_next(struct execution *s,unsigned id,struct qs_cell *row,struct projection_batch *b);
static inline struct compact_group *group_at(const struct operator_state *o,unsigned slot)
{ return (void *)((unsigned char *)o->groups+(size_t)slot*o->group_stride); }
static inline unsigned group_entry_state(unsigned flags,uint64_t hash)
{ return flags|(((unsigned)(hash>>48)&0x7fffU)<<16); }
int64_t projection_join_fill(struct execution *s,struct projection_batch *b);
struct qs_scan_cursor * qs_scan_cursor_create(cs_work_context *context,struct qs_dictionary *dict);
int64_t qs_scan_columns(struct qs_scan_cursor *cursor,struct femu_csd_args *a,uint64_t completion);
int qs_scan_tuple(const struct qs_plan *p,const unsigned char *raw,unsigned offset,unsigned len,unsigned page,struct qs_chunk *c,unsigned row,struct qs_dict *dict);
int64_t qs_scan_tuple_paged(struct qs_scan_cursor *cursor,const struct qs_plan *p,const unsigned char *raw,unsigned offset,unsigned len,unsigned page,struct qs_chunk *c,unsigned row,void *io,uint64_t completion);
int64_t pg_sina_filter_impl(struct femu_csd_args *a);
/* Installed by the device program module, before it can open a query. */
int accumulate_value(struct execution *s,const struct qp_node *n,
    const struct qs_agg *agg,struct accumulator *a,struct qs_cell v);
int group_update(struct execution *s,const struct qp_node *n,unsigned aggregate,
    struct group_value *g,struct qs_cell value,uint64_t count);
int group_update_continue(struct execution *s,unsigned id);
int group_sink_values(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *keys,const struct group_value *values,int shared);
int group_worker_values(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *keys,const struct group_value *values,int shared,uint64_t hash);
void group_keys(const struct qp_node *n,const struct operator_state *o,const struct compact_group *g,struct qs_cell *keys);
const struct compact_group *group_source_next(struct execution *s,unsigned id);
int group_final_value(const struct qs_agg *agg,const struct group_value *g,struct qs_cell *value);
int64_t radix_source_next(struct execution *s,unsigned id,struct qs_cell *row,int raw);
int64_t aggregate_sorted_source_next(struct execution *s,unsigned id,struct qs_cell *row,int raw);
int64_t aggregate_next_impl(struct execution *s,unsigned id,struct qs_cell *row,int raw);
int64_t projection_group_fill(struct execution *s,struct projection_batch *b);
int64_t group_combine_local(struct execution *s);


enum group_read_mode { GROUP_READ, GROUP_PREPARE, GROUP_ALLOCATE };

typedef int64_t (*graph_tail_operator)(struct execution *,struct femu_csd_args *,unsigned,unsigned);

typedef int64_t (*graph_stream_operator)(struct execution *,unsigned,unsigned,struct qs_result *);
int64_t graph_block_call(struct femu_csd_args *a,enum qp_graph_block_step step,graph_stream_operator execute);
int64_t graph_device_scan(struct execution *s,unsigned scan,
    int64_t (*read)(struct execution *,struct projection_batch *));
int graph_attach_stream_input(struct execution *s,unsigned scan,unsigned input);
int graph_stream_node(const struct execution *s);
int64_t graph_stream_project(struct execution *s,unsigned scan,unsigned id);

typedef int64_t (*graph_sink_batch)(struct execution *,unsigned,struct projection_batch *,struct qs_result *);
struct graph_sink_ops {
    graph_sink_batch consume;
    int64_t (*finish)(struct execution *,unsigned);
    bool borrow_projection;
};
int64_t graph_stream_sink(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out,
    const struct graph_sink_ops *ops);
int64_t graph_spool_result(struct execution *s,struct qs_result *out,unsigned cols);

static inline bool graph_device_source(const struct qp_node *leaf)
{ return leaf->kind==QP_AGGREGATE || leaf->kind==QP_CTE_SCAN || leaf->ordered_index; }
struct graph_result_source {
    bool (*ready)(const struct execution *,unsigned);
    int64_t (*next)(struct execution *,unsigned,struct qs_cell *);
};
int64_t graph_emit(struct femu_csd_args *a,enum graph_emit_kind kind,const struct graph_result_source *source);
void projection_clear(struct execution *s,struct projection_batch **slot);
bool graph_operator_args(const struct femu_csd_args *a);
int graph_wave_node(const struct femu_csd_args *a);
int64_t graph_wave_begin(struct femu_csd_args *a,const struct graph_projection_ops *ops,
    graph_query_initializer initialize,struct execution **execution);
int64_t hash_aggregate_next_impl(struct execution *s,unsigned id,struct qs_cell *row,
    enum group_read_mode prepare,int raw);
void plan_output_metrics(struct execution *s,struct qs_result *out,unsigned worker);
int64_t graph_tail_call(struct femu_csd_args *a, graph_tail_operator execute,bool report_worker);
int intern_view(struct execution *s,struct qs_text_view view,unsigned *id);
int inset_vectors(struct qs_vector_input *input,unsigned index,const uint16_t *rows,unsigned count);
static inline void hash_store_value(unsigned char *to,const struct qs_cell *value,unsigned bytes)
{
    memcpy(to,&value->value,bytes);
    if(bytes<16) to[bytes]=value->isnull;
    else { to[bytes]=value->scale; to[bytes+1]=value->dscale; to[bytes+2]=value->isnull; }
}
static inline unsigned group_payload_bytes(const struct qp_node *n,unsigned k)
{
    unsigned type=n->expr.exprs[n->expr.keys[k]].type;
    return type==Q_DECIMAL || type==Q_STRING?19:9;
}
static inline int group_add(struct group_value *g,struct qs_cell v,uint64_t count,unsigned kind)
{
    if(count && (kind==Q_SUM || kind==Q_AVG)) {
        if(!g->count) g->sum=v;
        else {
            unsigned scale=v.scale>g->sum.scale?v.scale:g->sum.scale; __int128 left,right;
            if(qs_scale(g->sum.value,scale-g->sum.scale,&left) || qs_scale(v.value,scale-v.scale,&right) ||
               __builtin_add_overflow(left,right,&g->sum.value)) return -5;
            g->sum.scale=scale; if(v.dscale>g->sum.dscale) g->sum.dscale=v.dscale;
        }
    }
    return __builtin_add_overflow(g->count,count,&g->count)?-5:0;
}
void hash_store(unsigned char *to,const struct qs_cell *from,const struct qs_plan *p);
int64_t spool_append_context(struct execution *s,struct spool *p,const void *row,unsigned row_bytes,
    cs_work_context *context,size_t resident_reserve);
int64_t spool_append(struct execution *s,struct spool *p,const struct qs_cell *row,unsigned cols);
int64_t spool_append_bytes(struct execution *s,struct spool *p,const void *row,unsigned row_bytes);
int64_t spool_row(struct execution *s,struct spool *p,uint64_t index,unsigned cols,struct qs_cell *row);
int64_t spool_row_bytes(struct execution *s,struct spool *p,uint64_t index,unsigned row_bytes,void *row);
int open_execution(struct femu_csd_args *a,struct qp_plan *plan,
    struct qp_control *control,const struct graph_projection_ops *ops,
    graph_query_initializer initialize,struct execution **opened);
int64_t initialize_dictionary(struct execution *s,struct femu_csd_args *a);
/* The EXEC owns the stage; the wave loop allocates and drains worker slots.
 * Worker sink IDs are one-based and remain zero for unused adapters. */
struct graph_wave_sink {
    unsigned node;
    unsigned hash,group,aggregate,partition,set;
    size_t reserve_bytes;
    int (*prepare)(struct execution *,unsigned,unsigned,int);
    int (*start)(struct execution *,unsigned,unsigned);
    int64_t (*merge)(struct execution *,unsigned,struct execution *);
    int (*finish)(struct execution *,unsigned,unsigned);
};
int64_t parallel_wave_next(struct execution *s,unsigned root,struct qs_cell *row,
    const struct graph_wave_sink *sink);
int64_t aggregate_wave_next(struct execution *s,unsigned root,struct qs_cell *row,unsigned id);
int64_t graph_finish(struct execution *s,struct femu_csd_args *a,struct qs_result *out);
int64_t namespace_wait(struct execution *s);
size_t sort_spill_reserve(const struct execution *s);
void host_begin(struct execution *s,const struct qs_plan *p,struct qs_result *out,unsigned capacity);
int host_append(struct execution *s,const struct qs_plan *p,struct qs_result *out);
void wait_resume(struct execution *s);
int resume_page_io(struct execution *s,struct femu_csd_args *a);
int resume_spool(struct execution *s,struct femu_csd_args *a);
int materialize_outputs(struct execution *s,struct projection_batch *b);
int evaluate(struct execution *s,struct qs_eval *e,int index,struct qs_cell *out);
void init_eval(struct qs_eval *e,const struct qs_plan *plan,const struct qs_cell *row,const struct qs_cell *aggregates);
struct projection_import {
    struct execution *execution;
    struct projection_batch *batch;
    unsigned nodes[QS_EXPRS],expressions[QS_EXPRS];
};

int projection_import(struct projection_import *in,unsigned node,int index);
int text_release(struct execution *s);

int hash_sink_batch(struct execution *s,struct qs_result *out);
int64_t block_hash_vectors(struct execution *s,unsigned id,struct projection_batch *b);
int64_t partition_sink_prefix(struct execution *s);
int64_t partition_sink_vectors(struct execution *s,struct projection_batch *b);
int64_t external_partition_combine(struct execution *s,struct execution *worker);
int64_t sort_call(struct execution *s,struct sorted_rows *sort,int source);
int64_t sort_init(struct execution *s,struct sorted_rows *sort,const struct qs_plan *input,
    unsigned keys,const int *columns,const struct qs_order *order);
int64_t sorted_input(struct execution *s,struct sorted_rows *sort,unsigned child,
    unsigned keys,const int *columns,const struct qs_order *order,struct qs_cell *row);
int64_t sort_sink_columns(struct execution *s,struct sorted_rows *sort,unsigned id);
int64_t projection_sort_fill(struct execution *s,struct projection_batch *b);
void parallel_sort_begin(struct execution *s,unsigned root);
#ifndef QP_SPLIT_RUNTIME
int64_t materialize(struct execution *s,unsigned id);
int64_t materialized_row(struct execution *s,unsigned id,uint64_t position,struct qs_cell *row);
#endif
int64_t projection_cte_fill(struct execution *s,struct projection_batch *b);
int project(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row);
int project_filtered(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row,int filtered);
int64_t build_set(struct execution *s,unsigned id);
int64_t pg_sina_sort_sink_impl(struct femu_csd_args *);
int64_t qs_sort_finish_sink(uint64_t,struct femu_csd_args *);
int64_t qs_sort_sink_columns(struct femu_csd_args *,const struct qs_plan *,const struct qs_vector *,unsigned,unsigned);
int64_t pg_sina_sort_source_impl(struct femu_csd_args *);
void qs_sort_dictionary(uint64_t,struct qs_dict *);
int qs_sort_paged_dictionary(uint64_t,struct qs_dictionary *);
const char *qs_sort_output_text(const struct qs_result *,uint32_t,unsigned *);
int qs_sort_prepare(uint64_t);
size_t qs_sort_min_workspace(void);
int retry_project(struct execution *s,unsigned id,struct qs_cell *row);

int hash_sink_vectors(struct execution *s,unsigned id);

int equal_cell(struct execution *s,struct qs_cell a,struct qs_cell b,unsigned type);

int64_t parallel_next(struct execution *s,unsigned root,struct qs_cell *row);

void close_execution(struct execution *s);

void wait_io(struct execution *s,int64_t status);

int join_qual_init(struct execution *s,unsigned id,struct projection_batch *b,
    const struct projection_batch *output);

bool graph_input_edge(const struct execution *s,unsigned id);

int64_t block_edge_row(struct execution *s,unsigned id,struct qs_cell *row);


int64_t sort_sink_vectors(struct execution *s,struct projection_batch *b);

int64_t aggregate_worker_source(struct execution *s,unsigned id,struct qs_cell *row);
int aggregate_combine(struct execution *s,unsigned id,const struct operator_state *from);
int parallel_group_begin(struct execution *s,unsigned id);
int finish_group_workers(struct execution *s,unsigned id,unsigned root);
int64_t aggregate_accept_result(struct execution *s,struct qs_result *out);
int64_t aggregate_accept_vectors(struct execution *s,unsigned id,struct qs_result *out,unsigned capacity);
int input_ordered(const struct qp_plan *p,unsigned child,unsigned count,
    const int *columns,const struct qs_order *order);

#endif
