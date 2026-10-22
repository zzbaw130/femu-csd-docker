-- Independent PG correctness oracle. The original correlated query remains
-- tests/q17.sql and is the query submitted to the CSD planner.
SELECT sum(l.l_extendedprice) / 7.0 AS avg_yearly
FROM lineitem l
JOIN part p ON p.p_partkey=l.l_partkey
JOIN (SELECT l_partkey,avg(l_quantity) AS quantity FROM lineitem GROUP BY l_partkey) a
  ON a.l_partkey=p.p_partkey
WHERE p.p_brand='Brand#23' AND p.p_container='MED BOX'
  AND l.l_quantity<0.2*a.quantity;
