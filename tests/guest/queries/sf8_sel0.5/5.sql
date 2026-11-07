-- SF8 target selectivity: 0.5
-- orders: 1827092/12000000 (selectivity=0.15225766666666666667)
-- region: 1/5 (selectivity=0.20000000000000000000)
\timing on

select
	n_name,
	sum(l_extendedprice * (1 - l_discount)) as revenue
from
	customer,
	orders,
	lineitem,
	supplier,
	nation,
	region
where
	c_custkey = o_custkey
	and l_orderkey = o_orderkey
	and l_suppkey = s_suppkey
	and c_nationkey = s_nationkey
	and s_nationkey = n_nationkey
	and n_regionkey = r_regionkey
	and r_name = 'AFRICA'
	and o_orderdate >= date '1996-01-26'
	and o_orderdate < date '1996-01-26' + interval '1' year
group by
	n_name
order by
	revenue desc;
