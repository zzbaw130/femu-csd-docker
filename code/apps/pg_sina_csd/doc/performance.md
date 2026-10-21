# Q1/Q6 瓶颈统计（2026-10-21）

基线代码 7b1a3cbac。PG12.22、SF=1、Guest 8 vCPU/8GiB，设备4 CU、64MiB AFDM/64MiB workmem、runtime_scale=1；原生PG最多2个并行worker加主进程。PCIe带宽模型关闭，ch_xfer_lat=0。实际算子运行在FEMU宿主CPU（Xeon E7-4820 v3，支持AVX2），不是物理SSD处理器。

每次查询均停止PG、sync/drop Guest caches、重新启动；不重启FEMU或重做映射。每组3轮，交替host/csd/profiled顺序。三组均使用EXPLAIN ANALYZE BUFFERS TIMING OFF及track_io_timing；profiled同时启用算子计时与SNIA传输计时，其他组关闭。采样时未并行编译或运行其他测试。

复现：`bash tests/pg-sina-csd-test.sh analyze`。原始JSON和日志保存到脚本打印的Guest结果目录；本次为 `/tmp/pg-sina-queries.w6xzzi`，主机副本 `/tmp/pg-snia-bottleneck`。诊断结束已恢复普通PG环境。

## 查询延迟

| 查询/模式 | 三轮耗时 ms | 均值 ms | 样本标准差 ms |
| --- | --- | --- | --- |
| q1/host | 4927.632 / 4648.394 / 4798.313 | 4791.446 | 139.746 |
| q1/csd | 1604.138 / 1447.110 / 1487.366 | 1512.871 | 81.562 |
| q1/profiled | 1787.458 / 1775.923 / 1477.861 | 1680.414 | 175.511 |
| q6/host | 1599.171 / 1659.523 / 2157.559 | 1805.418 | 306.453 |
| q6/csd | 1466.719 / 1453.357 / 1456.354 | 1458.810 | 7.011 |
| q6/profiled | 1423.871 / 1402.692 / 1416.728 | 1414.430 | 10.775 |

本轮Q1约3.17倍，Q6约1.24倍；Q6 host波动较大，中位数比约1.14倍。不能把本轮均值与此前持平的样本合并成稳定加速结论。profiled比关闭组快或慢都可能由波动造成，3轮不足以精确量化计时开销；正式性能对比使用关闭组。

## 设备算子CPU工作量

以下为profiled三轮均值；CPU跨块/CU累计，比例只占设备算子CPU总量，不是查询墙钟占比。

| 查询 | 算子 | 次数 | CPU累计 ms | CPU占比 | 墙钟累计 ms |
| --- | --- | --- | --- | --- | --- |
| q1 | Scan | 220 | 1505.889 | 36.96% | 1508.095 |
| q1 | Filter | 220 | 521.600 | 12.80% | 522.428 |
| q1 | Aggregate.sink | 220 | 2046.453 | 50.23% | 2048.421 |
| q1 | Aggregate.source | 1 | 0.054 | 0.00% | 0.056 |
| q1 | Sort | 1 | 0.010 | 0.00% | 0.011 |
| q6 | Scan | 220 | 1262.870 | 47.90% | 1264.443 |
| q6 | Filter | 220 | 1357.112 | 51.47% | 1359.052 |
| q6 | Aggregate.sink | 220 | 16.614 | 0.63% | 17.054 |
| q6 | Aggregate.source | 1 | 0.051 | 0.00% | 0.053 |
| q6 | Limit | 1 | 0.003 | 0.00% | 0.005 |

Q6约98%的行被过滤；Scan仍先解码所有行需要的四列。Q1约98%的行参与聚合，多个表达式都经过逐行解释器。两者均无spill，工作内存峰值679872字节；结果回传累计不足0.5ms，内存不足、spill及结果回传不是本轮主要问题。

## 墙钟关键阶段

| 查询 | 准备 ms | 第一pipeline ms | 第二pipeline ms | 清理 ms | 总计 ms | 有效算子忙碌度估计 |
| --- | --- | --- | --- | --- | --- | --- |
| q1 | 85.864 | 1583.871 | 0.665 | 7.061 | 1677.460 | 64.3% |
| q6 | 69.305 | 1335.900 | 0.625 | 5.930 | 1411.760 | 49.3% |

有效算子忙碌度≈累计算子CPU/(4×第一pipeline墙钟)，仅估计算子函数体在这段时间的占用；不包含CU线程其他工作，也不是硬件利用率。Q6平均约1.97个、Q1约2.57个CU在进行算子计算，仍有空档。不能直接将pipeline时间减去CPU累计时间称为I/O。

SNIA传输累计耗时（ms，跨并发请求求和）：

| 查询 | E1 663次 | D0 220次 | F2 9次 |
| --- | --- | --- | --- |
| q1 | 4667.822 | 928.110 | 0.417 |
| q6 | 3363.956 | 939.436 | 0.380 |

E1包括排队/提交/完成和函数体；D0包括内部读取与命令路径。以上不能相加当作查询时间，也不能由数据量除以D0累计时间推导设备带宽。第一pipeline每轮等待当前batch结束，再提交下一轮，尚无跨轮预取。等待与编排是实际限制，但现有累计统计还不能精确切分其墙钟占比。

## 独立perf采样

基准采集结束后另外冷启动采样；perf记录FEMU进程的cpu-clock:u、499Hz、DWARF栈，报告只筛选femu-csd-cu线程并按筛选后的样本归一化，未丢样。只附加CU线程的首次尝试遗漏动态SO映射，未用于结论。有效记录及报告位于主机 `/tmp/pg-snia-q{1,6}-process.perf.data`、`/tmp/pg-snia-q{1,6}-hotspots.txt`。perf运行不用于延迟比较。

| 查询 | 热点 | CU样本占比 |
| --- | --- | --- |
| Q6 | filter.so: qs_evaluate | 51.66% |
| Q6 | scan.so: pg_sina_scan_impl | 46.18% |
| Q1 | aggregate.so: qs_evaluate | 41.88% |
| Q1 | scan.so: pg_sina_scan_impl | 33.37% |
| Q1 | filter.so: qs_evaluate | 12.33% |
| Q1 | aggregate.sink 主体 | 8.30% |
| Q1 | get_group | 2.42% |

## 优化顺序

1. 参考DuckDB的SelectExpression/SelectionVector及类型化比较循环，将Filter从逐行递归解释改为按列、按谓词处理，支持SIMD；严格保留NULL及精确numeric语义。
2. 聚合表达式也按列批次求值，复用公共表达式；Q1应优先减少表达式解释，当前哈希查找占比很小。
3. Scan继续按需提取列，研究先过滤再解码聚合输入；目前已是列式输出，问题在提前解码被丢弃行及heap变长字段遍历。
4. 在4 CU基础上改进读取/计算重叠与跨批次空档，按依赖显式编排；不能仅增加CU数就假设吞吐提升。
5. 优化后重复关闭/开启计时冷启动与原生结果对照；PCIe/通道模型另行按目标设备校准，不随意限速制造收益。

## 第一轮列向量/SIMD过滤优化

实现参考本地DuckDB PhysicalFilter::SelectExpression、SelectionVector及类型化比较循环。复合AND中的列/常量比较按块归一化定点单位，AVX2一次处理4个int64，结合NULL位图生成行索引；无AVX2机器走类型化标量循环。OR/NOT、NULL常量、非无损阈值转换和其他表达式保留精确求值器。现有Scan已按所需列输出列式批次，4 CU并行分支保持不变。

### 交替A/B验证与取舍

只替换Filter SO，其他算子、PG、输入、内存/CU、计时开关均相同，每个样本独立冷启动；编译和perf不与测量并行。对照SO由9868e22bb的Filter以相同编译参数构建。文件使用原子替换，结束恢复最终构建。基准产物只留在/tmp，没有新增程序目录或配置项。

最初给单条件也使用列向量/SIMD，Filter CPU虽下降，Q1整条查询却回退。交替A/B为旧版1.526s、该候选1.659s；仅换成类型化标量循环也未消除回退，因此不能把原因简单归结为AVX2。分批传输计时中，Q1 E1累计时间超出函数体累计时间的部分从约0.59s增到约1.12s；这是不同采样组的线索，尚不足以断言具体排队或完成机制。需要进一步分开提交、排队及完成延迟，不能靠缩短函数CPU时间推导查询必然加速。

最终策略：仅为复合AND启用快路径；单谓词保持原执行路径，以避免已观察到的Q1回退。这是按表达式形态选择，不按SQL文本或Query编号选择。

| 对照 | 旧版三轮 ms | 最终/有效路径三轮 ms | 旧均值 ms | 新均值 ms |
| --- | --- | --- | --- | --- |
| q1 | 1405.591 / 1623.352 / 1441.300 | 1555.183 / 1390.132 / 1491.416 | 1490.081 | 1478.910 |
| q6 | 1337.713 / 1328.279 / 1317.440 | 964.034 / 986.016 / 981.021 | 1327.811 | 977.024 |

Q1基本持平；Q6延迟下降约26.4%（约1.36倍）。Q6表中是旧/新复合条件路径的交替A/B，最终加上单谓词回退条件后，另做了Q6冷启动检查，结果一致且仍走该快路径。开启计时的诊断数据不代替长期关闭计时的基准结论。

Q6交替A/B的Filter累计CPU从1363.984ms降至59.239ms，约减少95.7%；Scan从1260.343ms到1230.070ms，Aggregate.sink约23ms。优化后Scan占设备算子CPU约94%，成为下一项计算优化重点。Q1最终仍以聚合表达式解释和Scan为主要计算热点；准备和微小结果排序不是当前主要问题。

第一轮完整复测的原始目录：Guest `/tmp/pg-sina-queries.C2jofy`，主机 `/tmp/pg-snia-vector-after`；Q6交替A/B `/tmp/pg-snia-filter-ab`，Q1最终交替A/B `/tmp/pg-snia-filter-final-ab`（两处均有Guest和主机副本）。

验收：9项设备CTest及4项PG设备测试ASan/UBSan通过；差分测试对照原精确求值器，覆盖6种比较、反向常量、NULL、signed int64边界、定点小数、复合条件、复杂布尔回退及批次尾部。真实Guest冷启动SQL回归、Q1/Q6全部结果、超时回收与同backend设备复用通过；最终构建再冷启动核对Q1/Q6与原生结果完全一致。

下一步：Q6优先Scan解码/延迟物化；Q1优先聚合表达式列向量执行，但必须与E1等待/完成时序一起验收。共享的读取/提交空档仍需要流水线预取或减少往返；4 CU不能仅靠增加算子并行数就消除这些等待。

## 第二轮：过滤后才解码聚合输入

对照为5367ed316（已包含第一轮AVX2过滤），本轮只改Guest PG规划/参数和Scan/Filter。PG用现有行数估计选择延迟numeric解码：预计过滤后剩余不足一半行，且该列不在WHERE表达式中出现。Q6的extendedprice延迟解码；Q1不启用。Scan在原8字节值槽记录原始AFDM中的偏移和长度；Filter筛选后转换选中行，原始LOAD缓冲作为第三个MR传入。无新增设备命令、AFDM分配、SNIA/FEMU机制或配置项。

Q6扫描6001215行，仅119810行通过，因而extendedprice转换次数减少约98%。NULL值不转换。分组键同样支持延迟numeric解码。原heap仍全量内部读取921624576字节；节省的是数值转换CPU，不是NAND读量。

### 冷启动交替对照

每个版本每条查询8轮关闭计时，额外各3轮开启计时诊断。交换PG扩展与Scan/Filter SO，其他算子和设备配置保持一致；每次独立停止PG、清Guest缓存、启动PG，交替前后版本。最初3轮出现新版Q6的2099.576ms样本，因此补充5轮关闭计时和2轮开启计时；没有删除异常值。结束原子恢复新版并校验文件，未重启FEMU。

| 查询/版本 | 关闭计时的全部8轮 ms（按轮次） | 均值 ms | 中位数 ms | 样本标准差 ms |
| --- | --- | --- | --- | --- |
| Q1/前 | 1763.105 / 1469.667 / 1585.015 / 2032.961 / 1421.094 / 1399.367 / 1363.137 / 1409.719 | 1555.508 | 1445.381 | 232.995 |
| Q1/后 | 1616.520 / 1558.968 / 1444.378 / 1405.375 / 1760.854 / 1377.750 / 1382.790 / 1434.427 | 1497.633 | 1439.403 | 136.322 |
| Q6/前 | 1013.350 / 1107.239 / 1081.319 / 965.418 / 907.744 / 977.715 / 916.735 / 1009.004 | 997.316 | 993.360 | 71.165 |
| Q6/后 | 786.529 / 863.626 / 2099.576 / 915.129 / 845.865 / 743.744 / 779.270 / 882.959 | 989.587 | 854.745 | 452.203 |

Q6中位数降低约14.0%，但含异常值的均值仅降低0.8%，仍不能宣称端到端稳定提升14%。Q1中位数相差0.4%，基本持平。现有计时未覆盖该异常关闭计时样本，不能推断它具体由宿主调度、设备完成还是其他环节造成。

### 算子CPU与剩余开销

以下为另外3轮开启计时的累计线程CPU均值，不作为查询延迟基准：

| 查询/版本 | Scan CPU ms | Filter CPU ms | Scan+Filter CPU ms | Aggregate.sink CPU ms |
| --- | --- | --- | --- | --- |
| Q1/前 | 1266.378 | 496.132 | 1762.510 | 2037.755 |
| Q1/后 | 1325.907 | 497.515 | 1823.422 | 2028.127 |
| Q6/前 | 1176.250 | 53.563 | 1229.813 | 17.456 |
| Q6/后 | 1034.503 | 71.828 | 1106.332 | 18.374 |

Filter现在包含选中行的补解码，因此其CPU增加属于预期；Q6两算子合计减少约10.0%。Scan仍承担变长属性边界遍历、过滤列转换和列缓冲写入，不能因extendedprice解码减少98%就预期Scan同幅加速。Q1两算子合计CPU上升约3.5%，暂不能区分新增分支与测量波动，查询中位数未显示同等回退。

设备仍为4 CU、220块，workmem峰值679872字节。EXPLAIN中Q6 `CSD Deferred Columns=1`、Q1为0；开启计时AFDM分别45264896/64401408字节，与本轮对照相同。跨轮等待/命令编排仍存在，查询尾延迟需要进一步分开采样，不能由本轮CPU减少量直接推导整体加速。

验收：10项设备CTest、5项PG设备ASan/UBSan、Guest完整冷启动SQL回归与profile开关检查通过。新增真实heap差分测试确认被过滤行不解码、保留行精确值/NULL/显示精度一致；Guest覆盖延迟numeric聚合/分组键/空结果/复杂布尔，Q1/Q6全部输出与原生PG相同，取消后同backend重复查询及全量设备复用通过。

原始A/B目录：Guest和主机均为 `/tmp/pg-snia-late-ab`；关闭计时轮次为1/2/3/5/6/7/8/9，开启计时为4/10/11。Guest SQL回归目录 `/tmp/pg-sina-queries.UG6IRg`；本轮脚本日志为主机 `/tmp/pg-snia-late-{build,query,profile,ab,ab-more,sanitize}.log`。这些临时产物不纳入项目源码。

## 第三轮：共用向量表达式、共享聚合及基础优化

对照为d994a80e0（含SIMD复合过滤和延迟numeric解码）。本轮复杂Filter和Aggregate共用256行的表达式列循环，替代逐行递归；单谓词也使用类型化/SIMD过滤。同表达式SUM/AVG/COUNT共享累加状态，无分组COUNT(*)按批增加行数。Scan保留原属性遍历，增加常见numeric整数解码；小结果Sort排序行号后统一重排。Guest仅在块长度变化时更新input_bytes，220块的该字段写入从220次变为5次（代码路径计数）。

未改变原有pipeline：每轮4条 `LOAD → Scan → Filter → Aggregate.sink` 分支全部完成后才推进下一轮，各分支复用原输入/输出缓冲并保留局部聚合状态；全部220块、55轮之后再执行 `Aggregate.source → Sort（可选）→ Limit（可选）`。同轮分支本来就可并行，LOAD与其他分支的计算可能交错；本轮未新增同一分支的下一块预取或跨轮异步调度。

### 排除会回退的Scan改动

初始候选同时预计算Scan输出指针。首次完整A/B中Q1均值1520.093→1326.333ms，但Q6均值802.957→889.368ms，出现约11%回退，因此没有直接保留该实现。

固定新版PG/Filter/Aggregate/Sort，仅轮换三种Scan做3轮冷启动开启计时诊断，Scan累计CPU分别为：

| Scan变体 | 三轮CPU ms | 均值 ms |
| --- | --- | --- |
| 原版Scan/原版numeric | 1098.440 / 1094.332 / 1074.902 | 1089.225 |
| 原版Scan/仅numeric快路径 | 1027.106 / 1043.920 / 1048.971 | 1039.999 |
| 输出指针预计算+numeric快路径 | 1153.441 / 1127.451 / 1185.186 | 1155.359 |

据此撤回输出指针预计算，保留numeric快路径。此处只用算子CPU比较代码工作量；第一变体另有一次2252.039ms查询延迟，原始数据保留，没有用于宣称整体加速。根本原因尚未精确到指令/寄存器，不能把指针预计算视为通用有效优化。

### 最终版本冷启动对照

每版每查询6轮关闭计时、另外3轮开启计时。每次停止PG、清Guest缓存、重启PG；交替前后版本，仅切换PG扩展及相关SO，不重启FEMU。下表包含全部关闭计时样本，无剔除：

| 查询/版本 | 六轮延迟 ms | 均值 ms | 中位数 ms | 样本标准差 ms |
| --- | --- | --- | --- | --- |
| Q1/前 | 2270.660 / 1852.951 / 1534.046 / 1419.431 / 1675.843 / 1417.854 | 1695.131 | 1604.945 | 327.217 |
| Q1/后 | 1222.212 / 1285.648 / 1310.332 / 1375.949 / 1283.729 / 1269.920 | 1291.298 | 1284.689 | 50.690 |
| Q6/前 | 905.491 / 831.104 / 892.209 / 1201.448 / 905.055 / 798.396 | 922.284 | 898.632 | 143.581 |
| Q6/后 | 852.254 / 1233.639 / 874.287 / 874.307 / 821.149 / 817.736 | 912.229 | 863.271 | 159.376 |

Q1本组中位数降低约20.0%、均值降低23.8%；对照波动大，初始候选另一组均值降幅约13%，不能把24%当作稳定保证。Q6中位数降低约3.9%、均值降低1.1%，相对波动很小，当前判断整体延迟基本持平。

另外3轮开启计时的累计线程CPU均值：

| 查询/版本 | Scan ms | Filter ms | Aggregate.sink ms | 三者合计 ms |
| --- | --- | --- | --- | --- |
| Q1/前 | 1442.191 | 488.057 | 2023.723 | 3953.971 |
| Q1/后 | 1433.527 | 45.469 | 853.270 | 2332.266 |
| Q6/前 | 1170.461 | 76.579 | 18.392 | 1265.432 |
| Q6/后 | 1096.465 | 71.884 | 9.822 | 1178.171 |

Q1聚合CPU减少约58%，单条件Filter减少约91%，三者合计减少41%；Q6合计减少约6.9%。Q1第一pipeline墙钟均值1332.277→1232.354ms，Q6为875.945→796.570ms，这是开启计时的独立样本，不代替关闭计时的延迟对照。算子CPU大幅降低仍未等比例转为查询收益，批次等待/命令路径和未单独定位的尾延迟仍需后续分析。

两查询均4 CU、220块、读取921624576字节，匹配行数分别5884033/119810；AFDM分配与对照相同。新增表达式临时列全部使用workmem，设备报告的workmem峰值分别953152/1050432字节，仍远小于64MiB；未触发spill。精确int128算术保留，不把按列批量执行等同于每个运算都采用SIMD。

验收：11项设备CTest、6项PG设备ASan/UBSan通过。新差分测试对照原逐行求值器，覆盖算术、NULL、短路溢出、选择顺序、跨批次尾部，以及多CU/多块合并的共享聚合；补充numeric表示边界。Guest新增计算型过滤、布尔COUNT、常量共享聚合和重复输出检查；修复重复输出触发的CustomScan投影结果槽缺失。完整冷启动SQL、Q1/Q6精确结果、取消回收、同backend复用和profile开关通过。最终Scan构建再核对Q1/Q6全输出与原生PG相同。

原始数据：Guest/主机均有 `/tmp/pg-snia-vector2-final-ab`，1–6轮关闭计时、7–9轮开启计时；初始候选为 `/tmp/pg-snia-vector2-ab`，Scan隔离实验为 `/tmp/pg-snia-vector2-scan-ab`。Guest完整SQL回归目录 `/tmp/pg-sina-queries.U8yrdx`，profile回归 `/tmp/pg-sina-queries.H8lnGO`。结束已恢复最终SO和PG扩展并核对文件内容；测量与构建未并行执行。

## 第四轮：完成一个执行槽就补充下一块

对照为44fc3f53c，设备算子SO、FEMU/SNIA库、4 CU及64MiB AFDM/workmem均保持不变。仅替换Guest PG扩展：原每轮一个4分支HYBRID batch，改为4个独立SERIAL batch共享一个完成event。PG通过Context找到已完成的槽，重新配置该槽的LOAD范围并立即提交下一块；SERIAL只约束一个槽内的顺序。全部输入已分配且在途请求为零时才进入原第二条pipeline。无双缓冲、下一块预取或设备端SQL调度器。

这参考本地DuckDB `Pipeline::LaunchScanTasks` 与 `RowGroupCollection::NextParallelScan` 的并行任务领取输入方式；PG单线程维护输入游标，SNIA线程只执行设备调用。取消/错误先停止补充任务，尝试撤销未开始请求并收完所有在途完成事件，再读取/释放局部聚合状态及AFDM；不能强制撤销已开始请求。

### 冷启动交替对照

每版每查询6轮关闭计时，另外3轮开启计时。每次停止PG、清Guest缓存、启动PG；交替切换PG扩展SO，未重启FEMU，未与构建或其他测试并行。全部关闭计时样本如下，未删除异常值：

| 查询/版本 | 六轮延迟 ms | 均值 ms | 中位数 ms | 样本标准差 ms |
| --- | --- | --- | --- | --- |
| Q1/轮次屏障 | 2794.851 / 1278.204 / 1282.099 / 1259.576 / 1309.670 / 1383.495 | 1551.316 | 1295.885 | 610.756 |
| Q1/独立推进 | 834.348 / 925.559 / 885.486 / 833.241 / 838.133 / 839.134 | 859.317 | 838.634 | 38.032 |
| Q6/轮次屏障 | 826.373 / 871.743 / 828.281 / 875.872 / 844.070 / 967.975 | 869.052 | 857.907 | 52.816 |
| Q6/独立推进 | 593.967 / 603.378 / 610.595 / 596.971 / 650.470 / 615.783 | 611.861 | 606.986 | 20.599 |

本组Q1中位数降低35.3%，Q6降低29.2%。Q1均值降幅44.6%受到旧版首轮2794.851ms样本影响，不作为主要收益数字；Q6均值降低29.6%。这是同一FEMU配置的少量实验样本，不代表真实SSD的固定收益。

另外3轮开启计时的均值：

| 查询/版本 | 第一pipeline墙钟 ms | Scan累计CPU ms | Filter累计CPU ms | Aggregate.sink累计CPU ms |
| --- | --- | --- | --- | --- |
| Q1/轮次屏障 | 1167.655 | 1378.227 | 46.955 | 865.108 |
| Q1/独立推进 | 770.663 | 1214.341 | 45.024 | 841.886 |
| Q6/轮次屏障 | 764.719 | 1033.821 | 69.074 | 9.524 |
| Q6/独立推进 | 594.880 | 1063.273 | 79.271 | 10.488 |

按累计算子CPU/(4×第一pipeline墙钟)估计的函数忙碌度，Q1约49%→68%，Q6约36%→48%；这不是硬件利用率，不包含CU线程函数以外的工作，不能把剩余部分全部解释成NAND读时间。Q6算子CPU略升而查询墙钟下降，说明这次收益不依赖减少算子计算工作。SO未改，Q1的CPU差异也不能归因于新增算法；缓存/频率等宿主因素没有单独测量。

两查询均扫描6001215行、内部读取921624576字节、220块、4 CU，匹配分别5884033/119810行。开启计时AFDM仍为64401408/45264896字节，设备报告workmem峰值仍为953152/1050432字节。此次只增加Guest侧batch/event元数据，不增加设备输入/列缓冲，也没有spill。

验收：Guest扩展构建及heap布局CTest通过；完整冷启动SQL回归、Q1/Q6全结果、计时开关、取消后同backend复用通过。新增450000行多块表核对COUNT、分组、选择性过滤、无匹配及尾块；另以超出设备表示范围的numeric触发一个槽报错，确认其余在途请求排空后，同backend可连续两次完整扫描并得到450000行。测试表在回归结束清理，不修改TPC-H表。设备算子未改，本阶段未重复其已有native/sanitizer检查。

原始A/B数据在Guest及主机 `/tmp/pg-snia-dynamic-ab`，1–6轮关闭计时、7–9轮开启计时；Guest SQL回归 `/tmp/pg-sina-queries.1Yqf9S`。主机日志 `/tmp/pg-snia-dynamic-{build,query,profile,ab}.log`。结束已恢复新PG扩展并核对文件；EXPLAIN显示 `CSD Scheduling: independent serial batches`。

## 第五轮：按需解码、引用向量与算术 SIMD

对照为 b89c5c7b2。参考本地 DuckDB 的列引用、常量向量、条件选择和类型化二元运算，保留现有 AFDM 格式、4 个独立执行槽和全部设备命令。PG 将选择性过滤的延迟解码扩展到 WHERE numeric；设备端列和常量不再复制成 int128 临时列，AND/OR 只计算仍需判断的行。AVX2 扩展到安全范围内的加减、乘法及通用比较，不满足条件时回退精确 int128。完整调用方式见 README。

初始候选直接让简单 AND 也走通用布尔向量；Q6 一次诊断的 Filter 累计 CPU 约944ms，抵消了 Scan 的节省，因此没有保留。最终简单 AND 仍先执行已解码列的 SIMD 前缀，再单遍遍历存活行、按顺序判断剩余列；同一列相邻上下界共用一次解码。复杂 AND/OR 保留批次选择与每行公共表达式缓存。普通算术不构造无用的条件掩码。

### 冷启动交替对照

每版每查询6轮关闭计时、另3轮开启计时。每次停止PG、清除Guest页缓存、启动PG，交替替换扩展和相关算子SO，不重启FEMU。测量不与编译或其他测试并行。关闭计时的全部样本如下，没有剔除慢样本：

| 查询/版本 | 六轮延迟 ms | 均值 ms | 中位数 ms | 样本标准差 ms |
| --- | --- | --- | --- | --- |
| Q1/前 | 1286.154 / 871.210 / 853.779 / 845.986 / 1124.148 / 859.795 | 973.512 | 865.503 | 186.779 |
| Q1/后 | 855.462 / 806.638 / 799.826 / 813.123 / 817.855 / 843.571 | 822.746 | 815.489 | 21.933 |
| Q6/前 | 743.482 / 646.690 / 632.089 / 671.341 / 648.232 / 645.887 | 664.620 | 647.461 | 40.654 |
| Q6/后 | 783.267 / 594.956 / 615.038 / 635.702 / 610.280 / 617.824 | 642.845 | 616.431 | 70.032 |

Q1 中位数降低约5.8%，Q6约4.8%。Q1均值降低15.5%，受到旧版1286.154/1124.148ms样本影响，不作为主要收益数字；Q6均值降低3.3%，新版也有783.267ms慢样本。收益较小且有运行波动，不能解释为SIMD或延迟解码必然带来固定比例的查询加速。

另外3轮开启计时的累计CU线程CPU均值：

| 查询/版本 | Scan ms | Filter ms | Aggregate.sink ms | 三者合计 ms |
| --- | --- | --- | --- | --- |
| Q1/前 | 1286.854 | 44.001 | 850.208 | 2181.063 |
| Q1/后 | 1305.903 | 43.841 | 736.353 | 2086.097 |
| Q6/前 | 1089.164 | 87.400 | 9.315 | 1185.879 |
| Q6/后 | 936.295 | 260.239 | 10.302 | 1206.835 |

Q1聚合CPU减少约13.4%，三者合计减少4.4%；其Scan代码及提前解码方式未变，Scan计时差异不能归因于本轮扫描优化。Q6 Scan减少约14.0%，但Filter增加，合计反而增加约1.8%：推迟转换也带来了选择判断和原始heap再访问，当前没有测得Q6计算总量显著减少。三项CPU求和不是查询墙钟时间；小幅查询收益及profile阶段时序差异，不足以单独归因于某一条指令或缓存机制。

两查询仍读取921624576字节、扫描6001215行、220块、4 CU，匹配分别5884033/119810行。Q6延迟列由1增至3，Q1仍为0。开启计时的AFDM仍为64401408/45264896字节；workmem峰值分别由953152/1050432变为958528/1057728字节，仍远小于64MiB，没有spill。没有增加内部I/O预取或同槽双缓冲。

原始最终A/B数据：Guest SQL输入及容器/主机结果目录 `/tmp/pg-snia-vector4-ab`，1–6轮关闭计时、7–9轮开启计时。初始候选诊断为主机 `/tmp/pg-snia-vector-initial-profiles.json`，上一候选的完整数据为 `/tmp/pg-snia-vector3-ab`，只用于开发诊断，不与最终样本混合计算。结束恢复最终扩展与算子，并逐文件核对内容。

### 最终版本与原生 PG

交替A/B结束并恢复新版后，再单独交替运行host/CSD各3轮，逐次冷启动、关闭计时开关，所有输出精确一致。此组与前述新旧版本A/B分开统计：

| 查询/路径 | 三轮延迟 ms | 均值 ms | 中位数 ms |
| --- | --- | --- | --- |
| Q1/host | 4422.595 / 4311.650 / 4454.622 | 4396.289 | 4422.595 |
| Q1/csd | 844.854 / 810.381 / 870.057 | 841.764 | 844.854 |
| Q6/host | 1376.389 / 1441.415 / 1512.623 | 1443.476 | 1441.415 |
| Q6/csd | 614.926 / 624.354 / 650.457 | 629.912 | 624.354 |

这里CSD使用4 CU，原生PG保持其既有并行配置，并非等核数对照；FEMU参数未调整，结果不代表真实SSD硬件性能。

验收：11项设备CTest及最终改动相关的6项重复检查通过；6项PG设备ASan/UBSan通过（当前执行环境无法运行LeakSanitizer，设置detect_leaks=0；workmem释放仍由测试断言检查）。最终Guest完整冷启动SQL回归、Q1/Q6全部结果、prepared路径切换、取消后同backend重复执行、设备报错排空后复用，以及profile开关和计数检查全部通过。

最终Guest回归目录和主机日志分别记录在 `/tmp/pg-snia-vector-fused-{query,profile,host-csd}.log`；host/CSD原始计时另存主机 `/tmp/pg-snia-vector-fused-host-csd.tsv`。


## 阶段2a：外部行排序、TopN与可配置工作内存（2026-10-21）

本阶段增加单表行排序，Guest PG编排`LOAD→Scan→Filter（可选）→Sort.sink`四槽独立batch；输入耗尽后由设备`Sort.source`归并并执行OFFSET/LIMIT。小LIMIT使用局部TopN堆。输出按256行连续返回PG，256是结果块容量，不再限制排序总行数。全部run、表达式工作区、段头和归并堆通过workmem分配，AFDM仅用于输入、列批次、结果与spill传输。SNIA按NEED_STORE/LOAD/FREE完成设备内部数据移动与重入，无SQL runtime、辅助进程或Guest文件排序。

`pg_sina_csd.max_memory`默认64MB，为单条设备查询全部槽的工作内存上限；每槽先保留Filter临时空间，sink使用剩余份额，并受物理池容量限制。下一份run将超预算时spill，实际分配失败也会尝试腾出空间；没有统一的“空闲不足50%”阈值。参考本地DuckDB `src/common/sort/sort.cpp`的预算/线程分摊及外部排序判定，当前未引入其动态预算管理器。完整run对齐到64KiB传输块所能容纳的行数，避免仅几行的额外尾块。

### 容量升降验证

本地独立进程以1,048,583行、每行3个32字节单元（约96MiB），4槽sink、单source完整归并，与原生qsort逐行比较NULL顺序、值和小数显示精度。测试NVM仅模拟STORE/LOAD/FREE，不是设备带宽测试。每次结束断言全部token、上下文和分配已回收。

| 物理池 / 查询预算 MiB | 全部用例期间池峰值 bytes | 96MiB数据STORE次数 | LOAD次数 |
| --- | ---: | ---: | ---: |
| 4 / 4 | 4049296 | 1500 | 1867 |
| 8 / 8 | 8190704 | 1464 | 1621 |
| 16 / 16 | 16614992 | 1392 | 1419 |
| 64 / 64 | 65522528 | 624 | 624 |
| 128 / 128 | 103880016 | 0 | 0 |
| 64 / 4 | 4049296 | 1500 | 1867 |

每次传输64KiB；较小预算下，已有NVM副本的读缓冲可丢弃再加载，所以LOAD次数可能大于STORE次数。峰值包含该进程先前的小规模、TopN及提前关闭测试，不是只统计96MiB这一用例。64MiB池/4MiB预算与4MiB池行为一致，说明查询预算实际约束分配；128MiB池能保留全部数据而不spill。

17项设备CTest通过；最终改动相关12项重复检查以及6个外部排序容量ASan/UBSan检查通过。ASan设置detect_leaks=0，环境不支持LeakSanitizer，资源回收由workmem和token断言另行核对。Guest完整冷启动SQL回归通过，覆盖连续输出、实际spill、TopN/OFFSET、空输入、NULL、原生回退、prepared路径切换、报错/取消及游标提前关闭后的设备复用；不支持的外层查询整体回退PG，避免嵌套设备扫描争用同一设备锁。

完整TPC-H查询仍只验收Q1/Q6；下述排序语句是SF=1 heap上的算子验证，不是新增已完成的TPC-H查询编号。高基数聚合与Join继续属于下一阶段。

Guest最终容量实验 `/tmp/pg-sina-queries.J4XFak`：Q1、Q6及`tests/order_spill.sql`在4/16/64/128MB查询上限下分别冷启动，全部输出与host精确相等；PREPARE后的4→64MB切换也通过。物理池维持64MiB、不重启FEMU。下表为独立冷启动EXPLAIN，不与关闭profile的延迟样本混合：

| 查询上限 MiB | run数 / spill run数 | STORE bytes | LOAD bytes |
| --- | --- | ---: | ---: |
| 4 | 895 / 875 | 114688000 | 1399455744 |
| 16 | 225 / 197 | 103284736 | 1112539136 |
| 64 | 114 / 54 | 56623104 | 3538944 |
| 128 | 114 / 54 | 56623104 | 3538944 |

64/128MB行为相同，符合64MiB物理池上限。小预算下结果和内存限制正确，但当前直接多路归并不能为每个run保留64KiB读缓冲，丢弃和重读导致明显的读取放大；仅有LIMIT/OFFSET也不能消除为找出下一个最小行所需的补读。后续应增加受预算限制的多轮归并或更细的读缓存组织，不能把当前4/16MB实现描述为已完成I/O优化。所有run元数据仍需驻留，极大量run还有内存边界。

最终完整SQL回归日志 `/tmp/pg-snia-memory-final-query.log`，Guest `/tmp/pg-sina-queries.1hAsuE`；最终容量日志 `/tmp/pg-snia-memory-aligned-matrix.log`，profile快照 `/tmp/pg-snia-memory-final-matrix.json`。设备测试 `/tmp/pg-snia-memory-aligned-tests.log`，容量sanitizer `/tmp/pg-snia-memory-aligned-asan.log`。

### 最终版本SF=1排序延迟

Guest `/tmp/pg-sina-queries.KjmMQM`，主机日志 `/tmp/pg-snia-memory-order-bench.log`。全部为关闭profile、独立冷启动的3轮交替host/CSD，每次核对全部返回结果，未删除样本。原生PG保留既有`work_mem=4MB`及并行配置；CSD为4 CU、64MiB查询预算与64MiB物理池。这是现有两条执行路径的对照，不能解释为等核数、等总内存的排序算法比较。

| 测试 / 路径 | 三轮 ms | 均值 ms | 中位数 ms |
| --- | --- | ---: | ---: |
| order_topn / host | 2342.514 / 1627.240 / 1887.978 | 1952.577 | 1887.978 |
| order_topn / csd | 580.339 / 515.125 / 534.893 | 543.452 | 534.893 |
| order_spill / host | 2240.059 / 1987.981 / 2353.129 | 2193.723 | 2240.059 |
| order_spill / csd | 1353.913 / 1321.059 / 1328.090 | 1334.354 | 1328.090 |

TopN扫描lineitem后按extendedprice降序、orderkey升序取100行。外部排序筛选1995年发货记录，按四列排序后OFFSET 10000、LIMIT 12000，约117MiB紧凑行数据超过64MiB预算，验证实际spill。独立开启profile的样本保存在同目录两个JSON中，不混入延迟均值。FEMU带宽模型参数未调整，少量样本不代表真实SSD的固定收益。


## 阶段2b：高基数聚合与部分状态归并（2026-10-21）

所有支持的GROUP BY改为自适应局部哈希路径，不再依赖PG估计分组数≤256。局部表未flush、所有局部组的数量之和≤256时直接使用小哈希合并快路径；其他情况将部分状态按键归并，再计算最终SUM/AVG/COUNT。需要SQL排序时将最终组送入结果排序。输入和结果排序共享查询预算：固定局部状态扣除后，有ORDER BY的查询将余量分给两种排序，没有ORDER BY则用于部分状态run。这是简单的静态预算安排，没有实现动态借用。

参考本地DuckDB的radix_partitioned_hashtable.cpp（局部预聚合、内存预算和状态合并）及sorted_aggregate_function.cpp/sort.cpp（排序与状态组织）；没有复制DuckDB的完整radix分区实现。aggregate SO直接复用sort.c私有函数，不发送嵌套NVMe、不新增固件功能；profile的Aggregate.sink/source包含其内部排序CPU与I/O重入，不与独立Sort计数重复累计。

### 正确性与内存

21项设备CTest及16项PG算子ASan/UBSan通过；为最终新增全NULL聚合组又重复4项容量sanitizer，全部通过。LeakSanitizer仍因执行环境限制关闭，测试另行断言分配器零占用及token回收。

新的设备差分测试包含100003行、32772组的完整输出，与独立sum/count预期值逐列核对；同一AVG输入在不同槽和run中的条数不同，合并sum/count后再求平均。另有8组小结果、跨块边界、NULL键/全NULL值组、显示精度、排序、LIMIT/OFFSET、空输入、提前终止和失败scope已回收token的清理。下表取完整大结果case；峰值包括同进程先前的小结果等用例：

| 物理池 / 查询预算 MiB | 峰值 bytes | STORE次数 | LOAD次数 |
| --- | ---: | ---: | ---: |
| 4 / 4 | 3984224 | 305 | 10529 |
| 8 / 8 | 7910320 | 248 | 5704 |
| 64 / 64 | 23264416 | 0 | 0 |
| 64 / 4 | 3984224 | 305 | 10529 |

小结果case不生成run、不发生STORE/LOAD。缓存策略起初保留64KiB整块；仅在交错run反复提前丢弃缓存后缩小窗口，复制当前窗口到workmem，实际LOAD仍为64KiB。顺序run保持原读法，测试额外约束约96MiB顺序数据在4MiB预算下的LOAD不超过STORE的3倍，避免无条件缩小窗口产生回退。交错场景仍有读取放大，不能替代后续多轮归并。

Guest完整冷启动回归通过：8192组、NULL分组键、多列分组、隐藏排序键、4MB预算、真实spill、全部分批结果，以及设备报错和游标提前关闭后的同backend复用。Q1/Q6和SF=1新增语句全部结果与原生PG一致；profile开关/调用数也通过。日志：`/tmp/pg-snia-high-aggregate-final-query.log`（Guest `/tmp/pg-sina-queries.d9KCUk`）、`/tmp/pg-snia-high-aggregate-final-profile.log`（Guest `/tmp/pg-sina-queries.yQjO4c`）、`/tmp/pg-snia-high-aggregate-final-native.log`、`/tmp/pg-snia-high-aggregate-final-asan.log`和`/tmp/pg-snia-high-aggregate-extra-asan.log`。

### 冷启动性能

所有构建和其他测试结束后单独测量，每次停止PG、清Guest页缓存并重启PG，交替host/CSD各3轮，关闭profile。PG保留既有work_mem=4MB及并行配置；CSD为4 CU、64MiB AFDM及64MiB查询/物理workmem上限，不是等核数、等总内存对照。全部样本保留：

| 查询/路径 | 三轮 ms | 均值 ms | 中位数 ms |
| --- | --- | ---: | ---: |
| high_aggregate/host | 3958.478 / 3075.970 / 3066.997 | 3367.148 | 3075.970 |
| high_aggregate/csd | 2862.612 / 2776.241 / 3210.016 | 2949.623 | 2862.612 |
| Q1/host | 4380.653 / 4809.148 / 4544.757 | 4578.186 | 4544.757 |
| Q1/csd | 851.351 / 819.152 / 851.366 | 840.623 | 851.351 |
| Q6/host | 1496.184 / 1682.299 / 1482.344 | 1553.609 | 1496.184 |
| Q6/csd | 591.267 / 604.685 / 621.489 | 605.814 | 604.685 |

新增语句是1995年lineitem按partkey汇总revenue、AVG(quantity)、COUNT，再按revenue取前100，是算子验证，不是新增TPC-H查询编号。均值差异受到host首轮3958ms样本影响；中位数约3076→2863ms，收益较小且存在波动。当前完整TPC-H覆盖仍为Q1/Q6，Join、HAVING、MIN/MAX及一般字符串待后续阶段。

原始记录：Guest `/tmp/pg-sina-queries.dEPbad`、`/tmp/pg-sina-queries.ACDp5s`，主机 `/tmp/pg-snia-high-aggregate-{bench,q1q6-bench}.log`。独立profile快照 `/tmp/pg-snia-high-aggregate-profiles.json`，不混入关闭profile的延迟均值。

独立profile的新增语句：扫描6001215行，保留914963行，内部读取921624576字节；118个run、89个spill run，写93323264字节（89MiB）、读204865536字节（195.375MiB），workmem池峰值33843360字节。第一pipeline墙钟1702ms，source阶段1403ms；Scan/Filter/聚合sink/source累计CU CPU分别约899/194/1620/740ms，累计CPU不能直接相加为查询墙钟。sink有1644次、source有4551次调用，包含I/O重入。

当前主要限制是局部表固定256组、部分状态排序与单source归并，以及大量重入/LOAD命令。预留的结果排序预算在TopN场景几乎没有使用，使输入排序仅利用约一半容量；后续可考虑在明确生命周期内借用预算，而不是降低全局空闲百分比阈值。极多run元数据仍可能耗尽内存，多轮归并尚未实现。Q1独立profile保持0个run/0次spill，聚合source一次，说明其继续使用小哈希快路径；新状态结构使workmem峰值约1.50MiB。

## 阶段3a：Hash Join、CASE与Q14（2026-10-21）

两表整数INNER Join、搜索CASE、ASCII前缀LIKE和聚合后numeric表达式接入，完整TPC-H覆盖扩展到Q1/Q6/Q14。该阶段没有修改FEMU、SNIA、设备时序参数或原生PG配置。设备仍为4 CU、64MiB AFDM、64MiB共享workmem；查询预算默认64MB，build占一半，其余由probe/聚合槽分摊。build不能驻留时分批spill并重扫probe，没有实现radix分区。

24项设备CTest、19项PG算子ASan/UBSan及Guest完整query/join/profile回归通过。LeakSanitizer因现有执行环境限制关闭，独立设备测试断言workmem零占用及峰值约束。Join回归覆盖重复键、负/NULL键、两侧过滤、无匹配/空输入、4MB预算、原生回退、build/probe错误和取消后的同backend复用；Q14在4/64MB预算下与原生PG结果逐字一致。原有外部排序、高基数聚合、游标关闭和prepared plan切换也通过。

### 冷启动延迟

所有编译、sanitizer和回归结束后单独测量，host/CSD交替各3轮，每次停止PG并清除Guest页缓存后重新启动，profile关闭。均包含程序加载和回收，逐次比较完整输出；PG保留work_mem=4MB和既有并行配置，属于原生配置对照，不是等总内存、等核数测试。

| 查询/路径 | 三轮 ms | 均值 ms | 中位数 ms |
| --- | --- | ---: | ---: |
| Q14/host | 2876.199 / 1488.677 / 1834.704 | 2066.527 | 1834.704 |
| Q14/csd | 609.988 / 649.939 / 648.166 | 636.031 | 648.166 |
| Q1/host | 4736.216 / 4382.337 / 4512.684 | 4543.746 | 4512.684 |
| Q1/csd | 864.999 / 832.048 / 887.784 | 861.610 | 864.999 |
| Q6/host | 1484.863 / 1408.204 / 1404.922 | 1432.663 | 1408.204 |
| Q6/csd | 573.234 / 596.133 / 584.259 | 584.542 | 584.259 |

Q14均值约3.25倍，但host首轮波动明显；中位数约2.83倍。Q1/Q6保持既有性能范围，不能将不同阶段的小幅波动认定为此处Join改动的收益。未开启的PCIe带宽模型等实验限制仍适用，这些是FEMU环境结果，不代表真实CSD芯片性能。

原始日志：主机`/tmp/pg-snia-join-{guest-regression,old-regression,profile-regression,asan,all-native,bench,q1q6-bench}.log`；Guest正确性目录`/tmp/pg-sina-queries.en88Fb`、`/tmp/pg-sina-queries.WCO84y`、`/tmp/pg-sina-queries.09tGbt`，性能目录`/tmp/pg-sina-queries.PVwKR3`、`/tmp/pg-sina-queries.CqLFEE`。复现入口为`tests/pg-sina-csd-test.sh join|join-bench|bench`。

### 独立profile与spill

Q14的单次冷启动profile保存在主机`/tmp/pg-snia-q14-profile.json`。只有一个CSD CustomScan，build=200000行、1批，probe=6001215行、过滤后75983行，产生75983个连接组合。内部读取955187200字节（build加一轮probe），AFDM45346816字节，workmem池峰值13689712字节，无spill。

执行端墙钟：Setup 68.109ms，Build 58.688ms，probe阶段480.358ms，最终source 0.688ms，Cleanup 6.802ms，总计614.646ms。Scan累计CU CPU 918.827ms，Filter 44.605ms，HashJoin.build/load 14.413/5.671ms，HashJoin.probe+Aggregate 45.475ms。Scan覆盖两表229块，probe相关调用220次。当前主要计算开销仍是heap扫描解码，连接与聚合本身较小；累计CPU不是查询关键路径，不能直接相加解释延迟。

4MB预算的重复键测试使用70003行build、150003行probe，产生1289854个组合。build分3批，probe实际累计扫描450009行，STORE/LOAD各2949120字节；共享池该次峰值2343088字节。数据见`/tmp/pg-snia-join-small-profile.json`。独立设备4MiB池和64MiB池内4MiB预算测试另行严格断言实际峰值不超预算；Guest的池峰值可能包含历史查询，不作为通用的逐查询上限判据。重扫保证当前有界实现的完整语义，后续多表/大build应考虑分区以减少重复内部读取。

## 阶段3b.1：连接后分组与Q12（2026-10-21）

接通Join后的外部分组/结果排序，聚合spill时保存probe位置并重入；支持SQL字符串常量的等于/不等于、IN和受限字符串分组/排序。Q12全部数据算子在一个CSD CustomScan中执行；字符串仅在最终Datum转换时恢复。当前完整TPC-H覆盖为Q1/Q6/Q12/Q14，其他查询仍按计划扩展。

24项设备CTest、19项PG算子ASan/UBSan、完整Guest query/profile以及最终join回归通过。后者包含1289854个重复键组合的高基数组、4MB双重spill、多页结果、游标提前关闭/错误/取消后的设备复用，以及text/varchar/bpchar尾空格、空串、NULL数组项、排序及原生回退。SF=1 Q12分别以16/64MB预算核对，Q14以4/64MB预算核对；所有结果逐字一致。Q12为`MAIL|6202|9324`、`SHIP|6200|9262`，实际bpchar输出保留10字符宽度。

主机日志：`/tmp/pg-snia-q12-workmem-native.log`、`/tmp/pg-snia-q12-workmem-asan.log`、`/tmp/pg-snia-q12-workmem-join-regression.log`、`/tmp/pg-snia-q12-old-regression.log`、`/tmp/pg-snia-q12-profile-regression.log`。最后调整游标间距后，另外3项Join容量CTest及3项对应ASan/UBSan通过，日志为`/tmp/pg-snia-q12-cursors-{native,asan}.log`。Guest最终Join回归目录`/tmp/pg-sina-queries.JLsX9O`，单表及计时回归目录`/tmp/pg-sina-queries.4vCtr4`、`/tmp/pg-sina-queries.8YW3Ab`。

### 最终版本冷启动延迟

所有构建和回归结束后单独测量，保持原生PG并行/work_mem配置；CSD为4 CU、64MiB AFDM、64MB查询/物理workmem上限。每次停止PG、清Guest页缓存并启动，交替host/CSD各3轮，profile关闭，逐次核对完整结果。全部样本如下：

| 查询/路径 | 三轮 ms | 均值 ms | 中位数 ms |
| --- | --- | ---: | ---: |
| Q12/host | 2125.019 / 1971.768 / 1970.799 | 2022.529 | 1971.768 |
| Q12/csd | 3399.552 / 3426.804 / 3466.878 | 3431.078 | 3426.804 |
| Q14/host | 2258.033 / 1452.882 / 1599.472 | 1770.129 | 1599.472 |
| Q14/csd | 691.175 / 678.021 / 692.397 | 687.198 | 691.175 |
| Q1/host | 4428.497 / 4448.977 / 4350.515 | 4409.330 | 4428.497 |
| Q1/csd | 897.827 / 913.736 / 1035.822 | 949.128 | 913.736 |
| Q6/host | 1464.201 / 1573.149 / 1432.170 | 1489.840 | 1464.201 |
| Q6/csd | 667.279 / 656.134 / 951.190 | 758.201 | 667.279 |

Q12当前CSD约为host的1.70倍延迟，未获得加速；Q1/Q6/Q14仍有收益。Q1/Q6第三轮CSD样本明显较高，保留全部样本，Q6中位数为667.279ms；本阶段不是它们的性能优化，其CSD延迟也高于上一阶段部分样本，需后续独立归因，不能用本组测量声称加速进一步上升。FEMU的PCIe带宽模型未启用等限制仍适用，这不是等核数、等总内存或真实CSD芯片对照。

原始性能日志为`/tmp/pg-snia-q12-release-join-bench.log`、`/tmp/pg-snia-q12-release-q1q6-bench.log`，Guest目录`/tmp/pg-sina-queries.gXDnD9`、`/tmp/pg-sina-queries.e3pqF5`；`join-bench`包含Q12/Q14及另行冷启动的profile。

### Q12瓶颈：减少重复读取后，build扫描仍串行

最初仅按原始表行数选择orders做build，三轮CSD为3666.025/3599.402/4193.898ms，均值3819.775ms；host均值2113.861ms。独立profile确认1500000行build分3批，lineitem扫描18003645行，内部读取2978643968字节，spill写/读各60162048字节；Build Wall 921.897ms，probe阶段2816.831ms。Scan/Filter累计CU CPU约3968/3185ms。数据在`/tmp/pg-snia-q12-before-profile.json`及Guest`/tmp/pg-sina-queries.gWoTRq`。

最终补充了内存约束下的方向比较：原build估计超预算时，尝试另一侧，以过滤后行数和实际投影宽度估计记录/哈希桶空间。Q12因而选择过滤后的lineitem建表，而Q14默认预算下仍保留可驻留的part build。估计失准只影响方向和spill量，实际分配依然限额。

最终Q12独立profile（`/tmp/pg-snia-q12-release-profile.json`）：build扫描6001215行，过滤后建表，1个驻内存批次；probe为1500000行orders，产生30988个匹配组合，内部读取1135394816字节，无spill。AFDM51761152字节，workmem池峰值4312880字节。Build Wall=3204.146ms，probe阶段=139.852ms，source=0.596ms，Setup/Cleanup=80.864/7.795ms。Scan累计CU CPU=1779.013ms，Filter=1267.781ms，HashJoin.build=8.099ms，probe+Aggregate=99.742ms。

内部读取减少约62%，均值延迟约下降10%，但绝大部分时间转为串行build扫描/过滤。哈希插入与最终聚合并非主要瓶颈。下一步应让build侧LOAD/Scan/Filter也利用多个执行槽，再集中/合并哈希状态；更大build再考虑分区，不能仅靠交换连接方向保证加速。没有在本阶段引入新的设备调度机制或读算双缓冲。

### 4MB下的归并读取放大

独立设备压力测试扩大到1471273个连接组合、7001个潜在分组。4MiB池及64MiB池内4MiB查询预算均通过，峰值3562368字节，build分3批，STORE 1143次、LOAD 188408次；64MiB预算无spill。实际分配及结束时零残留均断言。恢复游标在build workmem中，每槽独占并隔开缓存行；AFDM只传槽编号、句柄和统计。

Guest的1289854组合测试，在4MB预算下用2槽，build分3批，source汇总487个run、478个spill run；写65601536字节、读12875333632字节，池峰值3551664字节，profile耗时26245.991ms。数据为`/tmp/pg-snia-q12-small-groups-profile.json`。其中build自身写/读约2.8MiB，其余主要是部分状态归并；源数据与状态并非真的有12GiB，而是小读缓存交错访问导致反复LOAD。结果和内存界限通过，不代表这种低内存场景具备良好性能。多轮归并或分区仍是需要解决的性能限制，不能用提高默认内存掩盖。

## 阶段3b.2：并行build扫描（2026-10-22）

保持同一Guest、4 CU、64MiB AFDM和64MB查询/物理workmem上限。先完成Guest回归，再单独执行`join-bench`；host/CSD交替各3轮，每次停止PG、清Guest页缓存并启动，profile关闭，每轮完整结果逐字一致。原生PG的work_mem与并行设置不变。没有同时运行编译、其他回归或sanitizer。

| 查询/路径 | 三轮 ms | 均值 ms | 中位数 ms |
| --- | --- | ---: | ---: |
| Q12/host | 1868.417 / 2033.948 / 1871.927 | 1924.764 | 1871.927 |
| Q12/csd | 1077.520 / 1438.838 / 1081.889 | 1199.416 | 1081.889 |
| Q14/host | 2037.881 / 1475.466 / 1425.821 | 1646.389 | 1475.466 |
| Q14/csd | 609.213 / 665.657 / 648.609 | 641.160 | 648.609 |

本轮Q12按均值约1.60倍加速，Q14约2.57倍。Q12第二轮CSD偏高，保留全部样本；中位数比约1.73倍。上一阶段Q12 CSD均值3431.078ms，本轮1199.416ms，但两组host延迟也有变化，因此应结合下面的分阶段计时确认收益，不能把跨组均值比当成严格同条件的孤立微基准。Q1/Q6的执行代码没有变化，本阶段未重测其延迟，沿用上一节记录时须保留测量日期。

另行冷启动的Q12 profile：Setup=99.355ms，Build Wall=834.015ms，probe阶段=137.332ms，source=0.467ms，Cleanup=7.940ms，Total=1079.109ms。上一阶段独立profile的Build Wall为3204.146ms。4槽并行build后，build阶段仍约占本次总时间77%，但已经明显缩短。Scan累计CU CPU=1446.165ms、Filter=1082.987ms；HashJoin.build为220次、CPU=8.781ms，load=1.036ms，probe+Aggregate=136.848ms。累计CPU包含并发工作，不能与墙钟直接相加。

输入和算法结果保持一致：build为lineitem的6001215行，probe为orders的1500000行，连接30988行，271块、1个驻内存build批次；内部读取1135394816字节，AFDM51761152字节，无spill，设备返回的池峰值4312880字节。没有降低读取量、提前终止或跳过聚合来取得本阶段收益。哈希写入成本很小，继续增加并发写入和局部哈希合并暂无测量依据；扫描解码、过滤及调度开销仍是主要优化方向。低预算部分状态归并的读取放大仍未解决。

Guest完整`join`回归34项检查通过，覆盖重复键、空build、两侧过滤、4MB build与分组双重spill、多页输出、字符串/原生回退，以及错误/取消/提前关闭后同backend复用。另以300ms超时取消完整Q12，再在同一backend连续两次CSD COUNT得到6001215，验证并行build在途请求排空后可继续使用。未修改设备CSF，未重复运行其上一阶段已通过的24项CTest和19项ASan/UBSan；本阶段重新编译安装PG扩展并执行真实Guest调度路径。

日志：主机`/tmp/pg-snia-parallel-build-join.log`、`/tmp/pg-snia-parallel-build-bench.log`、`/tmp/pg-snia-parallel-build-q12-profile.json`、`/tmp/pg-snia-parallel-build-cancel.log`；Guest回归目录`/tmp/pg-sina-queries.cslVMo`、测量目录`/tmp/pg-sina-queries.fBBPOM`。FEMU PCIe带宽模型未启用等既有限制仍适用；这是当前仿真环境对照，不代表真实设备或等总内存、等核数测试。

## 阶段3c：Q19、跨表过滤与哈希扩容（2026-10-22）

Q19使用项目已有qgen参数（Brand#42/#43/#33，数量9/19/28），保留三个OR分支中各自重复连接键的原SQL。另用Brand#12/#23/#34与数量1/10/20、公共等值条件在OR之外的写法验证规划不依赖SQL形状。两种查询均在4/64MB预算下与host逐字一致。完整覆盖增加到Q1/Q6/Q12/Q14/Q19，不能把本阶段视为完成全部22条查询。

### 首次测量暴露的估计容量问题

首次完整实现、尚未补build扩容时，三轮冷启动Q19 host为1659.353/1667.276/1729.557ms，均值1685.395ms；CSD为1920.956/1932.439/2597.747ms，均值2150.381ms、中位数1932.439ms，第三轮较高但保留。这一版本完整卸载却较慢。

独立profile（`/tmp/pg-snia-q19-profile.json`）确认part build扫描200000行，容量由过滤后估计行数固定，实际装满后即spill，即使64MB预算仍有余量。结果仅写/读各131072字节，却使build分2批、lineitem完整扫描两遍：probe累计12002430行、449块，内部读取1876811776字节。最终只有127个连接行进入聚合。Setup/Build/probe/source/Cleanup为62.889/47.656/1691.281/0.547/8.821ms，总计1811.194ms。Scan/Filter累计CU CPU为3193.569/1715.905ms，HashJoin.probe含跨表过滤及聚合仅22.184ms；主要代价是重复扫描/解码，不能归因于最终127行的聚合。

据此补充有界扩容：估计决定初始容量，后续装满先尝试增长，达到build预算或真实分配失败后才spill。复制扩容期间的新旧内存也受统一分配器限额约束；桶扩容失败时缩回记录数组新增空间，沿用原容量。没有提高任何内存配置。估计256行、实际70003行的设备差分测试在64MB下断言一批且无build spill，4MB下仍检查完整结果、多批和总峰值界限。

首次日志为`/tmp/pg-snia-q19-variant-and-bench.log`，Guest测量目录`/tmp/pg-sina-queries.QX8jBy`；这组数据用于解释扩容前的问题，不作为最终实现延迟。

### 最终扩容版本的冷启动延迟

全部构建、sanitizer与正确性回归结束后，独立运行`join-bench`。host/CSD交替各3轮，每次停止PG、清Guest页缓存并启动；profile关闭，逐次比较完整输出。设备仍4 CU、64MiB AFDM、64MB查询/物理workmem，原生PG仍使用既有work_mem和并行设置。没有同时运行编译或其他测试。

| 查询/路径 | 三轮 ms | 均值 ms | 中位数 ms |
| --- | --- | ---: | ---: |
| Q12/host | 1938.046 / 1917.202 / 2054.650 | 1969.966 | 1938.046 |
| Q12/csd | 1062.069 / 1082.074 / 1051.453 | 1065.199 | 1062.069 |
| Q14/host | 1571.375 / 1365.877 / 2319.934 | 1752.395 | 1571.375 |
| Q14/csd | 574.015 / 616.961 / 623.430 | 604.802 | 616.961 |
| Q19/host | 1591.329 / 2416.932 / 2256.246 | 2088.169 | 2256.246 |
| Q19/csd | 1076.218 / 1098.844 / 1029.659 | 1068.240 | 1076.218 |

本轮按均值，Q12约1.85倍、Q14约2.90倍、Q19约1.95倍。Q14/Q19的host波动较大，全部保留；扩容前Q19 host均值为1685.395ms，不能将两个阶段的host变化归为设备优化收益。更直接的依据是下面同一数据量的扫描次数和profile。Q1/Q6已通过完整结果回归，本阶段未重测其三轮延迟。FEMU未启用PCIe带宽模型等既有限制仍适用，本结果也不是等总内存、等核数对照。

最终Q19另行冷启动profile（`/tmp/pg-snia-q19-growth-profile.json`）：一个CSD CustomScan，part build扫描200000行，只有1个驻内存批次，lineitem probe扫描6001215行、物理过滤后128267行，跨表过滤后127行进入聚合；229块，内部读取955187200字节，spill写/读均为0。AFDM58150912字节，设备报告workmem池峰值3848720字节；严格逐查询容量边界仍由独立设备测试检查。

Setup/Build/probe/source/Cleanup分别为77.367/50.175/869.702/0.789/9.546ms，总计1007.579ms。Scan/Filter累计CU CPU分别1678.179/876.803ms，HashJoin.build/load仅0.206/0.011ms，probe含跨表Filter和聚合11.385ms。相较初次profile，少读921624576字节，少220次Scan/Filter，主要收益是避免无必要的第二次lineitem全表扫描。后续仍可优化heap解码和本地过滤，低预算外部聚合的归并读取放大依旧存在。

### 验证与复现

- 24项设备CTest和19项PG算子ASan/UBSan通过。扩容修改后重新通过24项CTest、3项Join ASan/UBSan；测试故意把70003行估为256行，在64MB断言单批无build spill，4MB验证多批、全部重复键组合、NULL与预算峰值，结束时workmem和上下文为0。
- Guest单表回归55项通过，覆盖Q1/Q6、高基数聚合、排序、IN/NULL/不同小数精度、空数组/NOT IN回退，以及prepared plan、报错/取消/提前关闭后的复用。最终扩容版本完整Join回归45项通过，包括跨表比较/附加等值/OR/NULL、4MB真实聚合spill、完整Q12/Q14、Q19两组参数和两种写法的4/64MB对照，及同backend资源回收。
- 4MB跨表分组压力用例得到7001组、318524个连接行，build分3批，聚合也有实际spill，按现有规则降低为1槽。较小的归并窗口仍可能产生明显读取放大；本阶段不掩盖这一限制。

主机日志：`/tmp/pg-snia-q19-native.log`、`/tmp/pg-snia-q19-asan.log`、`/tmp/pg-snia-q19-growth-native.log`、`/tmp/pg-snia-q19-growth-asan.log`、`/tmp/pg-snia-q19-final-guest.log`、`/tmp/pg-snia-q19-growth-guest-join.log`、`/tmp/pg-snia-q19-growth-bench.log`。Guest单表目录`/tmp/pg-sina-queries.1nwb9b`，最终Join目录`/tmp/pg-sina-queries.MabHoz`，最终性能目录`/tmp/pg-sina-queries.h4vM3R`。入口为`tests/pg-sina-csd-test.sh query|join|join-bench`。

## 阶段3d：连续多表Join与Q3（2026-10-22）

Q3使用原有qgen参数MACHINERY、1995-03-31，设备完整执行lineitem→orders→customer两个Join、按orderkey/date/priority分组、排序及LIMIT 10，Guest只编排与转换Datum。控制记录独立AFDM解决现有adapter不接受compute非零AFDM offset的限制，没有修改FEMU/SNIA。以下为最终代码、所有构建/回归/sanitizer结束后单独测量的数据。

### 冷启动延迟

仍为4 CU、64MiB AFDM、64MiB物理workmem和64MB查询上限；原生PG保持既有work_mem与并行配置。host/CSD交替各3轮，每次停止PG、清Guest页缓存并启动，关闭profile，完整输出逐次一致。全部样本保留，没有同时运行编译或本项目其他测试。

| 查询/路径 | 三轮 ms | 均值 ms | 中位数 ms |
| --- | --- | ---: | ---: |
| Q3/host | 3033.323 / 2548.007 / 2249.691 | 2610.340 | 2548.007 |
| Q3/csd | 4477.359 / 4182.655 / 4171.084 | 4277.033 | 4182.655 |
| Q12/host | 2012.860 / 2716.468 / 1929.892 | 2219.740 | 2012.860 |
| Q12/csd | 1071.458 / 1093.861 / 1088.480 | 1084.600 | 1088.480 |
| Q14/host | 3251.101 / 1513.215 / 1480.202 | 2081.506 | 1513.215 |
| Q14/csd | 575.419 / 657.107 / 664.584 | 632.370 | 657.107 |
| Q19/host | 1702.472 / 1717.042 / 1670.891 | 1696.802 | 1702.472 |
| Q19/csd | 1064.353 / 1050.492 / 1064.011 | 1059.619 | 1064.011 |

Q3当前CSD约为host的1.64倍延迟，没有加速。其他三条仍有收益；Q14 host首轮、Q12 host第二轮偏高，均值不能消除这些波动。Q1/Q6通过完整结果回归，本阶段未重测其三轮延迟。FEMU未启用PCIe带宽模型等既有限制仍适用，这不是等总内存或等核数对照。

### Q3瓶颈：build份额均分与重复probe

独立冷启动profile见`/tmp/pg-snia-chain-q3-profile.json`。只有一个CSD CustomScan，Join Count=2，build为orders和customer；两表原始行数共1650000。当前build总份额32MiB均分为每表16MiB。orders过滤后数据分6批，customer可驻留，因此组合数6。lineitem累计扫描36007290行，物理过滤后19208604行，两个Join之后29866行进入聚合，最终返回排序后的10行。1379块，内部读取5772886016字节；build spill写/读各41615360字节。局部聚合/排序有5个run，均驻内存，没有聚合run spill。

Setup/Build/probe/最终source与排序/Cleanup为53.602/340.891/3725.149/19.332/40.567ms，总计4179.542ms。Scan累计CU CPU为6691.273ms，Filter328.021ms，HashJoin.build/load为56.230/33.993ms，整条probe链含聚合1165.479ms。Scan/Filter各1379次，probe入口1320次；同CU内部的后续Join不另发NVMe，包含在probe计时内，不能再重复计入独立CSF。

AFDM45506560字节，设备报告workmem池峰值15868528字节，查询上限仍67108864字节。低于上限不表示应该继续缩小内存：均分预约与容量按2次幂取整使小表/聚合留下空闲份额，而orders仍反复spill。后续应改进各表预约和容量选择，或引入分区连接减少重扫；不能仅以增加CU或优化最终10行排序解决本次瓶颈。本阶段没有实现DuckDB完整的临时内存预约管理器或radix分区。

首次功能profile也确认6次probe重扫，但当时与native验证重叠，不用于延迟比较；上表及本节最终profile在全部验证结束后单独采集。

### 正确性、容量与失败回收

24项设备CTest和19项PG算子ASan/UBSan通过，最后增加物理份额约束后另外3项Join容量及3项sanitizer复测通过。差分测试包含三表链、后级键来自前级载荷、负键/重复键/NULL、空末级、分批结果、最终复合过滤，以及多个build批次与聚合spill同时发生；4MiB池、64MiB池内4MiB查询预算均断言完整结果、峰值界限和结束时零残留。另以查询预算为物理池两倍运行三表分组，检查物理上限。

Guest完整Join检查54项、单表/既有查询55项通过：三表/四表、跨表过滤、4MB链式分组、空表、断开连接图回退；Q3分别在16/64MB与host完整输出一致，Q12/Q14/Q19保持正确。最终版本另行核对Q3在16/64/128MB查询上限、64MiB实际池下结果一致；验证第一/后续build错误、链式聚合溢出、Q12/Q3取消之后，同一backend两次CSD COUNT都成功，确保排空在途请求后释放全部build状态。

主机日志为`/tmp/pg-snia-chain-all-native.log`、`/tmp/pg-snia-chain-all-asan.log`、`/tmp/pg-snia-chain-cap-native.log`、`/tmp/pg-snia-chain-cap-asan.log`、`/tmp/pg-snia-chain-guest-regression.log`、`/tmp/pg-snia-chain-final-check.log`、`/tmp/pg-snia-chain-bench.log`。Guest Join/单表目录为`/tmp/pg-sina-queries.Pxwb9Y`、`/tmp/pg-sina-queries.eCcDVi`，最终失败/容量检查目录`/tmp/pg-sina-chain-check.xmawDe`，性能目录`/tmp/pg-sina-queries.rIB7FR`。复现入口仍为`tests/pg-sina-csd-test.sh join|query|join-bench`。

## 阶段3e：Q3原生B-tree索引访问（2026-10-22）

原生PG独立冷启动EXPLAIN确认work_mem=4MB、max_parallel_workers_per_gather=2、shared_buffers=128MB。Customer哈希表约1472KiB、一个batch；聚合前各排序约1.1MiB、Top10为26KiB，整条计划Temp Read/Written均为0。PG先过滤/连接Customer和Orders，再147213次参数化查找lineitem_pkey，不是构建全部Orders后反复扫描Lineitem。原始计划保存在宿主`/tmp/pg-snia-q3-native-profile.json`，其单次EXPLAIN为3554.464ms，不与下表三轮均值混用。

本阶段准备原生索引的extent/根页，PG根据原生最优访问路径生成customer Hash Build、orders Scan/Filter/Hash Probe、lineitem B-tree Index Join。索引下探、TID提取、命中Heap的投影/过滤以及聚合均在设备SO内完成。页缺失以地址记录交给PG用原有SNIA内部LOAD填充；相邻目标槽和连续LBA合并。数据不经过Guest应用缓冲，也没有修改SNIA/FEMU核心或增加进程。

### 最终冷启动对比

所有构建、native/sanitizer和Guest正确性检查结束后，为八张冻结表重新prepare以保存可用索引映射，独立执行join-bench。仍为4 CU、64MiB AFDM、64MiB物理workmem、64MB查询预算；PG保持4MB work_mem和原并行配置。每条SQL之前重启PG并清Guest页缓存，关闭profile，交替host/CSD各3轮，逐次完整输出一致。

| 查询/路径 | 三轮 ms | 均值 ms | 中位数 ms |
| --- | --- | ---: | ---: |
| Q3/host | 2349.495 / 2629.771 / 2422.259 | 2467.175 | 2422.259 |
| Q3/csd | 1867.821 / 1887.052 / 1850.005 | 1868.293 | 1867.821 |
| Q12/host | 1926.758 / 2126.550 / 1961.555 | 2004.954 | 1961.555 |
| Q12/csd | 1095.991 / 1098.803 / 1082.422 | 1092.405 | 1095.991 |
| Q14/host | 1492.252 / 1774.612 / 1655.635 | 1640.833 | 1655.635 |
| Q14/csd | 624.483 / 672.294 / 665.888 | 654.222 | 665.888 |
| Q19/host | 1676.636 / 1836.133 / 1796.875 | 1769.881 | 1796.875 |
| Q19/csd | 1079.055 / 1083.574 / 1075.573 | 1079.401 | 1079.055 |

Q3本轮host/CSD均值为2467.175/1868.293ms，约1.32倍；原六轮重扫实现的CSD历史均值为4277.033ms，当前降低约56%。历史对照来自上一节，未将旧实现与新实现混装在一轮测试中。Q12/Q14/Q19仍通过完整结果比较，延迟与此前量级一致。FEMU未启用PCIe带宽模型等既有限制仍适用；这是当前模拟配置下的结果，不是等核数、等总内存的硬件性能结论。

### Q3独立profile与剩余开销

三轮计时结束后再次冷启动、单独开启profile：Setup/Build/probe/最终source和排序/Cleanup为50.482/20.051/1653.953/20.496/6.374ms，设备总计1751.356ms，EXPLAIN执行总计1752.861ms。一次profile不替代上述关闭profile的三轮均值。

- Orders顺序probe扫描1500000行，局部过滤后737165行；Customer原始build输入150000行，过滤后的哈希表驻留，build组合数1。Index Join解码589580个命中Heap Tuple，右表shipdate过滤及连接后剩29866行进入聚合，最终返回10行。
- 顺序Scan/Filter各59次，索引/Heap页加载104096次；总内部读取1095892992字节（约1.02GiB），包含顺序读取和重复加载的索引/Heap页，不是唯一页数。上一阶段为5772886016字节、六轮Lineitem顺序扫描。
- Scan累计CU CPU为276.133ms，Filter14.545ms，build/load为2.192/0.620ms，含Index Join和聚合的probe为881.789ms。probe入口调用7733次；剩余数千次继续执行及细粒度页读取值得进一步拆分，不把墙钟减CPU的差值全部称作设备I/O。
- AFDM45506560字节，设备报告workmem池峰值7502880字节；该峰值仍是分配器池统计，不是隔离的逐查询峰值。聚合/排序有5个驻内存run，build和聚合均无spill。没有通过提高64MB查询预算来避免本次重扫。

第一版索引路径逐页建立Storage请求，独立诊断约3482ms。合并相邻目标槽/连续物理范围后，最终独立profile约1751ms。两者仅为开发诊断，不混入最终三轮样本；改进的是命令和调度开销，不是凭空提高模拟NAND带宽。

### 验证与复现

25项设备CTest和Guest页布局测试通过；相关向量/扫描/Hash Join的6项ASan/UBSan通过，补充页槽复用、HOT、空根页、坏页错误后再次运行索引sanitizer通过，环境限制仍关闭LeakSanitizer。索引原生差分测试涵盖2/4/8字节键、复合前导键、重复值跨叶页、NULL外侧键、无匹配、513行跨批输出、256个不同Heap页超过128输入槽、右侧过滤、空根、HOT重定向、取消及坏页错误后的零残留。

Guest索引专项13项检查通过：复合索引NULL载荷/重复值、smallint/bigint、局部与跨表过滤、4/64MB差分、空表、REINDEX后旧映射回退、计算溢出和取消后的同backend复用；Q3在4/16/64/128MB查询预算下与host逐字一致。原有Join54项、单表/既有查询55项回归全部通过，包含既有低内存spill、错误/取消和source早关回收。

宿主日志：`/tmp/pg-snia-index-native-tests.log`、`/tmp/pg-snia-index-asan-tests.log`、`/tmp/pg-snia-index-asan-final.log`、`/tmp/pg-snia-index-guest-layout.log`、`/tmp/pg-snia-index-guest-final.log`、`/tmp/pg-snia-index-join-regression.log`、`/tmp/pg-snia-index-query-regression.log`、`/tmp/pg-snia-index-bench.log`；最终profile为`/tmp/pg-snia-index-q3-profile.json`，原始时延为`/tmp/pg-snia-index-timings.tsv`。Guest专项目录`/tmp/pg-sina-index.0GX7mE`，Join/单表目录`/tmp/pg-sina-queries.y4KXAX`、`/tmp/pg-sina-queries.45y6Kx`，性能目录`/tmp/pg-sina-queries.vEjZQu`。复现入口为`tests/pg-sina-csd-test.sh index|join|query|join-bench`。

## 阶段3f：运行期字符串字典与 Q5（2026-10-22）

Q5 的 `n_name` 是无约束字符串，静态 SQL 常量字典不适用。首版用查询级运行期字典 AFDM：Scan 插入去尾空格后的字节并输出 ID，Join/分组只传 ID，Aggregate.source 在最终结果前恢复字节；字符串仍写回原 16 字节 cell，最大 15 字节。Q5 的 nation 名称最长 14 字节，因此没有改变 join/sort/spill 记录布局。字符串 `ORDER BY` 暂回退，避免按首次出现 ID 排序产生错误顺序。

多表连接顺序从“最小过滤后行数”改为“单列唯一键优先、唯一键候选中较大关系优先”。此前 Q5 先按 `c_nationkey=s_nationkey` 连 customer/supplier，先生成约 customer×同国 supplier 的中间结果，单次实测超过 42s；调整后链为 lineitem→orders→customer→supplier→nation→region，最后执行 nationkey 残差。Build 预算按每表过滤后行数/投影列数/2 的幂容量计算“单批所需字节+1MiB”后按比例分配；默认 64MB 下五张 build 均为 1 批，lineitem 只扫描一次。

### 冷启动对比

每条 SQL 前停止 PG、清 Guest 页缓存并重启，host/CSD 交替各 3 轮，关闭 profile，完整结果逐次 cmp。设备仍为 4 CU、64MiB AFDM/workmem、64MB 查询预算；host 保持原生并行与 `work_mem=4MB`。

| 查询/路径 | 三轮 ms | 均值 ms | 中位数 ms |
| --- | --- | ---: | ---: |
| Q5/host | 2460.080 / 1926.239 / 1939.466 | 2108.595 | 1939.466 |
| Q5/csd | 989.475 / 929.035 / 919.904 | 946.138 | 929.035 |

Q5 均值约 2.23 倍、中位数约 2.09 倍；结果与 host 逐字一致。这是当前 FEMU 配置下的样本，不是等总内存/等核数硬件结论，也不代表已实现通用原生 path 编译器；Q5 仍走 Hash 链，优势来自唯一键连接顺序、单批 build 和一次 lineitem 扫描。

16MB 查询预算下 Q5 CSD=6440.564ms，结果仍一致，profile 显示12个build批次、spill写17367040字节/读39780352字节，属于内存受限；默认 64MB 不采用该配置。Q5 200ms 超时取消后，同一 backend 连续两次 CSD COUNT 均返回 6001215。既有 Q1/Q6/Q12/Q14/Q19 冷启动结果无回退，Q3 单次冷启动 1869.695ms。

原始数据：Guest `/tmp/q5-bench.log`、`/tmp/q5.host.*.log`、`/tmp/q5.csd.*.log`、`/tmp/q5_mem_16.log`、`/tmp/q5_cancel.sql`；复现入口 `tests/pg-sina-csd-test.sh join|join-bench`。

## 阶段3g：Q10 长字符串与 FD 分组键（2026-10-22）

Q10 的 SQL 有 7 个分组列，PG 按主键函数依赖把计划压缩到 `c_custkey,n_name`；本阶段规划端把其余非聚合输出列重新加入设备分组键，因此设备仍只保存整数字典 ID，不需要变长 FD payload。运行期字典扩成 1,048,576 项/20MB 大档；bucket 由 64KiB 拷贝清零，避免大块 FDM fill。最终结果增加 64KiB arena，超过 15 字节的地址/备注等按 offset/len 回传；partial state、sort run 和 spill 仍为定长 cell。

大字典约 33.5MB，FDM 容量循环在字典较大时先把每槽输入块从 4MiB 缩到 1MiB，再决定是否减少 worker。Q10 最终保持 4 CU、3 张 build 各 1 批、无 spill；profile 的 CSD AFDM 指标为 32,894,976 字节（不含完整字典区），workmem 峰值 45,794,960 字节，Joined Rows 114028，Sort Runs 17。

### 冷启动对比

host/CSD 交替各 3 轮，每条 SQL 前停止 PG、清 Guest 页缓存并重启，关闭 profile，完整结果逐次 cmp。

| 查询/路径 | 三轮 ms | 均值 ms | 中位数 ms |
| --- | --- | ---: | ---: |
| Q10/host | 3050.724 / 2028.636 / 2060.642 | 2380.000 | 2060.642 |
| Q10/csd | 1356.773 / 1388.854 / 1429.740 | 1391.789 | 1388.854 |

Q10 均值约 1.71 倍、中位数约 1.48 倍；全部20行与host逐字一致。独立profile的EXPLAIN ANALYZE为1456.291ms，包含程序加载和回收，不与关闭profile的三轮均值混用。

16MB 查询预算下 Q10 CSD=7433.795ms，结果一致；profile 显示 12 个 build 批次、54 个 sort run、33 个 spill run，spill 写23,789,568字节/读65,208,320字节，workmem峰值14,237,584字节。200ms 超时取消后同一 backend 连续两次 CSD COUNT 均返回6001215。既有 Q1/Q5/Q6/Q12/Q14/Q19 单轮冷启动结果一致。

当前限制：字符串 `ORDER BY` 仍回退；字典不 spill，Q10 的 customer 全量字符串约19.6MB，依赖20MB区；更长/更大基数需要 FD payload 或可 spill 字符串。原始日志：Guest `/tmp/q10-bench.log`、`/tmp/q10.host.*.log`、`/tmp/q10.csd.*.log`、`/tmp/q10_16.log`、`/tmp/q10_profile16.sql`；入口 `tests/pg-sina-csd-test.sh join|join-bench`。

## 阶段3h：Q9 复合连接键（2026-10-22）

Q9 新增复合连接键 `(l_partkey,l_suppkey)=(ps_partkey,ps_suppkey)`，避免 partsupp 单键连接后的四倍中间行；同时加入 Q_YEAR、% 通配 Scan 和字符串排序。Build 组合从42批降到6批，但 64MB 下 orders 1.5M 与 partsupp 800k 仍不能同时单批驻留，6 次 lineitem 重扫是剩余瓶颈。

### 冷启动对比

host/CSD 交替各 3 轮，完整 175 行结果逐次 cmp，均通过。

| 查询/路径 | 三轮 ms | 均值 ms |
| --- | --- | ---: |
| Q9/host | 4161.373 / 3068.764 / 3073.949 | 3434.695 |
| Q9/csd | 7702.432 / 7386.411 / 7677.284 | 7588.709 |

Q9 均值约2.21倍 host；profile Execution 8073.135ms，Build Batches=6、Internal Read 5921865728字节、Spill Write 98631680/Read 158793728字节、4 workers。32MB 查询预算 CSD=42491.859ms，结果一致；16MB 超过55s未返回，当前最低验收内存32MB，推荐64MB。

### Q9提交后 Q5/Q10 复核

Q9 的复合键/连接顺序/预算改动影响全部多表查询，提交后重新做3轮冷启动：Q5 host=2935.133/1967.948/1922.216ms（均值2275.099），CSD=1306.055/1170.357/1162.407ms（均值1212.940），约1.88倍；Q10 host=2186.796/2260.944/2413.306ms（均值2287.015），CSD=1449.362/1439.418/1706.491ms（均值1531.757），约1.49倍。两条各3轮完整结果 cmp 通过。16MB 查询预算 Q5=11102.431ms、Q10=5062.922ms，结果一致；该16MB档为内存受限，不使用旧记录中的 Q5 6440ms/Q10 7433ms。Q5/Q10 的独立profile数字未在本轮重采，性能表以这三轮关闭profile数据为准。

索引实验：orders_pkey 链尾在复合键后 Index Heap Tuples 323844、Pages Loaded 62849，耗时10.9s，仍慢于Hash，已撤回。下一步需要分区Hash Join或probe物化，将6次重扫压到1–2次。原始日志：Guest `/tmp/q9-bench2.log`、`/tmp/q9.host.*.log`、`/tmp/q9.csd.*.log`、`/tmp/q9_32.log`；入口 `tests/pg-sina-csd-test.sh join|join-bench`。

## 阶段3i：Q7 重复 RTE 自连接（2026-10-22）

Q7 的 nation n1/n2 是同一基表两个实例；设备按 RTI 分别描述列区间并进入连接链。OR 字符串等值使用静态字典，运行时未知字符串ID0会报错而不是合并。

| 查询/路径 | 三轮 ms | 均值 ms |
| --- | --- | ---: |
| Q7/host | 3251.633 / 2329.948 / 2404.706 | 2662.096 |
| Q7/csd | 3676.067 / 3691.620 / 3619.526 | 3662.404 |

Q7 均值约1.38倍；4行完整结果3轮cmp通过。profile Execution 3590.978ms、Build Batches=4、Internal Read 3931471872字节、Spill Write 66191360/Read 126287872字节、4 workers。16MB查询预算 CSD=8348.123ms，结果一致；200ms取消后同backend连续两次CSD COUNT均返回6001215。原始日志：Guest `/tmp/q7-bench.log`、`/tmp/q7.host.*.log`、`/tmp/q7.csd.*.log`、`/tmp/q7_16.log`。

## 阶段3j：Q8 8表/7join（2026-10-22）

Q8 的 8表/7join 在64MB单批完成；`sum(CASE...)/sum(...)` 由 final_expression 在分组后计算，CASE 条件使用静态字典。

| 查询/路径 | 三轮 ms | 均值 ms |
| --- | --- | ---: |
| Q8/host | 2951.971 / 2393.437 / 2259.607 | 2535.005 |
| Q8/csd | 846.824 / 859.838 / 901.284 | 869.315 |

CSD 约快2.9倍；2行结果3轮cmp通过。profile Execution 807.220ms、Build Rows=1860055、Build Batches=1、Internal Read 1200168960字节、Spill 0、4 workers。16MB查询预算 CSD=11484.375ms，结果一致；200ms取消后同backend连续两次CSD COUNT均返回6001215。原始日志：Guest `/tmp/q8-bench.log`、`/tmp/q8.host.*.log`、`/tmp/q8.csd.*.log`、`/tmp/q8_16.log`。

## 阶段3k：Q4 EXISTS 半连接（2026-10-22）

Q4 以 orders 为 probe、lineitem_pkey 为链尾索引，每个 order 最多输出一次；`l_commitdate<l_receiptdate` 在 B-tree heap tuple 上先过滤。小结果快速路径在字符串排序前解析字典，修复了 Q4 按字典 ID 排序的问题。

| 查询/路径 | 三轮 ms | 均值 ms |
| --- | --- | ---: |
| Q4/host | 2608.291 / 1945.221 / 2094.175 | 2215.896 |
| Q4/csd | 1312.971 / 1202.337 / 1332.642 | 1282.650 |

CSD 约快1.7倍；5行结果3轮cmp通过。profile Execution 1183.481ms、Internal Read 727531520字节、Index/Heap Pages Loaded 62715、Index Heap Tuples 52544、Spill 0、4 workers。16MB查询预算 CSD=1237.619ms，结果一致；200ms取消后同backend连续两次CSD COUNT均返回6001215。原始日志：Guest `/tmp/q4-bench2.log`、`/tmp/q4.host.*.log`、`/tmp/q4.csd.*.log`、`/tmp/q4_16.log`。

## 阶段3l：Q13 LEFT JOIN/两级聚合（2026-10-22）

Q13 采用 NULL 垫底行 + nested source 二级聚合，41行结果3轮cmp通过。

| 查询/路径 | 三轮 ms | 均值 ms |
| --- | --- | ---: |
| Q13/host | 1312.491 / 1134.331 / 1150.842 | 1199.221 |
| Q13/csd | 5631.952 / 5418.754 / 5892.680 | 5647.795 |

CSD约4.7倍 host，性能未达标。同规模合成表（150k customer/1.5M orders）32MB查询预算完成，CSD=11941.874ms；16MB长时间不返回，最低验收内存暂定32MB；Q13 profile未采集。原始日志：Guest `/tmp/q13_host_*.log`、`/tmp/q13_csd_*.log`。

## 阶段3m（部分）：Q18 HAVING（2026-10-22）

Q18 使用 IN→HAVING 消解，小规模三表 host/CSD 结果逐字一致。SF1 规模装载阶段 guest 失去响应，尚无 64MB/16MB/三轮性能数据，未标记完成。
