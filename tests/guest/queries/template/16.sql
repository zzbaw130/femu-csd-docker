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
	and p_brand <> :'q16_excluded_brand'
	and p_type not like (:'q16_excluded_type_prefix' || '%')
	and p_size in (:'q16_part_size_1'::integer, :'q16_part_size_2'::integer, :'q16_part_size_3'::integer, :'q16_part_size_4'::integer, :'q16_part_size_5'::integer, :'q16_part_size_6'::integer, :'q16_part_size_7'::integer, :'q16_part_size_8'::integer)
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
