-- 仅校准可变的 nation 过滤，订单状态和 EXISTS 条件为固定条件。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
freq AS (
    SELECT n_name, count(*) AS n, sum(count(*)) OVER () AS total
    FROM nation
    GROUP BY n_name
),
candidates AS (
    SELECT 'nation' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q21_nation', n_name::text) AS parameters
    FROM freq
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
