pg_sina_graph_operator(pg-sina-graph-sort
    SOURCES graph_sort.c
    EXEC
        pg_sina_graph_sort_wave
        pg_sina_graph_sort_sink
        pg_sina_graph_sort_source
        pg_sina_graph_sort_result
        pg_sina_graph_block_sort
)
