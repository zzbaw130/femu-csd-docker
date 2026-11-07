-- 联合搜索两个运输方式和一年收货窗口，计入同表的发货/承诺/收货日期约束。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
daily AS (
    SELECT l_receiptdate AS day, l_shipmode AS mode, count(*) AS n,
           count(*) FILTER (WHERE l_commitdate < l_receiptdate AND l_shipdate < l_commitdate) AS eligible
    FROM lineitem
    GROUP BY 1, 2
),
bounds AS (
    SELECT min(day) AS first_day, max(day) AS last_day, sum(n) AS total FROM daily
),
modes AS (
    SELECT DISTINCT mode FROM daily
),
prefix AS (
    SELECT d::date AS day, modes.mode, total, last_day,
           sum(coalesce(eligible, 0)) OVER (PARTITION BY modes.mode ORDER BY d)
               - coalesce(eligible, 0) AS before_day
    FROM bounds
    CROSS JOIN LATERAL generate_series(first_day - interval '1 year' - interval '1 day',
                                       last_day + interval '1 year' + interval '2 days', interval '1 day') AS g(d)
    CROSS JOIN modes
    LEFT JOIN daily ON daily.day = d::date AND daily.mode = modes.mode
),
windows AS (
    SELECT a.day, a.mode, a.total, b.before_day - a.before_day AS n
    FROM prefix a JOIN prefix b ON b.day = (a.day + interval '1 year')::date AND b.mode = a.mode
    WHERE a.day <= a.last_day + 1
),
candidates AS (
    SELECT 'lineitem' AS filter_scope, a.n + b.n AS matched_rows, a.total AS input_rows,
           jsonb_build_object('q12_ship_mode_1', a.mode::text, 'q12_ship_mode_2', b.mode::text, 'q12_start_date', a.day) AS parameters
    FROM windows a JOIN windows b ON a.day = b.day AND a.mode < b.mode
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
