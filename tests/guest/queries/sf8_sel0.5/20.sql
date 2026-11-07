-- SF8 target selectivity: 0.5
-- lineitem: 7304395/47989007 (selectivity=0.15220975503827366130)
-- nation: 1/25 (selectivity=0.04000000000000000000)
-- part: 17625/1600000 (selectivity=0.01101562500000000000)
\timing on
-- 实验改写：按 (l_partkey, l_suppkey) 预聚合 SUM 后连接，避免 HOST 重复扫描 lineitem。
-- 对照实验执行下方改写，原始相关查询附在文件末尾，使用同一组参数。

select
	s_name,
	s_address
from
	supplier,
	nation
where
	s_suppkey in (
		select
			ps_suppkey
		from
			partsupp,
			(
				select
					l_partkey agg_partkey,
					l_suppkey agg_suppkey,
					0.5 * sum(l_quantity) AS agg_quantity
				from
					lineitem
				where
					l_shipdate >= date '1996-02-28'
					and l_shipdate < date '1996-02-28' + interval '1' year
				group by
					l_partkey,
					l_suppkey
			) agg_lineitem
		where
			agg_partkey = ps_partkey
			and agg_suppkey = ps_suppkey
			and ps_partkey in (
				select
					p_partkey
				from
					part
				where
					p_name like ('tomato' || '%')
			)
			and ps_availqty > agg_quantity
	)
	and s_nationkey = n_nationkey
	and n_name = 'ALGERIA'
order by
	s_name;

/* 原始查询
select
	s_name,
	s_address
from
	supplier,
	nation
where
	s_suppkey in (
		select
			ps_suppkey
		from
			partsupp
		where
			ps_partkey in (
				select
					p_partkey
				from
					part
				where
					p_name like ('tomato' || '%')
			)
			and ps_availqty > (
				select
					0.5 * sum(l_quantity)
				from
					lineitem
				where
					l_partkey = ps_partkey
					and l_suppkey = ps_suppkey
					and l_shipdate >= date '1996-02-28'
					and l_shipdate < date '1996-02-28' + interval '1' year
			)
	)
	and s_nationkey = n_nationkey
	and n_name = 'ALGERIA'
order by
	s_name;
*/
