-- 分别校准 part 名称前缀、lineitem 日期和 nation 名称。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
daily AS (
    SELECT l_shipdate AS day, count(*) AS n
    FROM lineitem
    GROUP BY l_shipdate
),
bounds AS (
    SELECT min(day) AS first_day, max(day) AS last_day, sum(n) AS total FROM daily
),
prefix AS (
    SELECT d::date AS day, total, last_day,
           sum(coalesce(n, 0)) OVER (ORDER BY d) - coalesce(n, 0) AS before_day
    FROM bounds
    CROSS JOIN LATERAL generate_series(first_day - interval '1 year' - interval '1 day',
                                       (last_day + 1) + interval '1 year', interval '1 day') AS g(d)
    LEFT JOIN daily ON daily.day = d::date
),
windows AS (
    SELECT a.day, b.before_day - a.before_day AS n, a.total
    FROM prefix a JOIN prefix b ON b.day = (a.day + interval '1 year')::date
    WHERE a.day <= a.last_day + 1
),
nations AS (
    SELECT n_name, count(*) AS n, sum(count(*)) OVER () AS total
    FROM nation
    GROUP BY n_name
),
first_words AS (
    SELECT split_part(p_name, ' ', 1) AS word, count(*) AS n FROM part GROUP BY 1
),
colors AS (
    SELECT DISTINCT word FROM part CROSS JOIN LATERAL unnest(string_to_array(p_name, ' ')) AS w(word)
),
prefix_counts AS (
    SELECT c.word, coalesce(sum(f.n), 0) AS n
    FROM colors c LEFT JOIN first_words f ON f.word LIKE c.word || '%'
    GROUP BY c.word
),
candidates AS (
    SELECT 'part' AS filter_scope, n AS matched_rows, (SELECT sum(n) FROM first_words) AS input_rows,
           jsonb_build_object('q20_part_name_prefix', word) AS parameters
    FROM prefix_counts
    UNION ALL
    SELECT 'lineitem' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q20_start_date', day) AS parameters
    FROM windows
    UNION ALL
    SELECT 'nation' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q20_nation', n_name::text) AS parameters
    FROM nations
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
