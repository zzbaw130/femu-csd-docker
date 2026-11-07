-- 两处共用七个不同电话区号。内层计入余额为正的条件，外层统计标量子查询及反连接之前的区号过滤。
WITH RECURSIVE target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
prefixes AS (
    SELECT substring(c_phone FROM 1 FOR 2) AS code, count(*) AS n, sum(count(*)) OVER () AS total,
           count(*) FILTER (WHERE c_acctbal > 0.00) AS positive
    FROM customer GROUP BY 1
),
combinations AS (
    SELECT ARRAY[code] AS codes, code AS last_code, n, total, positive FROM prefixes
    UNION ALL
    SELECT codes || p.code, p.code, c.n + p.n, c.total, c.positive + p.positive
    FROM combinations c JOIN prefixes p ON p.code > c.last_code
    WHERE cardinality(codes) < 7
),
best AS (
    SELECT combinations.* FROM combinations CROSS JOIN target
    WHERE cardinality(codes) = 7 AND total > 0
    ORDER BY greatest(abs(n::numeric / total - value), abs(positive::numeric / total - value)),
             abs(n::numeric / total - value) + abs(positive::numeric / total - value), codes
    LIMIT 1
),
candidates AS (
    SELECT filter_scope, matched_rows, total AS input_rows,
           jsonb_build_object('q22_phone_prefix_1', codes[1], 'q22_phone_prefix_2', codes[2],
                              'q22_phone_prefix_3', codes[3], 'q22_phone_prefix_4', codes[4],
                              'q22_phone_prefix_5', codes[5], 'q22_phone_prefix_6', codes[6],
                              'q22_phone_prefix_7', codes[7]) AS parameters
    FROM best
    CROSS JOIN LATERAL (VALUES ('customer (outer phone prefix)', n),
                               ('customer (inner positive balance and prefix)', positive))
                       AS scopes(filter_scope, matched_rows)
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
