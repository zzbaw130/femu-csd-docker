-- SF8 target selectivity: 0.8
-- lineitem: 621498/47989007 (selectivity=0.01295084101240102759)
\timing on

select
	100.00 * sum(case
		when p_type like 'PROMO%'
			then l_extendedprice * (1 - l_discount)
		else 0
	end) / sum(l_extendedprice * (1 - l_discount)) as promo_revenue
from
	lineitem,
	part
where
	l_partkey = p_partkey
	and l_shipdate >= date '1995-03-17'
	and l_shipdate < date '1995-03-17' + interval '1' month;
