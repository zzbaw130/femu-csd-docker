pg_sina_graph_operator(pg-sina-graph-materialize
    SOURCES graph_materialize.c
    EXEC
        pg_sina_graph_cte_materialize
)
