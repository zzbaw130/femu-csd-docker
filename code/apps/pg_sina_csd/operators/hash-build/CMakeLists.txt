pg_sina_graph_operator(pg-sina-graph-hash-build
    SOURCES graph_hash_build.c
    EXEC
        pg_sina_graph_hash_wave
        pg_sina_graph_block_hash_build
        pg_sina_graph_block_partition
        pg_sina_graph_partition_reload
)
