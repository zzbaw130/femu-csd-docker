-- 阈值作用于 HAVING，分母为按订单号聚合后的总组数。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
order_quantities AS (
    SELECT sum(l_quantity)::integer AS quantity FROM lineitem GROUP BY l_orderkey
),
histogram AS (
    SELECT quantity, count(*) AS n FROM order_quantities GROUP BY quantity
),
cuts AS (
    SELECT quantity, sum(n) OVER (ORDER BY quantity DESC) - n AS matched,
           sum(n) OVER () AS total
    FROM histogram
    UNION ALL
    SELECT min(quantity) - 1, sum(n), sum(n) FROM histogram
),
candidates AS (
    SELECT 'lineitem HAVING (order groups)' AS filter_scope, matched AS matched_rows, total AS input_rows,
           jsonb_build_object('q18_total_quantity_limit', quantity) AS parameters
    FROM cuts
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
