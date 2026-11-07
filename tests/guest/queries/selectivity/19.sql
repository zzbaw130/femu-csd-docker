-- 分别校准两张表的 OR 分支投影，重叠数量区间只计一次。分支配对的最终 JOIN 选择率不在此估计。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
brands AS (
    SELECT p_brand AS brand,
           count(*) FILTER (WHERE p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') AND p_size BETWEEN 1 AND 5) AS small,
           count(*) FILTER (WHERE p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') AND p_size BETWEEN 1 AND 10) AS medium,
           count(*) FILTER (WHERE p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') AND p_size BETWEEN 1 AND 15) AS large,
           sum(count(*)) OVER () AS total
    FROM part GROUP BY p_brand
),
quantities AS (
    SELECT l_quantity AS quantity,
           count(*) FILTER (WHERE l_shipmode IN ('AIR', 'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON') AS n,
           sum(count(*)) OVER () AS total
    FROM lineitem GROUP BY l_quantity
),
ranges AS (
    SELECT low, mid, high, max(total) AS total,
           sum(n) FILTER (WHERE quantity BETWEEN low AND low + 10
                              OR quantity BETWEEN mid AND mid + 10
                              OR quantity BETWEEN high AND high + 10) AS n
    FROM generate_series(1, 10) AS a(low)
    CROSS JOIN generate_series(10, 20) AS b(mid)
    CROSS JOIN generate_series(20, 30) AS c(high)
    CROSS JOIN quantities
    GROUP BY low, mid, high
),
candidates AS (
    SELECT 'part' AS filter_scope, a.small + b.medium + c.large AS matched_rows, a.total AS input_rows,
           jsonb_build_object('q19_small_brand', a.brand::text, 'q19_medium_brand', b.brand::text, 'q19_large_brand', c.brand::text) AS parameters
    FROM brands a CROSS JOIN brands b CROSS JOIN brands c
    UNION ALL
    SELECT 'lineitem' AS filter_scope, coalesce(n, 0) AS matched_rows, total AS input_rows,
           jsonb_build_object('q19_small_min_quantity', low, 'q19_medium_min_quantity', mid, 'q19_large_min_quantity', high) AS parameters
    FROM ranges
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
