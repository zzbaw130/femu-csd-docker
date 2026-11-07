-- 按日搜索 3 months 窗口，分母为 orders 全表。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
daily AS (
    SELECT o_orderdate AS day, count(*) AS n
    FROM orders
    GROUP BY o_orderdate
),
bounds AS (
    SELECT min(day) AS first_day, max(day) AS last_day, sum(n) AS total FROM daily
),
prefix AS (
    SELECT d::date AS day, total, last_day,
           sum(coalesce(n, 0)) OVER (ORDER BY d) - coalesce(n, 0) AS before_day
    FROM bounds
    CROSS JOIN LATERAL generate_series(first_day - interval '3 months' - interval '1 day',
                                       (last_day + 1) + interval '3 months', interval '1 day') AS g(d)
    LEFT JOIN daily ON daily.day = d::date
),
windows AS (
    SELECT a.day, b.before_day - a.before_day AS n, a.total
    FROM prefix a JOIN prefix b ON b.day = (a.day + interval '3 months')::date
    WHERE a.day <= a.last_day + 1
),
candidates AS (
    SELECT 'orders' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q10_start_date', day) AS parameters
    FROM windows
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
