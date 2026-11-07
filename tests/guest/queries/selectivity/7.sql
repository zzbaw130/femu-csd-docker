-- 两个 nation 别名均可先用两个不同国家过滤，每个别名的选择率固定。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
nations AS (
    SELECT n_name, count(*) OVER () AS total FROM nation
),
pairs AS (
    SELECT a.n_name AS nation_1, b.n_name AS nation_2, a.total
    FROM nations a JOIN nations b ON a.n_name < b.n_name
),
candidates AS (
    SELECT 'nation (each alias)' AS filter_scope, 2 AS matched_rows, total AS input_rows,
           jsonb_build_object('q7_nation_1', nation_1::text, 'q7_nation_2', nation_2::text) AS parameters
    FROM pairs
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
