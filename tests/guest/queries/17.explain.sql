\timing on
-- using 1489058137 as a seed to the RNG
-- 实验改写：按 l_partkey 预聚合 AVG 后连接，避免 HOST 为每个匹配行重复扫描 lineitem。
-- 原始相关写法在现有索引下冷启动超过 120 秒；默认用此等价写法完成 host/CSD 对照，参数不变。
-- 去相关能力验证请使用 17.original.sql；对应计划脚本为 17.original.explain.sql。

explain select
	sum(l_extendedprice) / 7.0 as avg_yearly
from
	lineitem,
	part,
	(SELECT l_partkey AS agg_partkey, 0.2 * avg(l_quantity) AS avg_quantity FROM lineitem GROUP BY l_partkey) part_agg
where
	p_partkey = l_partkey
	and agg_partkey = l_partkey
	and p_brand = 'Brand#54'
	and p_container = 'JUMBO BAG'
	and l_quantity < avg_quantity;
