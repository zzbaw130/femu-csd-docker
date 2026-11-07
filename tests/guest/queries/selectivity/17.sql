-- 联合校准品牌与容器，分母为 part 全表。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
counts AS (
    SELECT p_brand, p_container, count(*) AS n FROM part GROUP BY p_brand, p_container
),
pairs AS (
    SELECT b.p_brand, c.p_container, coalesce(n, 0) AS n, (SELECT sum(n) FROM counts) AS total
    FROM (SELECT DISTINCT p_brand FROM counts) b
    CROSS JOIN (SELECT DISTINCT p_container FROM counts) c
    LEFT JOIN counts USING (p_brand, p_container)
),
candidates AS (
    SELECT 'part' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q17_brand', p_brand::text, 'q17_container', p_container::text) AS parameters
    FROM pairs
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
