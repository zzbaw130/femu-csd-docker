/* PostgreSQL's selected physical tree, compiled before set_plan_references.
 * This is private SQL-program data; FEMU and SNIA do not interpret it. */
#ifndef PG_SINA_PLAN_H
#define PG_SINA_PLAN_H
#include "query.h"
#include "host_result.h"
#define QP_NODES 96
#define QP_PARAMS 128
#define QP_SCAN_BYTES (1024U*1024)
#define QP_VECTOR_ROWS 2048U
/* cparam1: profile bit 0, CU slot bits 1..7. A scheduled heap input uses
 * bit 8 and its page number in bits 9..63. With bit 8 clear, bit 9 marks
 * completion of the previous requested page LOAD; cparam2 stays for spill. */
#define QP_SCAN_INPUT (UINT64_C(1)<<8)
#define QP_SCAN_PAGE_SHIFT 9
#define QP_PAGES_INPUT (UINT64_C(1)<<9)
/* Worker completion bits, below SNIA spill bits. Only CONTROL_REQUIRED needs
 * the full AFDM record to submit a page request not handled by device I/O. */
#define QP_OUTPUT_READY 1U
#define QP_INPUT_DRAINED 2U
#define QP_WORKER_FINISHED 4U
#define QP_CONTROL_REQUIRED 8U
#define QP_RESOURCE_WAIT 16U /* Retain input; retry after another batch advances. */
/* Direct Scan completion: flags 0..4, row count 5..21, varlena bytes 22..38.
 * Bit 39 distinguishes metadata from ordinary worker status. All remain
 * below SNIA's reentry operation bits; 17 bits cover any typed batch. */
#define QP_TYPED_OUTPUT (UINT64_C(1)<<39)
static inline unsigned qp_output_rows(uint64_t value) { return (value>>5)&0x1ffff; }
static inline unsigned qp_output_strings(uint64_t value) { return (value>>22)&0x1ffff; }
enum qp_kind { QP_SCAN, QP_PROJECT, QP_JOIN, QP_AGGREGATE, QP_SORT, QP_LIMIT, QP_INDEX_JOIN, QP_CTE_SCAN, QP_MATERIALIZED, QP_SET };
enum qp_join { QP_INNER, QP_LEFT, QP_SEMI, QP_ANTI };
enum qp_expr { QP_PARAMETER=Q_PARAMETER, QP_LIKE=Q_LIKE, QP_SUBSTRING=Q_SUBSTRING,
               QP_COALESCE=Q_COALESCE, QP_INSET=Q_INSET };
struct qp_node {
    struct qs_plan expr;
    int kind, left, right, join_type, join_qual;
    unsigned nkeys, distinct, distinct_aggs;
    unsigned sorted_input, ordered_index; /* bits: forward, heap read-ahead, covered keys, unordered leaf ranges */
    unsigned hash, parallel;
    int left_keys[QS_KEYS], right_keys[QS_KEYS];
    uint64_t bytes, index_bytes, input_offset, vector_offset;
    uint64_t parallel_input, parallel_vector;
    double estimated_rows;
    double estimated_groups; /* Pre-HAVING state capacity; may use a declared FK domain bound. */
};
/* Optional per-pipeline measurements; only the coordinator writes these.
 * CPU sums include all CSF slices, wall includes LOAD and scheduling waits. */
struct qp_pipeline_profile {
    uint64_t wall_ns, coordinator_ns, worker_ns, rounds;
    /* Shared Hash/aggregate sink work is a subset of worker_ns. */
    uint64_t sink_ns,sink_rows;
    /* Subintervals of coordinator Hash Build CPU, not additional time. */
    uint64_t hash_memory_ns,hash_reindex_ns,hash_reindexes,hash_reindexed_rows;
    uint64_t hash_moved_bytes;
};
/* PG appends immutable extent arrays after qp_plan in its input AFDM.
 * Offsets are relative to that AFDM; only the SQL program interprets them. */
struct qp_file_mapping {
    uint64_t offset;
    uint32_t nsid,lba_bytes,count,reserved;
};
struct qp_plan {
    unsigned count, root, nparams, ninit;
    unsigned workers, scan_bytes; /* workers counts task contexts, not physical CUs. */
    unsigned packed_result; /* 0: legacy, 1: projected cells, 2: typed, 3: CU-direct typed. */
    uint64_t parallel_stride;
    int param_nodes[QP_PARAMS],cte_nodes[QP_PARAMS],init_nodes[QP_PARAMS],init_params[QP_PARAMS];
    struct qp_node nodes[QP_NODES];
    struct qp_pipeline_profile pipelines[QP_NODES];
    struct qp_file_mapping files[QP_NODES],indexes[QP_NODES];
};
static inline int qp_host_raw_projection(const struct qp_plan *p)
{
    const struct qp_node *n=&p->nodes[p->root];
    if(p->packed_result<QP_TYPED_RESULT || n->kind!=QP_SCAN || n->ordered_index) return 0;
    if(n->expr.predicate>=0) {
        /* A fixed-width column comparison leaves projected Numeric/text
         * bodies untouched. Other predicates retain normal materialization. */
        const struct qs_expr *e=&n->expr.exprs[n->expr.predicate];
        if(e->op<Q_EQ || e->op>Q_GE || e->left<0 || e->right<0) return 0;
        const struct qs_expr *col=&n->expr.exprs[e->left],*value=&n->expr.exprs[e->right];
        if(col->op==Q_CONST) { const struct qs_expr *swap=col; col=value; value=swap; }
        if(col->op!=Q_COLUMN || value->op!=Q_CONST || col->column<0 ||
           (unsigned)col->column>=n->expr.nattrs || value->type==Q_STRING ||
           n->expr.cols[col->column].type==Q_STRING || n->expr.cols[col->column].type==Q_DECIMAL) return 0;
    }
    int variable=0;
    for(unsigned i=0;i<n->expr.nout;++i) {
        const struct qs_expr *e=&n->expr.exprs[n->expr.outputs[i].index];
        if(n->expr.outputs[i].aggregate || e->op!=Q_COLUMN || e->column<0 || (unsigned)e->column>=n->expr.nattrs) return 0;
        unsigned oid=n->expr.outputs[i].type_oid,type=n->expr.cols[e->column].type;
        if(type==Q_DECIMAL && oid!=1700) return 0;
        if(oid==1700 && type!=Q_DECIMAL) return 0;
        if(qp_host_variable(oid)) {
            if(oid!=1700 && type!=Q_STRING && type!=Q_CHAR) return 0;
            variable=1;
        } else if(type==Q_STRING) return 0;
    }
    return variable;
}
/* Only page addresses and lifecycle state cross to PG during execution. */
struct qp_control {
    uint64_t state, offset;
    unsigned node, bytes, ready, finished, close, index_nreads;
    int error;
    unsigned error_node;
    /* parallel=2 adds a CU wave barrier before a shared sink can resize/spill. */
    unsigned parallel, epoch;
    /* One-based heap scan whose input blocks PG can assign in this phase. */
    unsigned scan_node;
    uint64_t available, drained;
    /* Completion metadata avoids a second AFDM read for internal row batches. */
    unsigned output_rows, peak_cus, output_strings;
    struct qs_index_read index_reads[QS_INDEX_READS];
};
/* Each spilled group carries its keys and one value/count pair per aggregate.
 * Keep planner eligibility and the device's bounded row format in agreement. */
static inline int qp_hash_aggregate_supported(const struct qp_node *n)
{
    if(!n->nkeys || !n->expr.naggs || n->expr.naggs>QS_AGGS || n->nkeys+2*n->expr.naggs>QS_OUT) return 0;
    for(unsigned i=0;i<n->expr.naggs;++i) {
        const struct qs_agg *a=&n->expr.aggs[i];
        if(a->kind!=Q_COUNT_STAR && ((a->kind!=Q_SUM && a->kind!=Q_AVG && a->kind!=Q_COUNT &&
            a->kind!=Q_MIN && a->kind!=Q_MAX) || a->expr<0)) return 0;
    }
    return 1;
}
static inline uint64_t qp_worker_mask(unsigned workers)
{ return workers==64?UINT64_MAX:(UINT64_C(1)<<workers)-1; }
static inline unsigned qp_scan_buffer_bytes(const struct qp_plan *p,const struct qp_node *n)
{
    unsigned bytes=n->bytes<p->scan_bytes?n->bytes:p->scan_bytes;
    return bytes?bytes:QS_PAGE;
}
/* I/O blocks may contain many vectors; reserve only one vector per worker. */
static inline unsigned qp_scan_capacity(const struct qp_plan *p,const struct qp_node *n)
{
    unsigned rows=qp_scan_buffer_bytes(p,n)/24;
    return rows<QP_VECTOR_ROWS?rows:QP_VECTOR_ROWS;
}
/* Bound dictionary limits by physical Heap input, not estimated output
 * rows/NDV. A tuple needs at least a 24-byte header and a 4-byte line pointer;
 * all of its inline string bytes together fit inside the relation's bytes.
 * Expressions creating new strings keep the existing maximum limits. */
static inline void qp_dictionary_size(const struct qp_plan *p,unsigned *entries,unsigned *data)
{
    uint64_t count=QS_DICT_SMALL_ENTRIES,bytes=QS_DICT_SMALL_DATA;
    for(unsigned i=0;i<p->count;++i) {
        const struct qp_node *n=&p->nodes[i];
        unsigned strings=0;
        if(n->kind==QP_SCAN) for(unsigned c=0;c<n->expr.nattrs;++c)
            strings+=n->expr.cols[c].slot>=0 && n->expr.cols[c].type==Q_STRING;
        if(strings) {
            uint64_t rows=n->bytes/28+1;
            if(rows>QS_DICT_BIG_ENTRIES) rows=QS_DICT_BIG_ENTRIES;
            count+=rows*strings;
            bytes+=n->bytes<QS_DICT_BIG_DATA?n->bytes:QS_DICT_BIG_DATA;
        }
        count+=n->expr.nstrings;
        for(unsigned j=0;j<n->expr.nstrings;++j) bytes+=n->expr.strings[j].length;
        for(unsigned j=0;j<n->expr.nexprs;++j) {
            const struct qs_expr *e=&n->expr.exprs[j];
            if(e->type==Q_STRING && e->op!=Q_COLUMN && e->op!=Q_CONST &&
               e->op!=Q_CASE && e->op!=Q_COALESCE && e->op!=Q_AGGREF && e->op!=Q_PARAMETER) {
                count=QS_DICT_BIG_ENTRIES; bytes=QS_DICT_BIG_DATA;
            }
        }
        if(count>QS_DICT_BIG_ENTRIES) count=QS_DICT_BIG_ENTRIES;
        if(bytes>QS_DICT_BIG_DATA) bytes=QS_DICT_BIG_DATA;
    }
    *entries=QS_DICT_SMALL_ENTRIES;
    while(*entries<count) *entries*=2;
    *data=(bytes+4095)&~UINT64_C(4095);
}
/* Reuse the existing 4096-cell AFDM for projected rows. Bound string output
 * by the current host ABI's 255-byte value limit and the 64KiB text arena. */
static inline unsigned qp_result_capacity(const struct qs_plan *p,int packed)
{
    if(packed>=(int)QP_TYPED_RESULT) return qp_host_capacity(p);
    if(!packed) return QS_RESULT_ROWS;
    unsigned rows=QS_RESULT_ROWS*QS_OUT/(p->nout?p->nout:1),text=0;
    for(unsigned c=0;c<p->nout;++c) {
        int expression=p->outputs[c].index;
        if(p->outputs[c].aggregate) {
            const struct qs_agg *a=&p->aggs[expression];
            if(a->kind!=Q_MIN && a->kind!=Q_MAX) continue;
            expression=a->expr;
        }
        if(expression<0 || p->exprs[expression].type!=Q_STRING) continue;
        unsigned bytes=p->outputs[c].typmod>4?(unsigned)p->outputs[c].typmod-4:255;
        if(bytes>QS_STRING_MAX) text+=bytes<255?bytes:255;
    }
    if(text && rows>QS_RESULT_STRING_BYTES/text) rows=QS_RESULT_STRING_BYTES/text;
    return rows?rows:1;
}
static inline struct qs_cell *qp_result_row(struct qs_result *out,unsigned row,unsigned columns,int packed)
{ return (struct qs_cell *)out->data+(size_t)row*(packed?columns:QS_OUT); }
/* A parallel probe pipeline ends before an order-dependent/blocking operator.
 * Hash build children retain the exact PG-selected tree and are built once. */
static inline int qp_probe_scan(const struct qp_plan *p,int id)
{
    if(id<0) return -1;
    const struct qp_node *n=&p->nodes[id];
    if(n->kind==QP_SCAN) return n->ordered_index && !(n->ordered_index&8)?-1:id;
    if(n->kind==QP_PROJECT || n->kind==QP_INDEX_JOIN || (n->kind==QP_JOIN && n->hash))
        return qp_probe_scan(p,n->left);
    return -1;
}
/* A finalized resident Hash Aggregate can feed a new pipeline without
 * rescanning its input. Runtime spill still uses the serial merge source. */
static inline int qp_pipeline_source(const struct qp_plan *p,int id)
{
    if(id<0) return -1;
    const struct qp_node *n=&p->nodes[id];
    if(n->kind==QP_SCAN) return n->ordered_index && !(n->ordered_index&8)?-1:id;
    if(n->kind==QP_AGGREGATE && n->hash && !n->distinct && !n->distinct_aggs && qp_hash_aggregate_supported(n)) return id;
    if(n->kind==QP_PROJECT || n->kind==QP_INDEX_JOIN || (n->kind==QP_JOIN && n->hash))
        return qp_pipeline_source(p,n->left);
    return -1;
}
static inline void qp_mark_parallel(struct qp_plan *p,int id)
{
    int source=qp_pipeline_source(p,id); if(source<0) return;
    const struct qp_node *n=&p->nodes[source];
    if((n->kind==QP_SCAN && n->bytes>=4*QP_SCAN_BYTES) ||
       (n->kind==QP_AGGREGATE &&
        (n->estimated_groups>=QP_VECTOR_ROWS || n->estimated_rows>=QP_VECTOR_ROWS))) p->nodes[id].parallel=1;
}
/* Mark every independent scan/probe pipeline, including InitPlan scans.
 * Each phase reuses the same worker buffers after the previous phase drains. */
static inline void qp_parallel_boundaries(struct qp_plan *p,int id)
{
    if(id<0) return;
    struct qp_node *n=&p->nodes[id]; int candidate=-1;
    if((unsigned)id==p->root && n->kind==QP_SCAN) qp_mark_parallel(p,id);
    if(n->kind==QP_SORT || (n->kind==QP_AGGREGATE && (!n->nkeys || !n->sorted_input))) candidate=n->left;
    qp_mark_parallel(p,candidate);
    qp_parallel_boundaries(p,n->left);
    if(n->kind==QP_JOIN && n->hash) {
        qp_mark_parallel(p,n->right);
    }
    /* Merge inputs have independent blocking pipelines too. An index lookup
     * is driven by its left input and must not become a full parallel scan. */
    if(n->kind==QP_JOIN) qp_parallel_boundaries(p,n->right);
}
/* Index misses suspend a task while retaining its input. Keep a second
 * task per physical CU ready to run during that I/O. These are independent
 * execution contexts, not extra compute threads. The caller must still fit
 * their buffers in FDM and may reduce this count when capacity is limited. */
static inline unsigned qp_task_slots(const struct qp_plan *p,unsigned cus)
{
    unsigned slots=cus<QS_WORKERS?cus:QS_WORKERS;
    if(!slots) slots=1;
    for(unsigned i=0;i<p->count;++i) if(p->nodes[i].parallel)
        for(int id=i;id>=0;id=p->nodes[id].left) {
            if(p->nodes[id].kind==QP_INDEX_JOIN)
                return slots>QS_WORKERS/2?QS_WORKERS:2*slots;
            if(id==qp_pipeline_source(p,i)) break;
        }
    return slots;
}
#endif
