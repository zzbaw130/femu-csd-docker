-- SF8 target selectivity: 0.5
-- part: 10973/1600000 (selectivity=0.00685812500000000000)
-- region: 1/5 (selectivity=0.20000000000000000000)
\timing on

select
	o_year,
	sum(case
		when nation = 'JORDAN' then volume
		else 0
	end) / sum(volume) as mkt_share
from
	(
		select
			extract(year from o_orderdate) as o_year,
			l_extendedprice * (1 - l_discount) as volume,
			n2.n_name as nation
		from
			part,
			supplier,
			lineitem,
			orders,
			customer,
			nation n1,
			nation n2,
			region
		where
			p_partkey = l_partkey
			and s_suppkey = l_suppkey
			and l_orderkey = o_orderkey
			and o_custkey = c_custkey
			and c_nationkey = n1.n_nationkey
			and n1.n_regionkey = r_regionkey
			and r_name = 'MIDDLE EAST'
			and s_nationkey = n2.n_nationkey
			and o_orderdate between date '1995-01-01' and date '1996-12-31'
			and p_type = 'ECONOMY PLATED STEEL'
	) as all_nations
group by
	o_year
order by
	o_year;
