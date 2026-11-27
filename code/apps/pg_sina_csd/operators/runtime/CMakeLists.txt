pg_sina_graph_operator(pg-sina-graph-runtime RUNTIME
    SOURCES plan.c dictionary.c filter.c scan.c sort.c index_scan.c
        predicate.c projection.c hash.c index.c row_sort.c
    EXEC pg_sina_graph_query_close
)
if(TARGET pg-sina-graph-runtime)
    target_compile_definitions(pg-sina-graph-runtime PRIVATE
        pg_sina_filter=pg_sina_filter_impl pg_sina_scan=pg_sina_scan_impl
        pg_sina_sort=pg_sina_sort_impl pg_sina_sort_sink=pg_sina_sort_sink_impl
        pg_sina_sort_source=pg_sina_sort_source_impl)
    install(TARGETS cs-workmem LIBRARY DESTINATION .)
endif()
