-- SF8 target selectivity: 0.1
-- part: 235180/1600000 (selectivity=0.14698750000000000000)
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
	and p_brand <> 'Brand#33'
	and p_type not like ('ECONOMY ANODIZED' || '%')
	and p_size in ('45'::integer, '40'::integer, '31'::integer, '11'::integer, '20'::integer, '12'::integer, '15'::integer, '18'::integer)
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
