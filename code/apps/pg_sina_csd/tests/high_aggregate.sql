SELECT l_partkey,
       sum(l_extendedprice * (1 - l_discount)) AS revenue,
       avg(l_quantity), count(*)
FROM lineitem
WHERE l_shipdate >= date '1995-01-01'
  AND l_shipdate < date '1996-01-01'
GROUP BY l_partkey
ORDER BY revenue DESC, l_partkey
LIMIT 100;
