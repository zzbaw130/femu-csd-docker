-- 对每个实际出现的颜色词按原 LIKE 子串语义计数，每个 part 最多计一次。
WITH target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
colors AS (
    SELECT DISTINCT word FROM part CROSS JOIN LATERAL unnest(string_to_array(p_name, ' ')) AS w(word)
),
matching_words AS MATERIALIZED (
    SELECT token.word AS token, color.word AS color
    FROM colors token JOIN colors color ON token.word LIKE '%' || color.word || '%'
),
matches AS (
    SELECT m.color, count(DISTINCT p_partkey) AS n
    FROM part
    CROSS JOIN LATERAL unnest(string_to_array(p_name, ' ')) AS w(word)
    JOIN matching_words m ON m.token = w.word
    GROUP BY m.color
),
candidates AS MATERIALIZED (
    SELECT 'part' AS filter_scope, n AS matched_rows, (SELECT count(*) FROM part) AS input_rows,
           jsonb_build_object('q9_part_name_word', color) AS parameters
    FROM matches
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
