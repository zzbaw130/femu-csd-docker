-- SF8 target selectivity: 0.2
-- customer: 239867/1200000 (selectivity=0.19988916666666666667)
-- lineitem: 24592682/47989007 (selectivity=0.51246490680667762098)
-- orders: 6148382/12000000 (selectivity=0.51236516666666666667)
\timing on

select
	l_orderkey,
	sum(l_extendedprice * (1 - l_discount)) as revenue,
	o_orderdate,
	o_shippriority
from
	customer,
	orders,
	lineitem
where
	c_mktsegment = 'AUTOMOBILE'
	and c_custkey = o_custkey
	and l_orderkey = o_orderkey
	and o_orderdate < date '1995-05-18'
	and l_shipdate > date '1995-05-18'
group by
	l_orderkey,
	o_orderdate,
	o_shippriority
order by
	revenue desc,
	o_orderdate
LIMIT 10;
