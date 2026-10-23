/* PostgreSQL's selected physical tree, compiled before set_plan_references.
 * This is private SQL-program data; FEMU and SNIA do not interpret it. */
#ifndef PG_SINA_PLAN_H
#define PG_SINA_PLAN_H
#include "query.h"
#define QP_NODES 96
#define QP_PARAMS 128
#define QP_SCAN_BYTES (1024U*1024)
/* cparam1: profile bit 0, CU slot bits 1..7. A scheduled heap input uses
 * bit 8 and its page number in bits 9..63; cparam2 stays reserved for spill. */
#define QP_SCAN_INPUT (UINT64_C(1)<<8)
#define QP_SCAN_PAGE_SHIFT 9
/* Ordinary CompValue bits for a host-scheduled scan, below SNIA spill bits. */
#define QP_OUTPUT_READY 1U
#define QP_INPUT_DRAINED 2U
#define QP_WORKER_FINISHED 4U
enum qp_kind { QP_SCAN, QP_PROJECT, QP_JOIN, QP_AGGREGATE, QP_SORT, QP_LIMIT, QP_INDEX_JOIN, QP_CTE_SCAN, QP_MATERIALIZED, QP_SET };
enum qp_join { QP_INNER, QP_LEFT, QP_SEMI, QP_ANTI };
enum qp_expr { QP_PARAMETER=Q_DIV+1, QP_LIKE, QP_SUBSTRING, QP_COALESCE, QP_INSET };
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
};
/* Optional per-pipeline measurements; only the coordinator writes these.
 * CPU sums include all CSF slices, wall includes LOAD and scheduling waits. */
struct qp_pipeline_profile {
    uint64_t wall_ns, coordinator_ns, worker_ns, rounds;
    /* Shared Hash/aggregate sink work is a subset of worker_ns. */
    uint64_t sink_ns,sink_rows;
};
struct qp_plan {
    unsigned count, root, nparams, ninit;
    unsigned workers, scan_bytes;
    uint64_t parallel_stride;
    int param_nodes[QP_PARAMS],cte_nodes[QP_PARAMS],init_nodes[QP_PARAMS],init_params[QP_PARAMS];
    struct qp_node nodes[QP_NODES];
    struct qp_pipeline_profile pipelines[QP_NODES];
};
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
    unsigned output_rows, peak_cus;
    struct qs_index_read index_reads[QS_INDEX_READS];
};
static inline uint64_t qp_worker_mask(unsigned workers)
{ return workers==64?UINT64_MAX:(UINT64_C(1)<<workers)-1; }
static inline unsigned qp_scan_buffer_bytes(const struct qp_plan *p,const struct qp_node *n)
{
    unsigned bytes=n->bytes<p->scan_bytes?n->bytes:p->scan_bytes;
    return bytes?bytes:QS_PAGE;
}
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
/* Mark every independent scan/probe pipeline, including InitPlan scans.
 * Each phase reuses the same worker buffers after the previous phase drains. */
static inline void qp_parallel_boundaries(struct qp_plan *p,int id)
{
    if(id<0) return;
    struct qp_node *n=&p->nodes[id]; int candidate=-1;
    if(n->kind==QP_SORT || (n->kind==QP_AGGREGATE && (!n->nkeys || !n->sorted_input))) candidate=n->left;
    int scan=qp_probe_scan(p,candidate);
    if(scan>=0 && p->nodes[scan].bytes>=4*QP_SCAN_BYTES) p->nodes[candidate].parallel=1;
    qp_parallel_boundaries(p,n->left);
    if(n->kind==QP_JOIN && n->hash) {
        scan=qp_probe_scan(p,n->right);
        if(scan>=0 && p->nodes[scan].bytes>=4*QP_SCAN_BYTES) p->nodes[n->right].parallel=1;
    }
    /* Merge inputs have independent blocking pipelines too. An index lookup
     * is driven by its left input and must not become a full parallel scan. */
    if(n->kind==QP_JOIN) qp_parallel_boundaries(p,n->right);
}
#endif
