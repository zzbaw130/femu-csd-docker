-- Independent PG correctness oracle for the original correlated tests/q20.sql.
SELECT s.s_name,s.s_address
FROM supplier s JOIN nation n ON s.s_nationkey=n.n_nationkey
WHERE n.n_name='CANADA' AND s.s_suppkey IN (
  SELECT ps.ps_suppkey
  FROM partsupp ps JOIN part p ON p.p_partkey=ps.ps_partkey
  JOIN (
    SELECT l_partkey,l_suppkey,sum(l_quantity) AS quantity
    FROM lineitem
    WHERE l_shipdate>=date '1994-01-01' AND l_shipdate<date '1995-01-01'
    GROUP BY l_partkey,l_suppkey
  ) a ON a.l_partkey=ps.ps_partkey AND a.l_suppkey=ps.ps_suppkey
  WHERE p.p_name LIKE 'forest%' AND ps.ps_availqty>0.5*a.quantity
) ORDER BY s.s_name;
