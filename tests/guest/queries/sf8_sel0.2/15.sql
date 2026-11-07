-- SF8 target selectivity: 0.2
-- lineitem: 1840126/47989007 (selectivity=0.03834474007765986906)
\timing on

create view revenue (supplier_no, total_revenue) as
	select
		l_suppkey,
		sum(l_extendedprice * (1 - l_discount))
	from
		lineitem
	where
		l_shipdate >= date '1996-08-30'
		and l_shipdate < date '1996-08-30' + interval '3' month
	group by
		l_suppkey;


select
	s_suppkey,
	s_name,
	s_address,
	s_phone,
	total_revenue
from
	supplier,
	revenue
where
	s_suppkey = supplier_no
	and total_revenue = (
		select
			max(total_revenue)
		from
			revenue
	)
order by
	s_suppkey;

drop view revenue;
