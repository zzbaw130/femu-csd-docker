pg_sina_graph_operator(pg-sina-graph-scan
    SOURCES graph_scan.c
    EXEC
        pg_sina_graph_block_scan
        pg_sina_graph_scan_wave
        pg_sina_graph_scan_output
        pg_sina_graph_scan_drain
)
