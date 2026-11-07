-- SF8 target selectivity: 0.8
-- part: 1751/1600000 (selectivity=0.00109437500000000000)
\timing on
-- 实验改写：按 l_partkey 预聚合 AVG 后连接，避免 HOST 重复扫描 lineitem。
-- 对照实验执行下方改写，原始相关查询附在文件末尾，使用同一组参数。

select
	sum(l_extendedprice) / 7.0 as avg_yearly
from
	lineitem,
	part,
	(SELECT l_partkey AS agg_partkey, 0.2 * avg(l_quantity) AS avg_quantity FROM lineitem GROUP BY l_partkey) part_agg
where
	p_partkey = l_partkey
	and agg_partkey = l_partkey
	and p_brand = 'Brand#12'
	and p_container = 'LG PKG'
	and l_quantity < avg_quantity;

/* 原始查询
select
	sum(l_extendedprice) / 7.0 as avg_yearly
from
	lineitem,
	part
where
	p_partkey = l_partkey
	and p_brand = 'Brand#12'
	and p_container = 'LG PKG'
	and l_quantity < (
		select
			0.2 * avg(l_quantity)
		from
			lineitem
		where
			l_partkey = p_partkey
	);
*/
