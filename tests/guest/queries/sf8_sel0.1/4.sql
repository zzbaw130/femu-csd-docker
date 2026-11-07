-- SF8 target selectivity: 0.1
-- orders: 460562/12000000 (selectivity=0.03838016666666666667)
\timing on

select
	o_orderpriority,
	count(*) as order_count
from
	orders
where
	o_orderdate >= date '1992-11-21'
	and o_orderdate < date '1992-11-21' + interval '3' month
	and exists (
		select
			*
		from
			lineitem
		where
			l_orderkey = o_orderkey
			and l_commitdate < l_receiptdate
	)
group by
	o_orderpriority
order by
	o_orderpriority;
