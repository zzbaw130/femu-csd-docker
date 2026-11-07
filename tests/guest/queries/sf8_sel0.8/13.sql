-- SF8 target selectivity: 0.8
-- orders (JOIN ON): 11866559/12000000 (selectivity=0.98887991666666666667)
\timing on

select
	c_count,
	count(*) as custdist
from
	(
		select
			c_custkey,
			count(o_orderkey)
		from
			customer left outer join orders on
				c_custkey = o_custkey
				and o_comment not like ('%' || 'unusual' || '%' || 'packages' || '%')
		group by
			c_custkey
	) as c_orders (c_custkey, c_count)
group by
	c_count
order by
	custdist desc,
	c_count desc;
