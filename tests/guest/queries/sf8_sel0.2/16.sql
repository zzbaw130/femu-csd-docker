-- SF8 target selectivity: 0.2
-- part: 240500/1600000 (selectivity=0.15031250000000000000)
\timing on

select
	p_brand,
	p_type,
	p_size,
	count(distinct ps_suppkey) as supplier_cnt
from
	partsupp,
	part
where
	p_partkey = ps_partkey
	and p_brand <> 'Brand#44'
	and p_type not like ('PROMO BURNISHED' || '%')
	and p_size in ('22'::integer, '47'::integer, '16'::integer, '42'::integer, '48'::integer, '35'::integer, '24'::integer, '38'::integer)
	and ps_suppkey not in (
		select
			s_suppkey
		from
			supplier
		where
			s_comment like '%Customer%Complaints%'
	)
group by
	p_brand,
	p_type,
	p_size
order by
	supplier_cnt desc,
	p_brand,
	p_type,
	p_size;
