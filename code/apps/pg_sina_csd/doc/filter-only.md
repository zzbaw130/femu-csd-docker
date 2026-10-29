# filter-only 执行与返回

该模式只卸载基表扫描、过滤和必要的列提取；Join、Sort、Aggregate 继续由 PG 执行。实现不根据查询编号选择算法。扫描参照仓库 DuckDB `src/function/table/table_scan.cpp` 的投影列与共享扫描状态思路，复用现有 CSD 输入块调度。

## 调用流程

1. PG 规划器收集后续表达式、连接条件、分组键及排序键所需的基表列，删除物理 Scan targetlist 中无用的列。同步更新直接消费者的分组/排序位置；整行引用保留原输出。
2. 独立顺序 Scan 也可以使用多 CU。PG 原生 parallel-aware SeqScan 保留 Gather/Partial Aggregate 等结构，参与者通过 DSM 中的原子偏移领取不同块。只保留原生 PG worker，不另起辅助进程或代理。
3. 每个参与者使用独立 AFDM/算子状态和现有 SNIA batch。共享扫描按参与者数约束 AFDM 布局；同时存活的多个设备输入遇到真实 AFDM 分配失败时，释放未提交的分配，缩小输入块/活动槽后重试。共享扫描至少保留两个设备执行槽，以维持现有并行输入协议；最小布局仍放不下时明确报错。
4. CU 继续输出已有 `qs_cell`，协调端只紧密排列实际投影列。同一块 4096-cell 结果空间可容纳 `floor(4096 / 输出列数)` 行，不再每行预留 16 列；长字符串同时受 64KiB 字符串空间限制。设备控制记录直接带回有效行数和字符串长度。
5. PG 按有效长度读取结果，每次传输最多 256KiB，避免原来固定读取完整 16 列结果以及拆成多个 64KiB 请求。内部 CU 的列向量和行格式保持原有布局；改变的是通用树的最终返回缓冲，完整卸载也使用该返回方式。
6. PG 直接生成 Numeric 的 base-10000 表示，去掉整数→十进制字符串→`numeric_in` 的往返转换。PG 仍逐行接收 TupleTableSlot；`qs_result` 是本项目结构，不是 PG 原生元组。

EXPLAIN 增加实际返回字节、结果读取请求数、返回行数及输出列数。PG 并行时这些计数属于当前 backend，不代表所有 PG worker 的合计；`CSD Workers` 是该设备流的执行槽数，不应把各 PG backend 的数值相加当作物理 CU 数。物理 CU 数仍由设备查询给出。有序索引扫描保持原顺序，不强行改为无序并行。

## 针对性验证

Guest 执行 `sh /home/femu/tests/filter-only-test.sh`，默认仅验证正式 Q6/Q1/Q9；可以传入查询编号。每条每种模式先重启 PG、清除 Guest page cache，再执行 `tests/guest/queries/` 原查询并与 HOST 输出比较。未执行整套 22 条验收。

设备单元测试使用 `pgSinaPackedOutput64MiB`、`pgSinaDictionary64MiB`、`pgSinaFilterPaging64MiB`，覆盖紧凑返回、长字符串/NULL、并行排序输出及清理。PG 的 `pgSinaPlannerCost` 覆盖批次容量、文本 MIN/MAX、独立 Scan 并行和有序索引不并行。BUILD_TESTING 还生成不安装的 Numeric 测试模块，14 个用例覆盖正负数、零、截断、极小小数及 128 位边界：

```sql
LOAD 'pg_sina_csd';
CREATE FUNCTION pg_temp.csd_numeric() RETURNS boolean
AS '/home/femu/opt/pg_sina_csd/build/pg-sina-numeric-test', 'pg_sina_numeric_test' LANGUAGE C;
SELECT pg_temp.csd_numeric(); -- 应返回 true，任一不匹配直接报错
```

性能结果见下表。所有测试使用 4 CU、256MiB workmem、64MiB FDM；表中时间为 SQL 墙钟，不包含冷启动本身。

| 查询 | 修改前 filter-only（ms） | 修改后 filter-only（ms） | 本轮原生 HOST（ms） | 本轮完整 CSD、无 HPCP（ms） |
| --- | ---: | ---: | ---: | ---: |
| Q1 | 57365.573 | 7784.624 | 4476.813 | 809.904 |
| Q6 | 2958.198 | 579.055 | 1491.841 | 561.453 |
| Q9 | 57062.430 | 5695.783 | 2956.425 | 1203.205 |

修改前使用 `results/tpch.F1pZUv/` 的已记录冷启动结果，修改后来自 `results/filter-only.eJLjMm/`。每条结果均与本轮 HOST 一致。Q1/Q9 仍高于 HOST，**性能未达标**，不能归因为 workmem 不足：该轮设备 EXPLAIN 的 spill 读写为零，仍需优化大量行交付、PG Datum 生成及 HOST 消费链。`filter-only` 需要将过滤后的明细交给 HOST，因此不能直接用只返回聚合结果的完整卸载速度代表其性能；实际边界开销也不能只按 PCIe 理论带宽估算。

另在 `results/filter-only.o8BXAS/` 验证正式 Q2、Q13：HOST/filter-only/完整 CSD 分别为 758.964/1516.907/391.977ms、1123.478/1282.822/453.963ms，全部结果一致；这两条用于补查多输入 AFDM 竞争、排序及 LEFT JOIN/NULL，未作为新旧速度对照。未因单次测量差异宣称完整卸载得到加速。

共享返回格式与边界估计改动后，另以正式 Q9 冷启动验证 SQLerate：1244.163ms，输出与同轮 HOST 一致，日志保存在上述 `filter-only.eJLjMm` 目录。
