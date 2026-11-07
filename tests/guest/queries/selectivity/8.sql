-- q8_nation 只影响 CASE 聚合值，返回 NULL；其余两个参数各自过滤一张表。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
types AS (
    SELECT p_type, count(*) AS n, sum(count(*)) OVER () AS total
    FROM part
    GROUP BY p_type
),
regions AS (
    SELECT r_name, count(*) AS n, sum(count(*)) OVER () AS total
    FROM region
    GROUP BY r_name
),
selected_region AS (
    SELECT regions.* FROM regions CROSS JOIN target
    ORDER BY abs(n::numeric / total - value), (r_name = 'MIDDLE EAST') DESC, r_name
    LIMIT 1
),
candidates AS (
    SELECT 'part' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q8_part_type', p_type::text) AS parameters
    FROM types
    UNION ALL
    SELECT 'region' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q8_region', r_name::text, 'q8_nation', NULL::text) AS parameters
    FROM selected_region
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
