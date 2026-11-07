-- 按实际发货日累计行数，包含零命中的边界。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
daily AS (
    SELECT l_shipdate AS day, count(*) AS n FROM lineitem GROUP BY l_shipdate
),
histogram AS (
    SELECT day, sum(n) OVER (ORDER BY day) AS n, sum(n) OVER () AS total FROM daily
    UNION ALL
    SELECT min(day) - 1, 0, sum(n) FROM daily
),
candidates AS (
    SELECT 'lineitem' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q1_ship_delay_days', date '1998-12-01' - day) AS parameters
    FROM histogram
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
