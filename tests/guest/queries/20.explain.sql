\timing on
-- using 1489058137 as a seed to the RNG
-- 实验改写：按 (l_partkey, l_suppkey) 预聚合 SUM 后连接，避免 HOST 重复扫描 lineitem。
-- 原始相关写法在现有索引下冷启动超过 120 秒；默认用此等价写法完成 host/CSD 对照，参数不变。
-- 去相关能力验证请使用 20.original.sql；对应计划脚本为 20.original.explain.sql。

explain select
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
					l_shipdate >= date '1996-01-01'
					and l_shipdate < date '1996-01-01' + interval '1' year
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
					p_name like 'goldenrod%'
			)
			and ps_availqty > agg_quantity
	)
	and s_nationkey = n_nationkey
	and n_name = 'JORDAN'
order by
	s_name;
