SELECT l_partkey,l_suppkey,l_quantity,l_extendedprice
FROM lineitem
WHERE l_shipdate>=date '1995-01-01' AND l_shipdate<date '1996-01-01'
ORDER BY l_extendedprice DESC,l_partkey,l_suppkey,l_quantity
LIMIT 12000 OFFSET 10000;
