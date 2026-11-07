-- 同一个截止日同时约束 orders 与 lineitem，优先最小化两张表选择率中较大的绝对误差。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
order_days AS (
    SELECT o_orderdate AS day, count(*) AS n FROM orders GROUP BY o_orderdate
),
ship_days AS (
    SELECT l_shipdate AS day, count(*) AS n FROM lineitem GROUP BY l_shipdate
),
days AS (
    SELECT coalesce(o.day, l.day) AS day, coalesce(o.n, 0) AS orders, coalesce(l.n, 0) AS lines
    FROM order_days o FULL JOIN ship_days l USING (day)
),
calendar AS (
    SELECT generate_series(min(day) - 1, max(day) + 1, interval '1 day')::date AS day FROM days
),
rates AS (
    SELECT calendar.day,
           sum(coalesce(orders, 0)) OVER () AS orders_total,
           sum(coalesce(lines, 0)) OVER () AS lines_total,
           sum(coalesce(orders, 0)) OVER (ORDER BY calendar.day) - coalesce(orders, 0) AS orders_matched,
           sum(coalesce(lines, 0)) OVER (ORDER BY calendar.day DESC) - coalesce(lines, 0) AS lines_matched
    FROM calendar LEFT JOIN days USING (day)
),
cutoff AS (
    SELECT rates.* FROM rates CROSS JOIN target
    WHERE orders_total > 0 AND lines_total > 0
    ORDER BY greatest(abs(orders_matched::numeric / orders_total - value),
                      abs(lines_matched::numeric / lines_total - value)),
             abs(orders_matched::numeric / orders_total - value)
           + abs(lines_matched::numeric / nullif(lines_total, 0) - value), day
    LIMIT 1
),
segments AS (
    SELECT c_mktsegment, count(*) AS n, sum(count(*)) OVER () AS total
    FROM customer
    GROUP BY c_mktsegment
),
candidates AS (
    SELECT 'customer' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q3_market_segment', c_mktsegment::text) AS parameters
    FROM segments
    UNION ALL
    SELECT 'orders' AS filter_scope, orders_matched AS matched_rows, orders_total AS input_rows,
           jsonb_build_object('q3_cutoff_date', day) AS parameters
    FROM cutoff
    UNION ALL
    SELECT 'lineitem' AS filter_scope, lines_matched AS matched_rows, lines_total AS input_rows,
           jsonb_build_object('q3_cutoff_date', day) AS parameters
    FROM cutoff
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
