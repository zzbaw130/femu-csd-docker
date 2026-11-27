pg_sina_graph_operator(pg-sina-graph-aggregate
    SOURCES graph_aggregate.c graph_aggregate_state.c
    EXEC
        pg_sina_graph_aggregate_reserve
        pg_sina_graph_aggregate_wave
        pg_sina_graph_aggregate_combine
        pg_sina_graph_aggregate_source
        pg_sina_graph_aggregate_result
        pg_sina_graph_block_aggregate
        pg_sina_graph_sum
)
