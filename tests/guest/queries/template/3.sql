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
	c_mktsegment = :'q3_market_segment'
	and c_custkey = o_custkey
	and l_orderkey = o_orderkey
	and o_orderdate < date :'q3_cutoff_date'
	and l_shipdate > date :'q3_cutoff_date'
group by
	l_orderkey,
	o_orderdate,
	o_shippriority
order by
	revenue desc,
	o_orderdate
LIMIT 10;
