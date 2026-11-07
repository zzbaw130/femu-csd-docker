-- SF8 target selectivity: 0.5
-- lineitem: 1993668/47989007 (selectivity=0.04154426450207648598)
\timing on

select
	sum(l_extendedprice * l_discount) as revenue
from
	lineitem
where
	l_shipdate >= date '1996-02-27'
	and l_shipdate < date '1996-02-27' + interval '1' year
	and l_discount between '0.05000000000000000000'::numeric - 0.01 and '0.05000000000000000000'::numeric + 0.01
	and l_quantity < '51'::integer;
