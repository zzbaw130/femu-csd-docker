-- 分别校准 part 的尺寸与类型后缀，以及 region 的名称过滤。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
part_types AS (
    SELECT p_size, p_type, count(*) AS n FROM part GROUP BY p_size, p_type
),
suffixes AS (
    SELECT DISTINCT regexp_replace(p_type, '^.* ', '') AS suffix FROM part_types
),
part_matches AS (
    SELECT sizes.p_size, suffix, coalesce(sum(n), 0) AS n
    FROM (SELECT DISTINCT p_size FROM part_types) sizes CROSS JOIN suffixes
    LEFT JOIN part_types p ON p.p_size = sizes.p_size AND p_type LIKE '%' || suffix
    GROUP BY sizes.p_size, suffix
),
regions AS (
    SELECT r_name, count(*) AS n, sum(count(*)) OVER () AS total
    FROM region
    GROUP BY r_name
),
candidates AS (
    SELECT 'part' AS filter_scope, n AS matched_rows, (SELECT sum(n) FROM part_types) AS input_rows,
           jsonb_build_object('q2_part_size', p_size, 'q2_type_suffix', suffix) AS parameters
    FROM part_matches
    UNION ALL
    SELECT 'region' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q2_region', r_name::text) AS parameters
    FROM regions
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
