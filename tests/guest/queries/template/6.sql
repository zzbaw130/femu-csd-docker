\timing on

select
	sum(l_extendedprice * l_discount) as revenue
from
	lineitem
where
	l_shipdate >= date :'q6_start_date'
	and l_shipdate < date :'q6_start_date' + interval '1' year
	and l_discount between :'q6_discount'::numeric - 0.01 and :'q6_discount'::numeric + 0.01
	and l_quantity < :'q6_quantity_limit'::integer;
