-- 按 QGEN 的 16 个词组统计 orders 的 ON 条件，分母为 orders 全表。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
patterns AS (
    SELECT word_1, word_2, '%' || word_1 || '%' || word_2 || '%' AS pattern
    FROM (VALUES ('special'), ('pending'), ('unusual'), ('express')) a(word_1)
    CROSS JOIN (VALUES ('packages'), ('requests'), ('accounts'), ('deposits')) b(word_2)
),
counts AS (
    SELECT word_1, word_2, count(*) FILTER (WHERE o_comment NOT LIKE pattern) AS n, count(*) AS total
    FROM orders CROSS JOIN patterns
    GROUP BY word_1, word_2
),
candidates AS (
    SELECT 'orders (JOIN ON)' AS filter_scope, n AS matched_rows, total AS input_rows,
           jsonb_build_object('q13_comment_word_1', word_1, 'q13_comment_word_2', word_2) AS parameters
    FROM counts
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
