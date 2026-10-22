/* PostgreSQL's selected physical tree, compiled before set_plan_references.
 * This is private SQL-program data; FEMU and SNIA do not interpret it. */
#ifndef PG_SINA_PLAN_H
#define PG_SINA_PLAN_H
#include "query.h"
#define QP_NODES 96
#define QP_PARAMS 128
#define QP_SCAN_BYTES (1024U*1024)
enum qp_kind { QP_SCAN, QP_PROJECT, QP_JOIN, QP_AGGREGATE, QP_SORT, QP_LIMIT, QP_INDEX_JOIN, QP_CTE_SCAN, QP_MATERIALIZED, QP_SET };
enum qp_join { QP_INNER, QP_LEFT, QP_SEMI, QP_ANTI };
enum qp_expr { QP_PARAMETER=Q_DIV+1, QP_LIKE, QP_SUBSTRING, QP_COALESCE, QP_INSET };
struct qp_node {
    struct qs_plan expr;
    int kind, left, right, join_type, join_qual;
    unsigned nkeys, distinct, distinct_aggs;
    unsigned sorted_input;
    unsigned hash;
    int left_keys[QS_KEYS], right_keys[QS_KEYS];
    uint64_t bytes, input_offset, vector_offset;
    double estimated_rows;
};
struct qp_plan {
    unsigned count, root, nparams, ninit;
    int param_nodes[QP_PARAMS],cte_nodes[QP_PARAMS],init_nodes[QP_PARAMS],init_params[QP_PARAMS];
    struct qp_node nodes[QP_NODES];
};
/* Only page addresses and lifecycle state cross to PG during execution. */
struct qp_control {
    uint64_t state, offset;
    unsigned node, bytes, ready, finished, close, index_nreads;
    int error;
    unsigned error_node;
    struct qs_index_read index_reads[QS_INDEX_READS];
};
#endif
