-- SF8 target selectivity: 0.8
-- nation: 1/25 (selectivity=0.04000000000000000000)
-- partsupp HAVING (part groups): 189658/237072 (selectivity=0.80000168725113045826)
\timing on

select
	ps_partkey,
	sum(ps_supplycost * ps_availqty) as value
from
	partsupp,
	supplier,
	nation
where
	ps_suppkey = s_suppkey
	and s_nationkey = n_nationkey
	and n_name = 'ALGERIA'
group by
	ps_partkey having
		sum(ps_supplycost * ps_availqty) > (
			select
				sum(ps_supplycost * ps_availqty) * '0.000000863386662037682796'::numeric
			from
				partsupp,
				supplier,
				nation
			where
				ps_suppkey = s_suppkey
				and s_nationkey = n_nationkey
				and n_name = 'ALGERIA'
		)
order by
	value desc;
