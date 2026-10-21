# 开发阶段与验证记录

分支 pg_sina_csd_dev，基线 sina-femu-csd（f5a7df9dd）。每个可验收阶段单独提交。旧 pg_csd_dev 仅作实现参考，不移植其 Python 代理、AFDM 工作区或 FEMU SQL runtime。

## 阶段0：PG扩展、静态映射与共享heap通路（完成）

- 新增CMake构建的PG12扩展pg_sina_csd。sina_csd.prepare(regclass,device)检查持久化heap、visible/frozen覆盖，等待PG checkpoint，再读取FIEMAP。记录物理列布局、relfilenode、Guest boot ID、namespace/LBA几何和连续的全表extent映射，保存在扩展自己的三张表中。支持分区偏移和文件分段；无常驻连接或守护会话。
- Guest工具pg-sina-csd-check链接libpq和libsnia，使用标准API构建SERIAL LOAD→heap_count batch；复用描述并逐块更新，原始heap不经过工具缓冲，仅读回小型计数结果。原生PG COUNT独立核对结果，本阶段不作为性能比较。
- 设备heap_count.c只校验和解析PG12/x86-64/8KiB页，不接触文件、NVMe或PG规划信息。MR输入输出分离；cparam1表示物理列数，cparam2表示有效字节，正确处理尾块。无跨调用私有状态，统一链接workmem但无动态工作区。
- 新增宿主tests/pg-sina-csd-test.sh的build/prepare/check/sql入口。数据库schema名sina_csd避开PostgreSQL保留的pg_前缀；扩展/分支名称保留用户指定拼写。
- 当前Guest原先没有PG安装或数据集；通过既有脚本编译安装PG12.22和pg_visibility，恢复用户已有tpch_s1归档并启动数据库。复用现有80%数据分区，保留原256MiB排序文件；不重新分区、不重启FEMU。
- 宿主6项设备CTest通过；Guest扩展在-Werror下编译安装成功，真实PG头文件布局检查通过。设备count测试覆盖页/tuple越界、未冻结与未visible页、无效输入长度和非tuple line pointer；ASan/UBSan通过。检查环境不支持LeakSanitizer的ptrace模式，本无动态分配的测试运行时关闭detect_leaks。
- 八张表真实设备行数与PG完全一致：region=5、nation=25、supplier=10000、customer=150000、part=200000、partsupp=800000、orders=1500000、lineitem=6001215。lineitem映射112503页、12个extent，4MiB块共220次batch，内部读取921624576字节。
- 24KiB块读取part通过：4097页、1366个chunk、200000行；验证块大小无关和最后的不满块。SQL测试拒绝未冻结表，冻结后保存schema/extent；真实设备验证含NULL的3行表与空表，测试结束删除临时表和映射。

阶段0验收时的边界：SQL尚未自动卸载；只有准备函数和独立诊断工具。程序索引500供诊断独占，暂不支持跨backend并发索引管理；工具sudo访问NVMe，不给PG服务增加设备权限。准备后的实验表保持不变，设备/文件/schema变化后重新准备。

## 阶段1：执行路径选择与Q1/Q6完整卸载（完成）

执行路径与冷启动规则先独立提交为 e6d45909f；本阶段在该规则下实现实际 SQL 卸载。

- `pg_sina_csd.exe_path`：默认 `host`，采用原生 PG 计划；`csd` 为支持的查询增加 CustomScan，不能处理的计划继续由 PG 执行。切换时失效 prepared plan 缓存。`auto` 写入后续阶段，目前不接受该值。
- 从 PG 表达式树生成投影列、过滤表达式、聚合、分组、排序和 LIMIT/OFFSET 描述，不识别 SQL 文本或 Query 编号。支持 Q1/Q6 的所有数据算子；当前只接入单表聚合的完整上层计划，不将部分原生执行声称为全卸载。
- PG backend 直接链接并调用 SNIA，不读取 heap 到应用缓冲，不执行 SQL 数据计算。原始数据通过内部 LOAD 进入 AFDM，没有改动 FEMU 或 SNIA 核心。安装时配置设备访问权限，不使用辅助进程或代理。
- 阶段1初版的第一条 pipeline 为 LOAD→Scan→Filter→Aggregate.sink；按设备容量，构建最多4个并行分支的 HYBRID batch，当前异步执行槽改进见本文末尾。分支间使用独立缓冲和私有聚合状态，重复处理4MiB块直到输入结束。
- 第二条 pipeline 为 Aggregate.source→Sort→Limit，采用 SERIAL batch；合并局部状态、精确 AVG 除法、排序、LIMIT 都在设备执行。Guest 只转换结果类型并返回客户端。
- Scan 提取所需列，Filter 生成 selection vector；表达式树复用公共子表达式。参考本地 DuckDB table_scan.cpp、physical_filter.cpp 和 physical_hash_aggregate.cpp 的投影、选择向量及局部聚合组织；不直接复用其存储格式；该阶段初版没有显式 SIMD，后续 Filter 向量化见性能分析。
- numeric 以定点整数精确计算并保留显示精度，AVG 对齐 PG numeric 的精度与舍入。PG12 的 palloc 只保证8字节对齐，含 __int128 的结果显式16字节对齐。
- AFDM 仅保存输入输出、计划和句柄，聚合状态及排序工作区使用共享 workmem 分配器。错误和取消时调用设备清理入口释放状态，再释放 AFDM、batch 和程序。查询持有 namespace 文件锁，当前只支持单个设备查询进入，不属于多会话并发调度实现。
- EXPLAIN 展示真实 pipeline、投影列、CU 数、行数、内部读取和内存指标。Q1/Q6 以真实 SF=1 heap 验证，每条扫描6001215行、内部读取921624576字节。

验收：7项设备 CTest 和 Guest PG12 页布局测试通过；计数及列批次聚合的 ASan/UBSan 测试通过（环境限制关闭 LeakSanitizer）。SQL 回归覆盖 NULL/空输入/无匹配、布尔条件、精确 SUM/AVG/COUNT、排序/NULL顺序/OFFSET/LIMIT、原生回退、prepared plan 路径切换及100ms超时取消后完整设备复用。Q1四组全部列（含精度和小数位）与Q6结果逐字节一致，另验证Q1 LIMIT 1。每次执行均独立冷启动。

阶段1验收时的限制：最多256个分组，排序仅处理聚合结果；numeric 输入仅支持有限、至多两位小数并可表示为int64百分数的值。规划时已知不支持的能力回退PG；运行时数据越界或组数超预期报错。尚未实现通用高基数聚合/行排序spill、Join或所有22条TPC-H。

## 统一冷启动测量规则

所有 TPC-H 被测语句执行前停止 PG，清空 shared buffers；sync 并清除 Guest OS page cache，再启动 PG。测试期间关闭 autovacuum。host 与 csd 完全采用同一流程，EXPLAIN ANALYZE 也单独冷启动。设备每次新建和回收 AFDM/workmem，不复用前次输入。FEMU 模拟 NVM 字节本身是存储介质，不作为缓存清除。

普通 PG 服务重启且文件不变时复用映射；Guest/FEMU 重启、数据恢复或文件/schema 变化时重新 prepare，不为每次查询重新 FIEMAP。性能采用各3轮、交替顺序，逐次核对输出；包含程序加载、查询及回收时间，不包含重启/清缓存时间。

```sh
bash tests/pg-sina-csd-test.sh query
bash tests/pg-sina-csd-test.sh bench
```

## 阶段1初版冷启动性能（2026-10-21）

以下为初版的测量记录，包含现已移除的查询辅助进程开销，不代表移除后的性能。

现有 SF=1 tpch_s1，PG12.22，Guest 8 vCPU/8GiB；原生 PG 配置 shared_buffers=128MiB、work_mem=4MiB、max_parallel_workers_per_gather=2。设备4 CU、64MiB AFDM、64MiB workmem。未更改原生PG配置来制造加速。三轮交替顺序，单位为秒：

| 查询 | host 三轮 | csd 三轮 | host均值 | csd均值 | host/csd |
| --- | --- | --- | --- | --- | --- |
| Q1（四组完整结果） | 4.905493 / 4.380100 / 4.383609 | 2.005434 / 1.646425 / 1.628251 | 4.556401 | 1.760037 | 2.59 |
| Q6 | 1.500574 / 1.749342 / 2.116437 | 1.753388 / 1.868625 / 1.585608 | 1.788784 | 1.735874 | 1.03 |

Q6差异小于本次测量波动，不能据此认定有明显加速。Q1测试去掉原查询的LIMIT 1以核对四组全部结果；另做LIMIT 1正确性验证。Q6保留原LIMIT 1。原始每轮耗时由 bench 保存到 Guest 的结果目录，后续同样用该脚本生成，不把一次 EXPLAIN 的耗时当作三轮均值。

单独冷启动的 EXPLAIN ANALYZE 确认二者只有 SniaCsdAggregate，无原生 Scan/Agg/Sort；设备均4 CU、220块、扫描6001215行、内部读取921624576字节。Q1投影7列，匹配5884033行，AFDM64385024字节；Q6投影4列，匹配119810行，AFDM45248512字节。workmem峰值679872字节；AFDM与私有工作内存均未超过各自64MiB限制。这些是少量分组的聚合，不会触发spill；后续阶段必须专门覆盖内存不足。

## 直接调用SNIA与安装时权限配置

- 按用户要求，禁止通过临时辅助进程、代理或守护进程实现权限转发，规则保存到根目录 AGENTS.md。
- install-postgre.sh 末尾配置 /dev/nvme0 与 /dev/nvme0n1 的设备访问及 udev 规则，给 postgres 设置 CAP_SYS_ADMIN，以满足 vendor passthrough 检查。已有安装重新运行脚本也会应用；需重启 PG，重新安装二进制后需重新设置。该配置面向本专用实验 Guest。
- 删除 worker 可执行目标、进程启动和 socket 传输。原 batch 调度成为 executor/batch.c，由 PG backend 直接调用；静态映射读取整理到 executor/mapping.c。
- 查询仍按 batch 边界响应取消，先释放设备状态再让 PG 处理 pending interrupt，避免跨 SNIA C++ 调用栈跳转。保持正常、失败与取消路径统一回收。
- 验收：Guest 编译及页布局测试通过；安装脚本重复执行成功，udev 规则已安装，运行中 PG 的 UID/GID 为1000且 CapEff=0x200000。扩展动态链接 /usr/local/lib/libsnia.so，旧 worker 安装及构建产物均已删除。冷启动 Q1/Q6 和全部 SQL 边界回归通过；100ms 超时后同一个 backend 连续执行两次设备 COUNT 成功，再次冷启动的完整 Q1 与原生结果一致。

## Q6持平的诊断（移除辅助进程后）

重新按同一冷启动脚本交替执行各3轮：Q1 host=4.776122s、csd=1.807953s（2.64倍）；Q6 host=1.497217s、csd=1.536224s，仍无明显加速。Q6的host三轮为1.445149/1.405368/1.641134s，csd为1.540186/1.618171/1.450314s；结果均一致。因此辅助进程并非持平的充分解释。

另一次独立冷启动诊断：host EXPLAIN 实际为 Parallel Seq Scan→Partial Aggregate→Gather→Finalize Aggregate，两个并行worker加主进程，耗时1.577890s；csd耗时1.572422s，4 CU。两边均扫描112503页，设备匹配119810/6001215行（约2%），仅读回最终结果。没有spill，workmem峰值679872字节。

SNIA已有计时记录D0=220次、累计1.523479s，E1=663次、累计3.393881s，F2=5次、累计0.000237s。这是并发请求各自墙钟时间之和，包含等待/提交/完成，不能相加当作查询时间，也不能直接当作纯CU计算时间。原生PG的I/O读时间3.016022s同样包含并行进程累计时间。现有数据仍不足以划分Scan、Filter和Aggregate的精确占比。

该次诊断对应的实现中可确认的额外成本：Scan先为全部行解码四列并写入列批次，再运行逐行递归解释的Filter；即使98%行会被过滤，extendedprice也已转换。当前有列批次和selection vector，但没有显式SIMD，也没有按过滤结果延后解码聚合输入。每4MiB块分为LOAD/Scan/Filter/sink命令，各轮之间等待完成，尚无跨轮预取。Q1有大量匹配行和多项numeric聚合，定点局部聚合的收益更易覆盖这些共同成本。

核实实际QEMU设备命令行及默认值：未设置pcie_bandwidth_mbps/pcie_prop_delay_ns，默认均为0，PCIe带宽模型未开启；ch_xfer_lat也为0。当前内部LOAD仍使用与普通读取相同的FTL/NAND时序，跳过Guest DMA；不能据此声称实验已体现真实设备的内外部带宽差。没有在现有运行中修改SSD参数或重启FEMU。

后续优化应先补每CSF与关键路径计时，再测按谓词延后列解码、批量表达式执行、读算重叠；PCIe/通道带宽模型需按目标设备校准，不能通过任意限速制造加速。诊断完成后已关闭临时SNIA日志计时环境。

## 可关闭的逐算子计时

- 新增运行时 GUC `pg_sina_csd.profile`，默认 off，无须使 prepared plan 重新规划。开启后 EXPLAIN ANALYZE 展示 Scan、Filter、Aggregate.sink/source、Sort、Limit 的调用次数、累计墙钟/线程CPU时间及单次最大墙钟时间；TEXT可读，JSON保留数值字段。
- 计时集中在设备 profile.c 的入口包装。CMake 编译时仅重命名算法入口，再导出带可选计时的原入口；设备算法功能源码未改。关闭时不读取时钟，不申请/读取计时 AFDM，仅经过入口条件判断。
- 开启时，每个分支附加一个计数输出 AFDM，cparam1=1。包装层移除附加输出并清零该参数后调用原函数，不改变原 MR 顺序或返回值。分支内顺序写、分支间独占计数，PG 在 batch 完成后合并，无锁或原子计数。
- PG batch 执行边界另记 Setup、Pipeline 1、Pipeline 2、Cleanup、Total 墙钟时间。内部 LOAD 包含在 Pipeline 1 中，未单独拆分；不把剩余时间直接标为I/O。Total不包括规划、静态映射读取和结果Datum转换。
- 所有算子累计值跨CU求和；不得将其相加当作查询延迟。设备墙钟包含宿主调度停顿，线程CPU时间反映FEMU宿主CU线程实际占用；不包含NVMe排队或模拟介质延迟，不等同于真实SSD处理器周期。
- 无 FEMU/SNIA 核心改动，无辅助进程。开启时有时钟读取、小量AFDM及输出传输开销；发布和正常性能比较时关闭。具体使用见扩展及设备README。

验收：8项设备CTest、Guest编译/页布局及3项PG算子ASan/UBSan测试通过。计时开关前后Q1/Q6冷启动结果与原生PG一致；验证逐算子调用次数、JSON解析、阶段时间加和，以及默认关闭时无profile输出且节省每CU一个4KiB计数AFDM。默认路径的完整SQL回归通过；开启计时后100ms超时取消，再在同一backend连续执行设备查询并完整复用设备成功。统一测试入口增加 `tests/pg-sina-csd-test.sh profile`。

一次独立冷启动Q6示例（非三轮性能结论，单位ms）：

| 算子 | 次数 | 累计墙钟 | 累计CU CPU | 单次最大墙钟 |
| --- | --- | --- | --- | --- |
| Scan | 220 | 1240.677 | 1238.918 | 9.289 |
| Filter | 220 | 1355.810 | 1353.566 | 9.033 |
| Aggregate.sink | 220 | 15.528 | 15.094 | 0.316 |
| Aggregate.source | 1 | 0.047 | 0.046 | 0.047 |
| Limit | 1 | 0.005 | 0.003 | 0.005 |

该次执行端 Total Wall=1405.336（Setup=74.103、Pipeline 1=1323.924、Pipeline 2=0.642、Cleanup=6.667）。Scan/Filter占据设备算子CPU累计时间的绝大部分，聚合CPU开销很小；仍需进一步测量内部I/O和提交/调度，才能解释整个查询关键路径。

## 后续阶段与TPC-H覆盖目标

每阶段单独提交，记录算子能力、正确性、回退范围及冷启动性能；最终目标是22条查询的数据算子均可在 `csd` 路径执行。表中查询是阶段验收目标，并不表示当前已经支持。

| 阶段 | 实现内容 | 优先验收 |
| --- | --- | --- |
| 2（完成） | 行排序/TopN与高基数聚合已接入，详见阶段2a/2b。workmem优先，NEED_STORE/LOAD/FREE驱动spill与重入，保留sink/source生命周期 | 大数据排序、分组和强制小内存正确性 |
| 3a（完成） | 两表整数INNER Hash Join、投影、build分批spill及probe重扫，搜索CASE、前缀LIKE与聚合后数值运算 | Q14 |
| 3b.1（完成） | Join后分组、聚合spill重入、SQL常量字符串字典与受限投影/排序 | Q12 |
| 3b.2 | build侧扫描并行、多表Join、一般字符串投影、连接后复合谓词、分区spill以减少probe重复读取 | Q3/Q5/Q10/Q19 |
| 4 | 多路Join与分组，子查询去相关，semi/anti join，EXISTS/NOT EXISTS，LIKE/SUBSTRING、派生表与中间聚合结果复用 | Q4/Q7/Q8/Q9/Q11/Q13/Q15/Q16/Q18/Q20/Q21/Q22 |
| 5 | 补齐剩余表达式与嵌套聚合/标量子查询语义，逐条核查设备覆盖与原生结果 | Q2/Q17及全部22条 |
| 6 | `auto`：基于可支持性、估计行数、投影宽度、传输/计算/内存与spill成本选择host/csd；支持混合子计划 | 全部22条，比较host/csd/auto与实际计划 |

每实现一个算子，先看 DuckDB 中可借鉴的投影、批处理、选择向量、局部状态和阻塞边界，再决定是否需要 SIMD 等优化。多 CU 由主机显式构建依赖图；设备内部 I/O 与计算重叠另做测量后实施，不默认给所有请求加入并行策略。不能用少数Query通过代替完整SQL支持，也不能把FEMU宿主CPU执行解释为真实SSD的计算性能。

## 瓶颈统计与第一轮向量过滤

三轮host/csd/profiled冷启动统计、独立CU perf热点及原始数据位置见[性能分析](performance.md)。新增 analyze 采集入口，基线统计提交9868e22bb。Q6原设备CPU约48%为Scan、51%为Filter；Q1约50%为聚合（主要为表达式解释）、37%为Scan。第一pipeline占执行端墙钟约95%，CU函数有效忙碌度约49%/64%，仍有读取和命令等待。

据此参考DuckDB实现复合条件的类型化列比较/AVX2选择向量。单条件候选在Q1出现查询回退，已通过交替A/B验证并保留原单谓词路径；最终Q1均值1.490→1.479s，基本持平。Q6复合条件A/B均值1.328→0.977s，Filter CPU减少约96%。9项设备测试、4项ASan/UBSan及真实PG精确结果、取消回收回归通过。后续继续推进列向量聚合、Scan解码与读取/计算重叠，每项都以查询延迟验收。

## 阶段1优化：选择性过滤后的 numeric 解码

Guest PG 在规划过滤表达式时先确定其所需列，再分配分组/聚合所需列；后者中的 numeric 可以延迟解码。根据现有PG行数估计，仅在过滤后保留不足全表一半行时启用，避免 Q1 这类大部分行通过的查询承担额外遍历。EXPLAIN 显示 `CSD Deferred Columns`，Q6为1、Q1为0。

CSD Scan 在原有列值槽中暂存 numeric 在原始AFDM块中的偏移和长度，CSD Filter 完成选择向量后只转换保留行，并恢复正常列值/显示精度。Guest 为 Filter 增加原始输入AFDM参数；同一块数据保留到sink完成，批次图与NVMe命令数量不变，AFDM分配不变，FEMU/SNIA无需修改。`heap_numeric.h` 共享原有数值解码实现。Filter计时现在包含补解码，应比较Scan+Filter合计CPU。

新增设备差分用例覆盖真实heap布局、两种numeric头、NULL/精度、跨页与批次复用、复合条件及复杂表达式回退；被过滤的不支持数值不会被转换，选中后仍报错。Guest新增延迟聚合、numeric分组键、空结果和OR/NULL过滤的冷启动对照，均与原生PG完全一致；Q1/Q6全结果、取消后同backend重复查询及设备复用通过。10项设备CTest、5项PG设备ASan/UBSan检查通过。性能数据记录于[性能分析](performance.md)。

## 阶段1优化：共用向量表达式与基础算子

PG规划与动态batch的结构保持不变；本阶段不修改FEMU/SNIA调度，不增加读算重叠或可重入协议。当前每轮HYBRID batch有最多4条 `LOAD → Scan → Filter → Aggregate.sink` 分支，四分支结束后PG才复用缓冲并提交下一轮；SF=1为220块、通常55轮。全部输入结束后提交SERIAL `Aggregate.source → Sort（可选）→ Limit（可选）`。CU不固定绑定分支，四份聚合状态跨轮保留。

CSD Filter和Aggregate共用 `query_vector.h`，按256行处理一个表达式，替代逐行递归；保留精确整数、NULL、小数显示精度及每行AND/OR短路错误语义。简单单条件和复合AND比较使用AVX2；复杂过滤使用通用向量表达式。表达式临时列通过workmem分配，聚合跨块复用。SUM/AVG/COUNT同表达式共享累加状态，无分组COUNT(*)按批累加；source按输出类型分别完成计算。

Scan为常见numeric增加直接整数转换；Sort交换行号后统一重排；Limit保持仅调整范围。Guest的块长度相同时不再重复写input_bytes，220块由220次降为4份初始化加1份尾块更新（按代码路径计数）。没有改变NVM读取量、4 CU数量或AFDM缓冲分配。

新增差分测试覆盖完整表达式集合、NULL、短路溢出、选择向量和尾部；新增SQL覆盖计算型过滤、布尔COUNT、SUM/AVG/COUNT共享及重复输出。重复输出用例暴露CustomScan未设置 `ecxt_scantuple` 的既有问题，已在投影前设置当前结果槽，避免PG在非直接投影时异常退出。完整冷启动结果、取消回收、计时开关、11项设备CTest和6项PG设备ASan/UBSan验证通过；性能详见[性能分析](performance.md)。

## 阶段1调度：独立异步执行槽

参考本地DuckDB的Pipeline并行任务及NextParallelScan：PG backend维护唯一输入游标，最多4个执行槽完成后领取下一个范围。每槽包含原有独立AFDM缓冲和局部聚合状态，以及一个SERIAL batch；共享一个SNIA event，通过Context识别完成槽。只重配已完成槽的LOAD请求，再异步提交；没有每四块一次的屏障，也不固定绑定CU。全部输入分配完成且所有请求完成后，原第二条SERIAL pipeline合并/输出结果。

复用csQueueBatchRequest/csPollEvent/csAbortRequest，不改FEMU、SNIA或设备算子ABI，不增加PG辅助线程、进程、代理、双缓冲或预取。SNIA库既有线程仅运行设备请求；PG API始终由backend调用。取消/错误时停止补充任务，尽可能撤销尚未开始的请求，收完所有在途完成事件，释放batch引用后才清理聚合状态及AFDM；已开始请求无法强制撤销，必须等其结束。

EXPLAIN增加CSD Scheduling说明。Guest回归新增450000行多块表，覆盖COUNT、分组、选择性过滤、无匹配和尾块；投影异常numeric触发设备错误时验证其余请求排空后同backend可连续再次完整查询。Q1/Q6精确结果、取消和profile开关验证通过。冷启动对照见[性能分析](performance.md)。

## 阶段1优化：按需解码、引用向量与算术 SIMD

参考本地 DuckDB `execute_reference.cpp`、`execute_constant.cpp`、`execute_conjunction.cpp` 和 `binary_executor.hpp`。Guest PG 继续负责表达式规划，仅扩大现有选择性过滤启发式的适用范围：WHERE 所需的 numeric 也可以延迟解码。CSD Scan 继续输出相同 AFDM 列布局，记录原始 numeric 位置；设备 Filter 按条件判断实际需要的行，过滤结束后为最终存活行物化所有延迟列，再交给 Aggregate。

简单 AND 保留已有已解码列的 SIMD 前缀，再单遍处理通过前缀的行：每行按顺序检查后续列，不通过就跳过该行剩余条件；同一列相邻的上下界只解码一次。这样减少多次遍历选择向量及重复访问原始 heap。复杂 AND/OR 按批次向子表达式传递仍需判断的行，保留 NULL 三值逻辑；共享表达式按行缓存，避免不同分支重复计算。单个表达式如果也被聚合或分组直接引用，会补齐该结果需要的行，不会误用另一个分支只计算了部分行的缓存。

列向量直接引用 AFDM 原列与选择向量，常量仅保存一份，不再逐元素广播或把输入 int64 整列复制成 int128。计算结果才写入工作内存。AVX2 扩展到无 NULL/错误批次的加减、乘法和通用比较，支持列引用、常量和 int64 中间结果。加减检查溢出，乘法使用精确的 int32×int32→int64；不满足条件、NULL、非 AVX2 或需要更宽结果时回退精确 int128。聚合累加器仍为 int128，不以窄整数或浮点近似替代精确聚合。

没有增加设备命令、SNIA 参数、AFDM 字段、调度框架或配置项，同一槽仍无下一块预取。设备测试对照原标量求值器，补充非连续选择、SIMD 尾部、正负 int64 边界和宽结果回退；真实 heap 测试确认被短路排除的 predicate numeric 不解码，实际需要时仍报错。结果与性能验收见[性能分析](performance.md)。

## 阶段2a：外部行排序、TopN与分批结果（完成）

这是面向其余TPC-H的第一个阻塞算子阶段，不代表新增了TPC-H整条查询的覆盖。目前完整卸载仍为Q1/Q6；高基数聚合、Hash Join、字符串、CASE和子查询仍按上述阶段推进。

- Guest PG 从单表行查询生成 `SniaCsdSort`，仍由表达式树驱动，不按Query编号识别。第一条pipeline是多槽 LOAD→Scan→Filter→Sort.sink；第二条为Sort.source归并并处理OFFSET/LIMIT。不支持的结果类型在规划时回退PG。当前不在未支持外层查询的子查询中单独插入CustomScan，避免两个分批source各自持有namespace锁时相互等待；混合计划将随统一查询资源生命周期实现。
- 参考本地DuckDB的 `physical_order.cpp` 和 `physical_top_n.cpp`：局部排序、sink/source阻塞边界、有界TopN堆。设备Sort在原sort.c扩展，保留原小聚合结果快路径；投影后的行按实际列数存储。每段至多1MiB，优先保留workmem，下一段超预算或分配失败时经最后一个AFDM请求NEED_STORE。64KiB spill块属于同一个段，不增加归并段数。
- source保留每段的头部，按小根堆归并，NEED_LOAD按需补充数据；读缓冲可回收重载，因此不会要求所有段的读窗口同时驻留。正常结束主动释放token及私有内存；PG提前关闭或失败时先释放私有上下文，再关闭本查询的SNIA设备回收临时空间。失败batch可能已回滚部分token，不把重复释放失效token作为内存清理的前提。
- 区分256行结果块与原聚合256组上限；Guest保留source batch和设备句柄，逐块取结果。查询内存上下文回调用于错误、取消和游标提前关闭，不把全部最终结果收集到Guest工作表。计时新增Sort.sink/source，统计包含I/O重入调用；计数AFDM位于I/O AFDM之前，包装后算法MR顺序保持不变。
- 首次真实spill暴露了安装权限缺口：SNIA需打开标签femu-spill的分区，原脚本只配置了namespace。install-postgre.sh已补充按标签的udev和当前设备权限；不假定p1/p2角色，也不采用代理。

验收：17项设备CTest通过，最终相关12项重复检查通过；新增外部排序测试以4/8/16/64/128MiB workmem处理约96MiB完整行数据，覆盖四sink、spill重入、归并窗口压力、全部输出顺序/NULL/显示精度、TopN/OFFSET、最大int64 OFFSET、提前退出和失败scope已回收token后的清理；12项PG算子ASan/UBSan及最终6项外部排序容量重复检查通过（环境限制关闭LeakSanitizer）。Guest覆盖普通排序、隐藏排序键、空结果、跨256行输出、大结果spill、游标提前关闭与同backend复用，旧Q1/Q6、取消、计时开关也通过。SF=1 TopN和外部排序各3轮冷启动与原生输出精确一致，数字见[性能分析](performance.md)。

阶段2a同时增加`pg_sina_csd.max_memory`（默认64MB，执行时生效），按执行槽分摊并保留过滤临时空间，以设备共享workmem池为硬上限。排序按下一段是否超预算触发spill，容量降低时缩小run；不是全局空闲比例阈值。后续Hash Join和高基数聚合应沿用同一查询预算，并为自身不可spill状态留出空间，不能各自再申请一份完整上限。当前固定256组聚合尚未改成外部聚合，Join尚未接入。


## 阶段2b：高基数聚合、状态归并与结果排序（完成）

- 所有支持的GROUP BY具备spill路径，删除估计分组数超过256时的规划回退。实际小分组保留原哈希合并快路径；总分组数不再受局部表256组容量限制，不用估计值作为正确性边界。
- 参考DuckDB局部哈希预聚合和部分状态合并：局部表满时flush按键有序的中间状态，source归并sum/count/显示精度，最后完成AVG。复用已有sort.c有界run和SNIA NEED_STORE/LOAD/FREE，没有新增设备接口或运行时框架，也没有照搬radix分区实现。
- GROUP BY sink/source追加I/O AFDM，profile缓冲仍位于最后I/O之前；排序算法编译进aggregate SO内部直接调用，不生成嵌套设备请求。结果需要ORDER BY时在设备内接结果排序，PG只持续提交source和转换Datum。
- 保留查询预算和物理池硬上限，分别限制局部状态、部分状态run及结果排序，使两个排序阶段同时存活时也不超额。连续run保留整块缓存，检测到交错访问造成缓存反复丢失后才缩小窗口，不把缓存策略当作新的用户配置。
- 新增超过32768组的设备差分测试，以及Guest多列/NULL分组、隐藏排序键、4MB预算、实际spill、连续结果、报错与游标提前关闭后的复用；SF=1自定义高基数聚合单独冷启动比较host/CSD。详细数据见performance.md。

阶段2b验收时仍不支持Join、HAVING、MIN/MAX、一般字符串或全部22条TPC-H。中间记录键数加聚合数暂限16，run元数据驻留、外部聚合采用排序部分状态而非radix分区；极多run和极低预算仍需后续多轮归并。Hash Join和所需表达式扩展见下文阶段3a。

## 阶段3a：两表 Hash Join 与完整 Q14

- PG从规划树提取两个输入和一个整数等值连接条件，按全表行数估计选择较小build侧，校验两表映射属于同一namespace。分别生成物理Scan/Filter计划和连接后的列布局；逗号连接与显式INNER JOIN均支持，不匹配SQL文本。兼容PG预处理后已整理成List的条件，以及整数列和不同宽度整数常量的比较。
- 新增设备`join.c`：build仅保存投影后的键/载荷，哈希桶使用重复键链；NULL键不匹配。probe按256行生成连接列批次，直接调用同一SO内的既有聚合sink，最终仍由aggregate source合并。多个probe槽共享只读build状态，切换批次前PG排空所有在途请求，不新增设备调度线程或协议。
- 查询预算一半给build，另一半分摊给probe/聚合槽，受实际设备workmem硬上限约束。build超预算时返回NEED_STORE，SNIA用内部STORE保存64KiB块并返回token；load通过NEED_LOAD恢复一批并建桶。每批重扫probe，聚合状态跨批次保留。驻内存build不发生spill。这是当前有界实现，没有声称实现DuckDB的radix分区或动态重分区；小预算会增加probe内部读取，批次/token元数据仍驻内存。
- 参考本地DuckDB `physical_hash_join.cpp`、`join_hashtable.cpp`的build/probe边界、键/载荷分离和列批输出；参考`expression_executor/execute_case.cpp`的真假selection，实现搜索CASE短路，只处理被选中的分支。Scan可把ASCII常量前缀LIKE提取成布尔列，Q14不存储完整p_type字符串。标量聚合结果支持numeric加减乘除，精确int128、PG显示精度及半舍规则，不用浮点近似。
- 参数仍按既有AFDM顺序构造；共享控制记录只保存状态句柄、批次序号和统计。设备私有哈希表、批次描述和token数组放workmem，输入输出放AFDM。设备不访问Guest文件或直接提交NVMe；错误/取消排空probe后释放两类状态，再由本查询的SNIA deviceClose回收临时token。
- 入口包装增加HashJoin.build/load/probe+Aggregate计时，最后一项包含私有聚合调用。EXPLAIN增加Build Rows/Batches、Join Rows和spill读写，分开显示Build Wall；Internal Read包含所有probe轮次。Guest只编排和转换最终Datum，FEMU和SNIA核心均未修改。

验收：24项设备CTest通过，19项PG算子ASan/UBSan通过。独立Join测试按键统计预期值，核对负键、重复键、NULL、倾斜、空build/probe、3批spill、计时和异常清理；4/64MiB池与64MiB池内4MiB查询预算均核对零残留及实际内存峰值。Guest的`join`入口核对两侧过滤、重复组合、空输入、无匹配、4MiB强制spill、原生回退、build/probe错误及取消后的同backend设备复用。SF=1 Q14在4/64MiB预算下均为一个CSD CustomScan，最终结果逐字一致：`16.3807786263955401`。

阶段3a验收时完整TPC-H覆盖为Q1/Q6/Q14。该阶段Join只支持两个heap表、一个同类型整数等值INNER条件和标量SUM/AVG/COUNT；后续分组能力见阶段3b.1。LIKE仅限C/C.UTF-8下ASCII前缀、末尾一个`%`，同列多个前缀或其他通配符回退PG。numeric仍受现有输入和int128表示范围限制，运行时越界报错。

完整单表query/profile回归也通过，包括新CASE/聚合后运算以及原有高基数聚合、排序、prepared plan、取消/错误和提前关闭。全部检查结束后冷启动交替各3轮：Q14 host=2066.527ms、CSD=636.031ms；Q1为4543.746/861.610ms，Q6为1432.663/584.542ms。Q14 host首轮波动较大，中位数1834.704/648.166ms；完整样本、内存/spill和独立算子计时见[性能分析](performance.md)。

## 阶段3b.1：连接后分组、受限字符串与完整 Q12

- 连接后的GROUP BY复用已有外部聚合，不新增一种聚合器。probe每生成256行列批次，就调用同SO内的聚合sink；聚合返回NEED_STORE/LOAD时，probe把原状态返回SNIA，保留输入行位置、重复键链下一项和待提交列批次。重入先完成该批聚合，再生成新行；新批次清除旧ACK，统计只在首次处理时累加。
- 每个分组probe增加最后一个I/O AFDM，复用已不再使用的heap输入，计时AFDM仍放在它之前。普通标量probe保留原MR顺序。恢复游标保存在build的workmem中，每槽独占；AFDM任务只输入槽编号并输出句柄/统计，不保存私有恢复工作区。聚合source继续分批返回256行结果，PG保留独立build/probe统计，在每次source刷新聚合统计后合并，避免后续页丢失Join行数和build spill字节。
- 所有probe结束后立即释放build哈希表。连接后分组的局部部分状态run使用各槽固定状态之外的余量，最终结果排序使用原build预算。4MB查询预算下减少为2槽，为每槽保留约1MiB固定状态、表达式与spill缓冲。此调整针对实际压力测试：先前将每槽余量再对半保留给尚未开始的结果排序，导致run描述在4MB下分配失败；扩大到150003行probe的独立设备测试可复现，复用已结束生命周期的预算后通过。仍不保证任意多run的元数据都能驻留。
- SQL字符串常量在PG计划内构成小字典，按属性及字节顺序排列。Scan产生整数编号，未知非NULL为0、NULL仍独立；后续沿用8字节列槽、向量比较、CASE和整数分组/排序，不扩大全部数值列。字典随计划保留，spill只保存编号，PG恢复最终字符串Datum。bpchar比较去尾空格并在输出恢复列长度，text/varchar保留尾空格。
- 只有顶层等于/IN能证明列的存活值属于字典，才允许原字符串投影/分组/排序。OR/CASE内的比较不能单独作此证明；未约束列、字符串间比较、范围谓词和一般字符串CASE保留原生路径，防止合并未知值。当前限C/C.UTF-8下ASCII、32项/计划、127字节/项；同列前缀LIKE与字典比较组合未开放。这是SQL常量分类，不是通用字符串字典。
- 参考本地DuckDB `common/types/vector.cpp` 的字典/选择向量组织，以及Hash Join的build/probe阻塞边界和局部聚合组织；针对现有内存和SNIA接口复用机制，没有引入SQL专用runtime、设备格式、辅助进程或SNIA新API。

Q12通过PG表达式树生成两侧Scan/Filter、Hash Join和分组流水线。orders的priority常量比较和lineitem的shipmode IN均使用字典；日期列间比较、CASE、两组SUM和ORDER BY都在设备执行，Guest只恢复最终bpchar Datum。

首轮Q12性能诊断发现按原始表大小选择orders build会分3批，lineitem被重扫3次。补充简单的方向选择：当前build投影估计超预算时，尝试另一方向，按过滤后行数、实际投影列数及当前记录/哈希桶布局估计空间，选择较小者。当前方向已能驻留时保留原选择，避免无条件将大表Scan改为串行build。估计不决定正确性，实际超限仍能spill；EXPLAIN新增Build Relation。已PREPARE的方向可能保留旧预算的选择，执行时仍按当前上限限额。

设备验收：24项CTest和19项ASan/UBSan通过，关闭现有环境不支持的LeakSanitizer。新增/扩大的用例覆盖1471273个重复键连接组合、7001个潜在分组、多build批次与聚合spill同时出现、跨256行输出、加权AVG、CASE/NULL、空输入及计时；4/64MiB池和64MiB池内4MiB查询预算均核对完整结果及零残留。Scan测试补充字典的空串、NULL、未知值、短/长varlena、bpchar尾空格及无效长度。

当前完整TPC-H覆盖扩展到Q1/Q6/Q12/Q14。多表Join、一般字符串、连接后的普通行输出、连接后跨表复合谓词、外/semi/anti Join、HAVING和子查询仍按后续阶段推进，不能将本阶段视为完成全部TPC-H。

Guest完整Join/字符串回归通过，包括4MB高基数分组、source多次输出、早关游标/错误/取消后同backend复用；Q12在16/64MB、Q14在4/64MB下与host逐字一致。原有query/profile回归也通过。最终冷启动各3轮：Q12 host=2022.529ms、CSD=3431.078ms，当前没有加速；Q1为4409.330/949.128ms，Q6为1489.840/758.201ms，Q14为1770.129/687.198ms。Q1/Q6第三轮CSD偏高，完整样本及中位数一并保留。

Q12方向调整将内部读取从约2.98GB降至1.14GB、CSD均值从3819.775ms降至3431.078ms，但build侧Scan/Filter仍为单槽，独立profile的Build Wall约3204ms，成为主要瓶颈。4MB高基数场景另有明显归并读取放大，需要多轮归并/分区。详细样本和资源统计见[性能分析](performance.md)；不将完整卸载等同于已经获得加速。

## 阶段3b.2：并行build扫描（2026-10-22）

上一阶段Q12的Build Wall约3204ms，主要是单槽heap扫描与过滤，HashJoin.build本身仅约8ms。本阶段参考本地DuckDB `pipeline_executor.cpp`的独立执行状态和`physical_hash_join.cpp`的局部输入/合并边界，复用PG已有执行槽：每槽一个SERIAL batch执行LOAD→Scan→Filter，PG通过SNIA完成事件领取已完成列批次，用一个独立的单条目batch执行HashJoin.build，然后立即为该槽补入下一个输入块。没有四块一轮的等待屏障。

本实现仍只有一个共享哈希表写入者，没有照搬DuckDB的每线程局部哈希表。其他槽的扫描/过滤可以在追加或spill时继续运行；同一个槽只有在追加结束后才能复用输入缓冲。单条目build batch继续由SNIA自动处理NEED_STORE重入。build结束后排空所有槽，释放其batch，清零输入长度，再切换为原有并行probe；计时缓冲累计保留。错误或取消沿用现有在途请求排空与设备状态回收，不新增代理、设备接口、内存参数或AFDM状态字段。

`EXPLAIN`增加`CSD Build Scheduling: parallel scan/filter; single hash writer`；README同步说明PG调度与设备计算的边界。该调度属于跨槽并行，没有在一个槽内加入下一块的双缓冲读算重叠。

验收：PG扩展在Guest重新编译安装；完整join回归34项检查通过，包括两侧过滤、重复键、空build、4MB双重spill、结果分页、字符串回退、报错/取消/提前关闭后的同backend复用。另以300ms超时取消完整Q12，随后同backend连续两次CSD COUNT均得到6001215。测试脚本的Join取消时间同步改为300ms，给程序加载留出时间，覆盖并行build阶段。

独立冷启动各3轮，完整输出一致：Q12 host/CSD均值1924.764/1199.416ms，Q14为1646.389/641.160ms。Q12第二轮CSD为1438.838ms，已保留。单独profile确认Build Wall从上一阶段3204.146ms降至834.015ms，内部读取仍1135394816字节且无spill。设备CSF未修改，不重复其上一阶段容量/sanitizer测试；完整数据和限制见[性能分析](performance.md)。本阶段没有增加完整TPC-H Query覆盖，后续仍需扩展多表Join、一般字符串、HAVING及子查询。
