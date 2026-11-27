pg_sina_graph_operator(pg-sina-graph-hash-probe
    SOURCES graph_hash_probe.c
    EXEC
        pg_sina_graph_block_probe
        pg_sina_graph_partition_probe
)
