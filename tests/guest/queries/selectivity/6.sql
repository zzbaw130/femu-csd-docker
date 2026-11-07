-- 在日、百分位折扣和整数数量阈值上联合搜索，分母为 lineitem 全表。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
daily AS (
    SELECT l_shipdate AS day, (l_discount * 100)::integer AS discount,
           l_quantity::integer AS quantity, count(*) AS n
    FROM lineitem
    GROUP BY 1, 2, 3
),
bounds AS (
    SELECT min(day) AS first_day, max(day) AS last_day, min(quantity) AS min_qty,
           max(quantity) AS max_qty, min(discount) AS min_discount,
           max(discount) AS max_discount, sum(n) AS total
    FROM daily
),
bands AS (
    SELECT day, discount + offset_cent AS center, quantity, sum(n) AS n
    FROM daily CROSS JOIN generate_series(-1, 1) AS offsets(offset_cent)
    GROUP BY 1, 2, 3
),
by_quantity AS (
    SELECT d::date AS day, discounts.center, threshold, total, last_day,
           sum(coalesce(n, 0)) OVER (PARTITION BY d, discounts.center ORDER BY threshold)
               - coalesce(n, 0) AS eligible
    FROM bounds
    CROSS JOIN LATERAL generate_series(first_day - interval '1 year' - interval '1 day',
                                       last_day + interval '1 year' + interval '2 days', interval '1 day') AS days(d)
    CROSS JOIN LATERAL generate_series(greatest(0, min_discount - 1), max_discount + 1) AS discounts(center)
    CROSS JOIN LATERAL generate_series(min_qty, max_qty + 1) AS quantities(threshold)
    LEFT JOIN bands ON bands.day = d::date AND bands.center = discounts.center
                   AND bands.quantity = threshold
),
prefix AS (
    SELECT day, center, threshold, total, last_day,
           sum(eligible) OVER (PARTITION BY center, threshold ORDER BY day) - eligible AS before_day
    FROM by_quantity
),
windows AS (
    SELECT a.day, a.center / 100.0 AS discount, a.threshold, a.total,
           b.before_day - a.before_day AS n
    FROM prefix a JOIN prefix b ON b.day = (a.day + interval '1 year')::date
                               AND b.center = a.center AND b.threshold = a.threshold
    WHERE a.day <= a.last_day + 1
),
best AS (
    SELECT windows.* FROM windows CROSS JOIN target
    WHERE total > 0
    ORDER BY abs(n::numeric / total - value), day, discount, threshold
    LIMIT 1
),
candidates AS (
    SELECT 'lineitem' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q6_start_date', day, 'q6_discount', discount, 'q6_quantity_limit', threshold) AS parameters
    FROM best
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
