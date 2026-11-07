-- 联合搜索品牌/类型排除项及 8 个不同尺寸。用最小和最大可达行数剪枝。
WITH RECURSIVE target AS (
    SELECT :'target'::numeric AS value
    WHERE :'target'::numeric BETWEEN 0 AND 1
),
parts AS (
    SELECT p_brand AS brand, regexp_replace(p_type, ' [^ ]+$', '') AS prefix,
           p_size AS size, count(*) AS n
    FROM part GROUP BY 1, 2, 3
),
by_size AS (
    SELECT size, sum(n) AS n FROM parts GROUP BY size
),
by_brand AS (
    SELECT brand, size, sum(n) AS n FROM parts GROUP BY brand, size
),
by_type AS (
    SELECT prefix, size, sum(n) AS n FROM parts GROUP BY prefix, size
),
eligible AS (
    SELECT brands.brand, types.prefix, s.size,
           s.n - coalesce(b.n, 0) - coalesce(t.n, 0) + coalesce(p.n, 0) AS n
    FROM (SELECT DISTINCT brand FROM parts) brands
    CROSS JOIN (SELECT DISTINCT prefix FROM parts) types
    CROSS JOIN by_size s
    LEFT JOIN by_brand b ON b.brand = brands.brand AND b.size = s.size
    LEFT JOIN by_type t ON t.prefix = types.prefix AND t.size = s.size
    LEFT JOIN parts p ON p.brand = brands.brand AND p.prefix = types.prefix AND p.size = s.size
),
ranked AS (
    SELECT *, sum(n) OVER (PARTITION BY brand, prefix ORDER BY n, size) AS cumulative
    FROM eligible
),
families AS (
    SELECT brand, prefix, array_agg(size ORDER BY n, size) AS sizes,
           ARRAY[0::numeric] || array_agg(cumulative ORDER BY n, size) AS sums,
           count(*)::integer AS size_count
    FROM ranked
    GROUP BY brand, prefix
    HAVING count(*) >= 8
),
goal AS (
    SELECT sum(n) * value AS wanted FROM by_size CROSS JOIN target GROUP BY value
),
seed AS (
    -- Consecutive windows give a bound and include the exact extremes.
    SELECT brand, prefix, sizes[i:i + 7] AS picked, sums[i + 8] - sums[i] AS n,
           wanted, abs(sums[i + 8] - sums[i] - wanted) AS error
    FROM families CROSS JOIN goal
    CROSS JOIN LATERAL generate_series(1, size_count - 7) AS g(i)
    ORDER BY error, brand, prefix, picked
    LIMIT 1
),
search AS (
    SELECT f.*, 0 AS last_size, ARRAY[]::integer[] AS picked, 0::numeric AS n
    FROM families f CROSS JOIN seed
    WHERE seed.error > abs(round(wanted) - wanted)
      AND sums[9] < wanted + seed.error
      AND sums[size_count + 1] - sums[size_count - 7] > wanted - seed.error
    UNION ALL
    SELECT s.brand, s.prefix, s.sizes, s.sums, s.size_count,
           i, s.picked || s.sizes[i], s.n + s.sums[i + 1] - s.sums[i]
    FROM search s CROSS JOIN seed
    CROSS JOIN LATERAL (SELECT 7 - cardinality(s.picked) AS remaining) r
    CROSS JOIN LATERAL generate_series(last_size + 1, size_count - remaining) AS g(i)
    WHERE remaining >= 0
      AND s.n + s.sums[i + remaining + 1] - s.sums[i] < wanted + seed.error
      AND s.n + s.sums[i + 1] - s.sums[i]
          + s.sums[size_count + 1] - s.sums[size_count - remaining + 1] > wanted - seed.error
),
best AS (
    SELECT brand, prefix, picked, n FROM seed
    UNION ALL
    SELECT brand, prefix, picked, n FROM search WHERE cardinality(picked) = 8
),
candidates AS (
    SELECT 'part' AS filter_scope, n AS matched_rows, (SELECT sum(n) FROM by_size) AS input_rows,
           jsonb_build_object('q16_excluded_brand', brand::text, 'q16_excluded_type_prefix', prefix,
                              'q16_part_size_1', picked[1], 'q16_part_size_2', picked[2],
                              'q16_part_size_3', picked[3], 'q16_part_size_4', picked[4],
                              'q16_part_size_5', picked[5], 'q16_part_size_6', picked[6],
                              'q16_part_size_7', picked[7], 'q16_part_size_8', picked[8]) AS parameters
    FROM best
)
SELECT DISTINCT ON (filter_scope)
    filter_scope, value AS target_selectivity,
    matched_rows::numeric / nullif(input_rows, 0) AS actual_selectivity, matched_rows, input_rows, parameters
FROM candidates CROSS JOIN target
WHERE input_rows > 0
ORDER BY filter_scope, abs(matched_rows::numeric / nullif(input_rows, 0) - value), parameters;
