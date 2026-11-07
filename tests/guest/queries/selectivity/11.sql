-- 先校准 nation，再对该国家的零件聚合做 HAVING 校准，分母为过滤前零件分组数。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
nations AS (
    SELECT n_name, count(*) AS n, sum(count(*)) OVER () AS total
    FROM nation
    GROUP BY n_name
),
selected_nation AS (
    SELECT nations.* FROM nations CROSS JOIN target
    ORDER BY abs(n::numeric / nullif(total, 0) - value), n_name
    LIMIT 1
),
part_values AS (
    SELECT ps_partkey, sum(ps_supplycost * ps_availqty) AS part_value
    FROM partsupp JOIN supplier ON s_suppkey = ps_suppkey
    JOIN nation ON n_nationkey = s_nationkey
    JOIN selected_nation USING (n_name)
    GROUP BY ps_partkey
),
histogram AS (
    SELECT part_value, count(*) AS n FROM part_values GROUP BY part_value
),
cuts AS (
    SELECT (part_value + coalesce(lag(part_value) OVER (ORDER BY part_value DESC), part_value + 2)) / 2 AS cutoff,
           sum(n) OVER (ORDER BY part_value DESC) - n AS matched,
           sum(n) OVER () AS total_groups,
           sum(part_value * n) OVER () AS total_value
    FROM histogram
    UNION ALL
    SELECT min(part_value) - 1, sum(n), sum(n), sum(part_value * n) FROM histogram
),
candidates AS (
    SELECT 'nation' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q11_nation', n_name::text) AS parameters
    FROM selected_nation
    UNION ALL
    SELECT 'partsupp HAVING (part groups)' AS filter_scope, matched AS matched_rows, total_groups AS input_rows,
           jsonb_build_object('q11_value_fraction', cutoff / nullif(total_value, 0)) AS parameters
    FROM cuts WHERE total_value > 0
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
