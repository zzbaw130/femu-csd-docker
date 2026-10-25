# 开发阶段与验证记录

> 以下旧阶段记录曾使用扩展目录的自建 SQL，其测试入口和 SQL 副本现已删除；不属于当前验收。当前只认 `tests/guest/queries/`，新规则和结果见文末。

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

## 阶段3c：跨表复合过滤、数值IN与完整Q19（2026-10-22）

- 从两表INNER JOIN中选择一个直接整数列等值条件建哈希表，其余跨表表达式在probe产生的列批次上处理，不再因额外比较或AND/OR而整体回退。单表谓词仍交给各自物理Filter，全部所需列继续由PG投影。
- 参考DuckDB `join_hashtable.cpp` 的候选匹配后选择，以及已有`physical_filter.cpp`/列向量实现：将原filter.c编译进join SO，直接复用比较、SIMD、NULL三值逻辑与AND/OR短路。没有另发设备请求或新增CU；`HashJoin.probe+Aggregate`计时包含此Filter，EXPLAIN标出Filter(join)。
- 连接列值已由物理Filter完成numeric解码，连接后Filter禁用heap偏移解释。聚合spill时保留已经过滤的selection和probe位置，恢复只执行待提交聚合，避免重复过滤与Join Rows计数；Join Rows改为跨表过滤后的行数，原来无额外谓词时含义不变。
- 过滤临时空间按本地与跨表表达式中较大的需求预留，聚合常驻期间仍能执行下一批Filter。小查询预算下带跨表过滤的Join为每槽保留约2MiB，减少并行度，而非提高总预算。4MB可退为1槽；既有无跨表过滤的分组Join规则不变。
- 常量IN扩展到smallint/int/bigint/numeric，单数组最多32项，复用比较与OR表达式以及公共子表达式。参考DuckDB `in_clause_rewriter.cpp` 的小集合展开；没有实现其大集合MARK JOIN。空数组、NOT IN和未支持的数组形式仍按PG原生计划执行。
- Q19使用现有qgen参数及OR分支重复连接键的原SQL，另保留不同参数、公共连接键在OR之外的写法。PG处理表达式树，不识别查询编号。未编码完整字符串上的跨表NULL判断仍不支持，并有显式回退用例；不能据此声称一般字符串已经完成。

首次Q19测量发现一个原有内存策略问题：哈希容量被PG过滤行数估计固定住，64MB预算下仅用少量内存也提前分成2批，lineitem重扫两遍。本阶段补充build动态扩容：估计只决定首次分配，满表后在build上下文限额内逐次加倍记录和桶数组，扩容失败或到达按预算计算的最大容量才spill。桶扩容失败时缩回新增记录空间，保留原容量继续spill；不留下失配数组。统一分配器在复制扩容期间同时计入新旧块，因此瞬时峰值也受硬上限约束，可能比最终布局允许的容量更早spill。新增估计256行、实际70003行的差分测试，64MB时断言不发生build spill；4MB预算仍验证多批与完整结果。

扩容策略参考本地DuckDB `PhysicalHashJoin::Finalize`：按实际总大小和当前内存预约判断是否外部执行，而非用基数估计直接决定spill。本项目采用现有连续记录数组的最小改动，没有引入其临时内存管理器或radix分区框架。

验收：最初24项设备CTest、19项PG算子ASan/UBSan全部通过；补充动态扩容后重新运行24项CTest及3项Join ASan/UBSan，均通过。Guest单表/既有查询55项检查通过，包含数值IN的NULL/显示精度、空数组和NOT IN回退、Q1/Q6、prepared plan及资源清理；最终扩容版完整Join检查45项通过，包含跨表大小比较、附加等值、OR/NULL、4MB聚合spill、Q12/Q14、Q19原SQL及不同参数写法的4/64MB对照，以及报错/取消/早关游标后同backend复用。仍未实现多表Join、一般字符串、HAVING/MIN/MAX/COUNT DISTINCT、semi/anti和子查询；后续优先扩展多表连接与其所需的列/中间结果传递。

最终扩容版各3轮冷启动均值：Q12 host/CSD=1969.966/1065.199ms，Q14=1752.395/604.802ms，Q19=2088.169/1068.240ms，完整输出一致。Q14/Q19 host波动较大，全部样本保留。Q19独立profile确认build从2批变为1批、内部读取从1876811776降至955187200字节，无spill，Scan/Filter从449次降至229次；总计1007.579ms。原始样本、指标及限制见[性能分析](performance.md)。

## 阶段3d：连续多表Hash Join与完整Q3（2026-10-22）

- PG从最多8个base relation的INNER连接图选择连通的probe链。先以原始行数最多的表为起点，再选与当前前缀有直接整数等值键的表；多个候选按过滤后估计行数排序。保留两表方向启发式。Var按每张表的物理属性区间绑定，单表过滤与投影、最终跨表过滤、GROUP BY/排序仍复用原表达式编译，不识别SQL编号。所有输入映射都校验同一设备/namespace/LBA几何。
- 每张表复用原HashJoin.build/load/close，有自己的workmem、批次元数据和控制AFDM。query.h中追加的连接描述仅是本应用计划。当前FEMU adapter的compute AFDM offset非零会返回不支持，故不改SNIA/设备，PG在probe前汇总控制记录中的不透明句柄；不读取哈希记录或中间行。
- 参考DuckDB pipeline对列批次的持续传递和HashJoin的独立build状态，在同一个probe CSF/CU内将每级最多256行的列批次传给下一级，最后再过滤/聚合。每级游标在自己的build workmem，各槽中间列批次在joined AFDM的独立对齐区。聚合NEED_STORE/LOAD沿调用链返回；重入从未完成的子级恢复，父级输入与选择保持不变。没有新增算子运行框架或设备调度API。
- build总预算仍为查询的一半，再按表数均分；设备还以物理池的一半除以表数限制每个build，因此查询GUC高于物理池时也不会各自申请完整份额。估计只决定初始分配，原有有界扩容/spill继续生效。其余预算由probe/聚合执行槽共享。source开始前释放全部build，供结果排序使用。
- PG先完成每张build的首批载入，再遍历各批次的组合。所有probe完成后才替换其中任何一张表的数据；原始probe只在新组合下重扫。Build Batches统计组合数，Build Rows为build原始行数总和，Internal Read包含全部重扫。组合数乘积会造成读放大，均分会使小表留下闲置份额；没有宣称实现DuckDB的动态内存预约、radix分区或任意树形计划。后续需据profile改进这部分。
- 完整Q3使用原有qgen参数：lineitem依次probe orders、customer，再按orderkey/date/priority分组、设备排序和LIMIT 10。Q1/Q3/Q6/Q12/Q14/Q19现已具备完整卸载路径；一般字符串、非INNER连接、HAVING/MIN/MAX/COUNT DISTINCT与子查询仍未完成。

设备差分测试在原join.c测试中增加三表链，键来自前一级build载荷，独立按乘数计算预期值，覆盖重复键、负键、NULL、空末级、跨结果页、最终残余过滤和聚合spill。4MiB池及64MiB池内4MiB预算验证多个build批次组合与零残留；64MiB可驻留。另测试查询预算为物理池两倍，检查总峰值不超查询和物理池。24项CTest、19项PG算子ASan/UBSan通过；最后补物理份额限制后3项Join容量CTest及3项对应sanitizer复测通过。

Guest完整Join回归54项、单表及旧查询55项通过。Q3在16/64MB下与host一致，三表/四表、4MB链式分组、空表及断开连接图回退均通过；最终物理份额版本另测16/64/128MB查询上限与64MiB物理池，结果一致。独立检查第一/后续build报错、链式聚合溢出、Q12/Q3取消后同backend设备复用。

最终各3轮冷启动均值host/CSD：Q3=2610.340/4277.033ms，Q12=2219.740/1084.600ms，Q14=2081.506/632.370ms，Q19=1696.802/1059.619ms。Q3尚未加速；独立profile有6个build批次组合、lineitem累计扫描36007290行、内部读取5772886016字节，主要瓶颈是重复扫描；最终source/排序仅19.332ms。完整样本、均分预算的限制和后续方向见[性能分析](performance.md)。

## 阶段3e：原生 B-tree 索引访问与 Q3 连接顺序（2026-10-22）

- 先核对原生 Q3：work_mem=4MB，两个并行 worker；customer 的共享哈希表约1472KiB、一个batch，聚合前各排序约1.1MiB，最终Top10为26KiB，没有临时文件I/O。其路径是customer过滤后连接orders，再参数化查找lineitem_pkey。之前CSD六次扫描lineitem的原因包括缺少索引路径、连接方向和有界批次策略，不能仅归因于64MB内存不足。
- 参考本地DuckDB `physical_table_scan.cpp` 的投影及独立扫描状态、`join_order_optimizer.cpp` 的统计/访问路径选择；PG索引格式以本仓库PG12 `nbtsearch.c`、`nbtree.h`、`itup.h`为准，不套用DuckDB的ART格式。规划端复用PG已选成本最低路径中的连接方向和参数化IndexPath，当前只接受左深Hash前缀加链尾一个整数等值Index Join，不按Q3编号匹配SQL，不另造成本模型。不支持的访问形状继续原有Hash路径。
- 扩展版本0.2新增`sina_csd.indexes`，0.1通过ALTER EXTENSION升级。prepare(heap)在同一次checkpoint后为可用的原生索引保存extent和根页等元数据，复用原relations/extents及FIEMAP实现，没有复制键值、构建影子索引或每查询重新准备。仅支持PG12 B-tree v4、内置整数操作符族、升序NULLS LAST前导整数键；可处理复合索引、NULL、重复键跨叶页和VACUUM后的HOT根重定向。REINDEX造成物理文件变化时不选旧索引映射，重新prepare(heap)后恢复。
- 设备`index_scan.c`与原join/scan/filter/aggregate一起链接到join SO；与全表Scan共用Tuple解码器。每批最多64个外侧键执行B-tree下探，按TID仅解码命中Heap Tuple，右表局部过滤后进入既有跨表过滤和聚合。没有把B-tree比较、Heap解码或中间Tuple传回Guest。
- 复用各槽的输入AFDM作为页输入，spill区之后最多128个8KiB页槽；物理Scan复用输入时失效，不跨查询缓存。游标、页标签和键状态受workmem限额，每索引状态预留128KiB，剩余build份额由哈希表使用；物理上限按相同角色划分。没有把AFDM用作哈希表或私有运行时空间。
- 缺页时CSF把文件类别、页号和目标槽写入任务AFDM，PG只转换地址并提交原有SNIA内部LOAD，再执行仅包含Join的继续batch；合并相邻目标槽和连续物理范围。应用私有QS_INDEX_WAIT只在SO内部传播，CSF对SNIA仍正常完成，PG检查任务记录继续；没有扩展CS_STATUS、FEMU runtime或SNIA库。聚合NEED_STORE/LOAD沿既有重入路径处理，父级Hash游标保留未完成的子级批次。
- Q3改为customer build、orders顺序Scan/Filter→HashJoin→lineitem索引连接→分组/排序/Top10。EXPLAIN新增实际扫描表、索引、页加载次数和索引Heap解码数；内部读字节同时包含顺序扫描和索引页/Heap页。Index Join计时包含在现有probe+Aggregate入口内，不能把其余墙钟全部称作设备I/O。

本阶段没有实现一般常量范围Index Scan、Bitmap Heap Scan、Index Only Scan的设备实现、多处Index Join或通用分区Hash Join。原生IndexOnlyScan只用于选择候选路径，设备仍查Heap；非索引路径的build批次组合仍可能重扫probe。Q3通过小表build加索引访问消除了该实例的六轮重扫，不能据此声称已解决全部Join spill场景。

验收：25项设备CTest、Guest页布局及相关ASan/UBSan检查通过。原生索引测试覆盖不同键宽、跨叶重复值、513行输出、超过页槽容量的256个Heap页、HOT、空根、错误/取消回收；Guest索引专项13项、既有Join54项、单表55项检查通过。Q3在4/16/64/128MB预算下结果一致，索引计算溢出或取消后同backend能连续执行两个CSD COUNT。Guest已升级0.2并为八张冻结表保存索引映射。

全部检查结束后独立冷启动各3轮：Q3 host/CSD均值2467.175/1868.293ms，约1.32倍，CSD较历史六轮重扫均值4277.033ms降低约56%。Q12/Q14/Q19分别为2004.954/1092.405、1640.833/654.222、1769.881/1079.401ms。Q3独立profile中顺序扫描1500000行、索引Heap解码589580行，内部读取1095892992字节，7733次probe入口，build组合数1且无spill；数据和剩余继续执行开销见[性能分析](performance.md)。

## 阶段3f：低基数运行期字符串字典与完整 Q5（2026-10-22）

- 新增查询级运行期字符串字典 AFDM（`struct qs_dict`，最多512项、64KB字节区），仅承接低基数、可安全内联的字符串：Scan把去尾空格后的字节插入字典并输出ID，Join/分组/排序只传ID；Aggregate.source在最终结果前把ID恢复成字节。结果仍放在原有16字节 `qs_cell` 中，字节写入 `value`、长度写入 `scale`，上限15字节。Q5的 `n_name` 最长14字节（char(25)去尾空格），因此不需要改 `qs_cell`、Hash Join stride、Sort run 或 spill 记录布局。
- 规划端允许 `Q_STRING` 列作为分组键/输出；SQL 常量等于/IN 仍沿用原静态字典，不改成运行期字典。字典与静态字典不混用。字符串 `ORDER BY` 暂不做：运行期ID按首次出现分配，不是字节序；规划时对 `ORDER BY` 字符串直接回退原生路径，避免输出错误顺序。Q5按 revenue 排序，不受此限制。
- 多表连接顺序改为优先选择“单列唯一键”候选，并在多个唯一键候选中优先较大的过滤后关系。此前Q5按最小估计行数先连 customer 与 supplier 的 `c_nationkey=s_nationkey`，会先生成约 customer×同国supplier 的大中间结果，再连orders，实测超过42s；调整后链为 lineitem→orders→customer→supplier→nation→region，`c_nationkey=s_nationkey` 留作最终残差，避免该放大。
- build预算不再按表数均分：按每表过滤后行数、投影列数、按2的幂容量计算的“单批所需字节 + 1MiB”比例分配，设备仍受查询预算一半和物理池硬上限约束。Q5在默认64MB查询预算下五张build表均为1批，probe只扫描lineitem一次；16MB查询预算下仍正确，但发生内存受限的多批重扫。
- 未实现：原生计划中间位置的 Index Join（Q5仍走Hash链）；运行期字典>15字节或>512项；字符串排序比较器；FD输出payload；Q10的 c_name/c_address/c_comment 长字符串。Q5的性能来自一次lineitem扫描、4 CU并行probe和单批build，不代表已实现通用path编译器。

验收：Q5完整结果与host逐字一致（3轮cmp均通过）；默认64MB下五张build表各1批、无build组合重扫；16MB查询预算下结果仍一致，profile显示12个build批次、spill写17.4MB/读39.8MB，属内存受限。Q5默认64MB冷启动三轮host=2460.080/1926.239/1939.466ms（均值2108.595），CSD=989.475/929.035/919.904ms（均值946.138），约2.23倍；16MB查询预算CSD=6440.564ms。Q5取消（200ms statement_timeout）后同一backend连续两次CSD COUNT均返回6001215。既有Q1/Q6/Q12/Q14/Q19冷启动结果无回退，Q3单次冷启动1869.695ms。复现入口为`tests/pg-sina-csd-test.sh join`和`join-bench`，Q5 SQL位于`tests/q5.sql`。

## 阶段3g：FD分组键、长字符串结果与完整 Q10（2026-10-22）

- 运行期字典扩成大小两档：Q5 512项/64KB；Q10 1,048,576项/20MB。字典头后依次是开放寻址bucket、entry和字节区，bucket由Guest按64KiB拷贝清零，避免大块FDM fill。大字典实际分配约33.5MB。
- 规划端把PG因函数依赖从`groupClause`去掉的非聚合输出列重新加入分组键。Q10的7个SQL分组列因此仍作为7个整数ID键进入设备，不需要变长FD payload记录；Q10字典在Scan阶段插入customer全部150k行字符串，约19.6MB，未超过20MB区。
- 最终结果增加64KiB字符串arena：不超过15字节仍内联在16字节cell；更长的c_address/c_comment等写arena，cell保存offset/len。Aggregate.source只在最终结果块离开设备前解析字典ID，partial state、sort run和spill仍只保存定长ID。
- FDM容量循环在字典较大时先缩小每槽4MiB输入块到1MiB，再减少worker数；Q10保持4 CU、bucket清零和字典分配成功。Q10 profile为4 workers、3张build各1批、无spill，EXPLAIN的CSD AFDM为32,894,976字节（该指标不含完整字典区，字典约33.5MB），workmem峰值45,794,960字节。

验收：Q10完整结果与host逐字一致（3轮cmp均通过）。默认64MB冷启动三轮host=3050.724/2028.636/2060.642ms（均值2380.000），CSD=1356.773/1388.854/1429.740ms（均值1391.789），约1.71倍；Q10独立profile 1456.291ms，Hash Join 114028行，Sort 17个run且无spill。16MB查询预算下CSD=7433.795ms，结果一致，profile显示12个build批次、54个sort run、33个spill run，写23,789,568/读65,208,320字节，workmem峰值14,237,584字节。Q10取消（200ms statement_timeout）后同一backend连续两次CSD COUNT均返回6001215。既有Q1/Q5/Q6/Q12/Q14/Q19单轮冷启动结果全部一致。

当前限制：字符串`ORDER BY`仍回退原生；大字典只在Query级、不支持spill，Q10的20MB区依赖customer全量字符串约19.6MB，超过即报设备错误；尚未实现FD payload/可spill字符串。复现入口`tests/pg-sina-csd-test.sh join|join-bench`，SQL位于`tests/q10.sql`。

## 阶段3h：复合连接键与 Q9（2026-10-22）

- 支持第二组整数等值键：`qs_join.build_key2/probe_key2` 同时参与 build/probe 哈希和相等判断。Q9 的 `partsupp` 按 `(ps_partkey,ps_suppkey)=(l_partkey,l_suppkey)` 连接，避免单键连接后再用残差过滤造成的四倍中间行。
- 新增 `Q_YEAR`：识别 PG12 `date_part('year', date::timestamp)`，设备把 date 转为年份整数，结果按 float8 返回；新增 `Q_PATTERN` Scan glob，支持 `%floral%` 这类中间通配；短字符串结果排序改为 inline 字节序比较。
- 连接顺序改为“单列唯一键优先，唯一键候选中过滤后较小关系优先”；build预算增加按目标批次数分配，Q9 的 build组合从42批降到6批。
- 尝试过 orders_pkey 作为链尾索引：复合键后 Index Heap Tuples 从25.9M降到323844，但随机页加载62849次，耗时10.9s，比Hash慢，已撤回。

验收：Q9 175行结果与host逐字一致（3轮cmp通过）。默认64MB冷启动三轮host=4161.373/3068.764/3073.949ms（均值3434.695），CSD=7702.432/7386.411/7677.284ms（均值7588.709），约2.21倍。Q9 profile为6个build批次、Internal Read 5921865728字节、Spill Write 98631680/Read 158793728字节、4 workers、Execution 8073.135ms。32MB查询预算CSD=42491.859ms，结果一致；16MB超过55s未返回，当前最低验收内存为32MB，推荐64MB。200ms取消后同backend连续两次CSD COUNT均返回6001215。

限制：Q9仍有6次lineitem重扫，64MB下2.21倍host；16MB不可用。问题来自orders 1.5M与partsupp 800k无法同时单批驻留，下一步需要分区Hash Join或probe物化，不能把当前结果当作性能完成。复现入口`tests/pg-sina-csd-test.sh join|join-bench`，SQL位于`tests/q9.sql`。

Q9提交后复核Q5/Q10（连接顺序/预算已变化）：Q5 host均值2275.099ms、CSD均值1212.940ms，约1.88倍；Q10 host均值2287.015ms、CSD均值1531.757ms，约1.49倍；各3轮结果cmp通过。16MB查询预算Q5=11102.431ms、Q10=5062.922ms，结果一致；旧Q5/Q10的64MB/16MB记录以本次复核为准。

## 阶段3i：重复 RTE 自连接与完整 Q7（2026-10-22）

- Q7 的 `nation n1`/`nation n2` 是同一基表的两个 RangeTblEntry。规划端按 RTI 绑定列区间，`greedy_order` 允许同一 relid 的两个实例分别进入连接链；Q7 生成 supplier、nation n1、orders、customer、nation n2 五级 build链。
- OR 中的字符串等值（n1/n2 名称）在 `string_comparison` 中标记为等值受限；若运行时出现字典外字符串，输出ID0会在结果转换时报错，不会静默合并。Q7 只输出 KENYA/JORDAN 两值，静态字典覆盖。
- 字符串 `ORDER BY` 使用短字符串 inline 字节序；Q7 的 `supp_nation,cust_nation,l_year` 排序在设备完成。

验收：Q7 4行结果与host逐字一致（3轮cmp通过）。默认64MB冷启动三轮host=3251.633/2329.948/2404.706ms（均值2662.096），CSD=3676.067/3691.620/3619.526ms（均值3662.404），约1.38倍。Q7 profile：Build Batches=4、Internal Read 3931471872字节、Spill Write 66191360/Read 126287872字节、4 workers、Execution 3590.978ms。16MB查询预算CSD=8348.123ms，结果一致。200ms取消后同backend连续两次CSD COUNT均返回6001215。复现入口`tests/pg-sina-csd-test.sh join|join-bench`，SQL位于`tests/q7.sql`。

## 阶段3j：Q8 8表/7join 与聚合比值（2026-10-22）

- Q8 的 8 张表合计60列，设备计划列上限64，`describe`/join绑定支持满7级build；nation n1/n2 两个RTE按RTI独立绑定。
- `p->nkeys` 非空时，输出若包含聚合表达式（`contain_aggs_of_level`），走 `final_expression` 生成分组后的标量树；Q8 的 `sum(CASE...)/sum(volume)` 编译为 `Q_DIV(Q_AGGREF,Q_AGGREF)`，不再要求输出必须是裸Aggref或分组键。
- CASE 条件 `nation='JORDAN'` 使用静态字典；p_type/r_name 过滤同路径。

验收：Q8 2行结果与host逐字一致（3轮cmp通过）。默认64MB冷启动三轮host=2951.971/2393.437/2259.607ms（均值2535.005），CSD=846.824/859.838/901.284ms（均值869.315），CSD约快2.9倍。Q8 profile：Build Rows=1860055、Build Batches=1、Internal Read 1200168960字节、Spill 0、4 workers、Execution 807.220ms。16MB查询预算CSD=11484.375ms，结果一致。200ms取消后同backend连续两次CSD COUNT均返回6001215。复现入口同上，SQL位于`tests/q8.sql`。

## 阶段3k：Q4 EXISTS 半连接与索引 tail（2026-10-22）

- `join_inputs` 接受规划器生成的 `JOIN_SEMI` JoinExpr；`compile_semi_index` 以 orders 为 probe、lineitem 为 build 描述，保留 `l_orderkey=o_orderkey` 键，并把 lineitem-only 的 `l_commitdate<l_receiptdate` 从混合残差移到索引谓词。
- `qs_join` 增加 `semi`；index probe 在 B-tree heap tuple 上先执行谓词，每个 probe 行最多输出一次。`lineitem_pkey` 作为链尾索引，不再需要构建6M行哈希。
- 小结果快速路径此前在 `Aggregate.source` 中先按字典ID排序、后解析字符串，Q4 只有5组会走该路径；现在排序前调用 `resolve_output_strings`，保证字符串 ORDER BY 的字节序。

验收：Q4 5行结果与host逐字一致（3轮cmp通过）。默认64MB冷启动三轮host=2608.291/1945.221/2094.175ms（均值2215.896），CSD=1312.971/1202.337/1332.642ms（均值1282.650），CSD约快1.7倍。Q4 profile：Internal Read 727531520字节、Index/Heap Pages Loaded 62715、Index Heap Tuples 52544、Spill 0、4 workers、Execution 1183.481ms。16MB查询预算CSD=1237.619ms，结果一致。200ms取消后同backend连续两次CSD COUNT均返回6001215。复现入口同上，SQL位于`tests/q4.sql`。

## 阶段3l：Q13 LEFT JOIN 与两级聚合（2026-10-22）

- LEFT JOIN 采用“每 probe 行先输出一条 build 列为 NULL 的垫底行”方案；Q13 的 `count(o_orderkey)` 忽略该 NULL，零订单 customer 自然得到 c_count=0，避免 unmatched 状态机。
- `Aggregate.source` 增加 nested 模式：第一级 group 完成后不再直接输出，而是按第一级 count 累加 c_count 分布；最终输出 c_count/custdist，并在设备内排序。
- 小库 q13_test 与 10 万客户/100 万订单场景验证通过后，再接 TPCH 主库。

验收：Q13 41行结果与host逐字一致（3轮cmp通过）。64MB冷启动三轮host=1312.491/1134.331/1150.842ms（均值1199.221），CSD=5631.952/5418.754/5892.680ms（均值5647.795），约4.7倍。同规模合成表（150k customer/1.5M orders）在32MB查询预算完成（11941.874ms）；16MB查询预算长时间不返回，当前最低验收内存为32MB；性能和16MB小内存仍未达标。Q13 profile未在崩溃前采集。SQL位于`tests/q13.sql`。

## 阶段3m（部分）：Q18 IN 消解为 HAVING（2026-10-22）

- Q18 的 IN 子查询对 lineitem 按 orderkey 求和 `>300`；外层主查询同样按 orderkey 汇总同一批 lineitem，因此 IN 等价于主聚合后的 `HAVING sum(l_quantity)>300`。
- 设备计划增加 post-aggregate `having` 表达式；`finalize_group` 在输出前用 Q_AGGREF 求值，不满足的组直接跳过。
- Q18 专用 planner 移除 SubLink、编译正常三表聚合，并附加 Q_GT(Q_AGGREF,300)。
- 验证：10k customer/20k orders/60k lineitem 测试表上 host/CSD 结果逐字一致，LIMIT 100 通过。
- 补充：SF1 规模三表装载完成后，host Q18 6M 行数据集可完成（104 行输出）；后续单轮实测：host 6M Q18 14905.102ms、CSD 15088.137ms，100行结果 cmp 通过。3轮冷启动脚本再次导致 guest 失去响应；16MB 单轮超过60s无返回，最低验收内存为64MB；Q18 暂不能标记完成。

## 阶段3n（基础设施）：planner 前子查询 decorrelation（2026-10-22）

- 新增 `planner_hook` 包装：CSD 模式下先复制 Query，在 `standard_planner` 之前运行 `pg_sina_rewrite_query()`。
- 首个形态：WHERE 中等值相关、单聚合的 `EXPR_SUBLINK`（Q17/Q22 类）。
  - 原 SubLink 子查询改写成 `GROUP BY correlation_key` 的 derived table；
  - 外层 WHERE 中的 SubLink 替换为该 derived table 的值列；
  - 增加外层 correlation key 与 derived key 的等值 join qual。
- Q17 在 `exe_path=csd` 下的 EXPLAIN 已从 `Join Filter: (... SubPlan 1)` 变为：
  `Hash Join` + 右侧 `Finalize GroupAggregate Group Key: lineitem_1.l_partkey`，不再有 SubPlan。
- 该阶段只完成 PG planner 前的重写，设备尚未实现 grouped derived table 的 materialize/build 算子，因此 Q17 设备路径仍不能完整 offload；不能把 Q17 加入已支持列表。

重写等价性验证：同一 Q17 的手写 grouped join 与 CSD planner 重写后的查询结果一致（348406.054285714286），说明当前 rewrite 不改变结果。

## 2026-10-23：基于 PG 原生 Plan 的设备执行

本节取代前面阶段中关于贪心 Join 顺序、链尾单个 Index Join、Q13/Q18 专用编排和“仅去关联但不能卸载”的当前状态描述；前文保留为开发历史。当前接口和限制以 README 为准。

- `src/planner/plan.c` 接管 `UPPERREL_FINAL` 已选中的原生 Path，在 `PlanCustomPath` 收到 PG 生成的 Plan、尚未执行 setrefs 时转换。保留左右输入和连接树，不从表集合重新挑选连接顺序。转换失败的规划包装在 setrefs 后去除，原生 InitPlan 和参数依赖保留。
- 参考本地 DuckDB 的 unused-column pruning，从父输出、过滤、连接键和排序/分组键向下推导投影需求，同步重映射位置引用。不能直接照搬 PG Scan 暴露全物理 Tuple 的 targetlist，否则会无意义地解码大字符串，甚至耗尽设备字典。
- `src/planner/rewrite.c` 只在可证明的严格 WHERE 场景，将 NULL-on-empty 标量聚合改成按整数关联键分组的派生表；支持多个关联键和多次改写。COUNT 空输入、OR 和不覆盖的形态不强行去关联。Query 按副本尝试并原子采用，避免失败后留下半改写。
- Guest 的 `src/executor/plan.c` 用一个直接 SNIA 会话执行 `pg-sina-plan.so`；按已有 extent 提交 Heap/索引内部 LOAD，只转换最终结果。私有中间行、连接组、聚合状态、CTE 和标量参数均留在设备。没有新增进程、代理、IPC 或 FEMU SQL runtime。
- 设备 `plan.c` 组合 Scan、Projection、Hash/Merge/Index Join、Aggregate、Sort/Limit、CTE 和 InitPlan。连接覆盖 INNER/LEFT/SEMI/ANTI；聚合增加 MIN/MAX、COUNT DISTINCT、HAVING 和分组结果作为上层输入。多个位置的参数化 Index Join 复用 PG12 B-tree 实现；复合索引的额外条件在设备判断。
- Hash 超限时保存已消费的投影行，再对该前缀和剩余输入归并，不按 build 批次重新读大表。排序使用有界 fan-in 的多轮归并；归并缓存不足时优先 spill 驻留 run，修复读放大。workmem 新增父子 context 记账，子排序和索引共同受查询总额度约束。
- 修复旧聚合结果满页时重复输出边界组，以及小结果合并释放 context 后再读取字符串状态的问题。短字符串排序标记由合法 numeric 精度 3 改为 UINT8_MAX，避免 Q3/Q10 收入排序错误。
- 通用树默认 1 MiB 扫描块，列批次通过现有 decoder 生成；树内中间值保留 int128。当前通用路径为单 CU、按行组合，Q1/Q6 等单表列向量快路径仍保留最多四槽调度。不能把新功能覆盖当成已经继承全部向量化、并行和性能优化。

验证入口：`tests/pg-sina-csd-test.sh plan`、`plan-bench`；后者逐条检查 CSD CustomScan，再冷启动核对原始 SQL 的结果。Q17/Q20 的原生 host 120 秒超时时，使用独立参考 SQL 作为正确性 oracle，保留超时而不替换原生 host 延迟。设备 CTest 覆盖 NULL/重复键/连接类型、精度、Hash 超限、乱序多 run、嵌套排序聚合、4–128 MiB 排序预算和回收；原生树及排序另用 ASan/UBSan 检查。

最终验证：26 项设备 CTest、1 项本地 PG 页布局 CTest、22 项 Guest 原生计划回归及取消后同 backend 复用全部通过。Q1–Q22 均实际以 CSD CustomScan 执行并核对完整结果；Q17/Q20 的独立参考核对与原始 host 超时分别记录。原生树/外部排序 ASan+UBSan 通过（当前环境关闭 LeakSanitizer），最终计数变更后又复验原生树。Guest 结果目录分别为 `/tmp/pg-sina-plans.It2DI2`、`/tmp/pg-sina-plan-bench.8WwC10`。性能退化及剩余工作已明确记录在 performance.md，未宣称 Join 顺序调整已经带来整体加速。

## 2026-10-23：收敛唯一 TPC-H 验收入口

- 删除 `tests/guest/queries` 中17条非标准 LIMIT 1及对应explain的限制，共34个文件；Q2/Q3/Q10/Q18/Q21原样保留。参数、子查询及Q15的视图创建/删除流程不替换。
- 删除扩展测试目录的SQL/TSV fixture、自建TPC-H及reference副本，保留heap_layout.c；删除四个旧Guest测试脚本、timed.sql及其临时bench/统计脚本。CSD/SNIA/workmem的C/C++单元测试保留。
- 唯一入口为 `tests/guest/tpch-test.sh`；主机 `tests/pg-sina-csd-test.sh tpch` 直接调用它。旧命令入口删除，活动README依赖同步更新。
- AGENTS.md明确唯一SQL来源及性能门槛：只有实际内存不足导致必须频繁spill的较高延迟可以接受，实现不完整/不够优化造成的退化不合格。
- 当前扩展使用统一22条SQL重测：完整结果及实际卸载全部通过，无超时或参考SQL；Q1/Q6较快，20条更慢，按新规则性能未达标，脚本返回1。较慢项另行采集内存/spill/CPU，不能以结果一致或发生spill直接宣布合格。详见performance.md最后一节和 `results/tpch.sCmJHo/`。

## 2026-10-23：通用 Plan 多 CU 与无 spill 慢查询优化

本节更新前述“通用树单 CU”的状态，保持 PG 选定的连接树、标准 SQL、设备/PG 资源配置及冷启动规则不变。

- Guest `src/executor/plan.c` 标记 Scan/Projection/Hash Probe/Index Probe 流水线，以现有 SNIA event/batch 推进至多四槽。设备 `plan.c` 动态领取块，Hash build 共享一次；阻塞节点消费紧密投影的内部批次，PG 不接收中间行。不同阶段复用缓冲，扫描块按 FDM 容量选 4/2/1 MiB，小表按实际大小分配。
- 复用原 Filter 的 SIMD/selection/复杂条件短路与延迟 numeric，直接列投影省去解释器；已有字典值无锁查找、新增值仍加锁。动态状态和 Filter scratch 均计入同一查询根 workmem，放不下时回退原算法，不绕过总内存限制。
- 索引页合并批量异步 LOAD→继续计算；SEMI/ANTI 匹配后停止查找。自连接借用当前保留的扫描块，块外仍使用原索引缓存。单整数/date run 新增有预算约束的 radix sort，其他类型保持原精确比较。
- 修复 spill 重入丢失已消费缓冲标记、嵌套 Hash fallback 复用未消费完 AFDM 两个并发正确性问题；更深的 build 依赖优先完成。没有增加 FEMU/SNIA 协议、SQL runtime、配置开关或辅助进程。
- 现有 profile 开关增加 Scan/Filter/Sort 调用边界计时、实际同时计算的 CU 峰值和 LOAD 请求准备统计；不逐行读时钟，累计分项不与总时间重复相加。

验证：26 项设备 CTest、1 项 PG heap CTest 通过；原生 Plan 的 ASan+UBSan 通过（关闭 LeakSanitizer）。原生 Plan 测试覆盖四任务并发、Hash/Index 的 INNER/LEFT/SEMI/ANTI、多阶段 build、零列 COUNT(*)、并行后 spill、嵌套 fallback、4 MiB 输入块、负整数排序、并发字典插入以及 context 完整回收。spill 缓冲消费标记及嵌套 fallback 的新用例可使旧实现失败，修复后通过。

真实 Guest：唯一入口 `tpch-test.sh` 完整 22 条结果一致并实际卸载；对 Q3/Q11/Q15/Q21 又各做两轮冷启动，所有结果一致。11 条无 spill 查询的独立 profile 都观察到 CU 并发峰值 4；没有改变 64 MiB 总 workmem。取消测试用原始 Q12 在 250ms 超时，再在同一个 backend 完成原始 Q12、Q7，完整结果与 host 一致。日志在 `results/tpch.qf481C/`、`tpch.8RuIV4/`、`tpch.PFtKzS/`。

性能：完整轮 13 条未更慢、9 条仍未达标。Q3/Q11/Q21 的重复测量接近持平，个别样本略慢，不能宣称稳定加速；Q15 三轮中位数 host 2.263s / CSD 1.494s。其余 9 条的 spill 尚不能归因于物理 64 MiB 内存耗尽，节点配额和算法成本仍需继续优化。具体数据及口径见 performance.md 最后一节，结果正确和性能验收分别报告。

## 当前改造目标与三层边界（2026-10-24）

目标是使用唯一验收目录中的全部 22 条 SQL，在 8 CU、256 MiB workmem 下实现完整卸载与全面加速。结果正确、实际卸载和性能合格分别验证；尚未贯通的列接口或未分析的慢样本不能视作完成。

- PG 扩展：采用 PG 选定的 Plan/Join 顺序，完成 SQL rewrite、物理列与 extent 准备、pipeline 编排和资源容量选择。已通过 SNIA 读取 CU/FDM 容量；workmem 通过普通 CSF 查询分配器容量。FDM 配置变化后新连接重新发现，无需硬编码扩展。
- SNIA/FEMU adapter：把标准 batch 翻译成通用 EXEC_GRAPH，处理请求、事件和会话生命周期；不解析 SQL、Heap 或 Join 顺序。
- FEMU CSD 固件：只管理通用 LOAD/STORE/COMPUTE、AFDM、CU、依赖就绪与完成时间队列，以及隐藏临时空间/通用重入。SQL 算法仅存在于设备程序 `csd-programs/pg_sina_csd`，不放进固件。

本轮推进了字符串列批次过滤和精确参数引用。`query_vector.h` 中字符串向量借用原始 LOAD 或不可变字典中的字节，SUBSTRING 产生切片而不是立即插入字典；结果需要保留时才物化。Scan/Filter 私有计划中的 `deferred` 区分普通延迟列与仅用于过滤的列，后者不离开 Scan 的输出。通用 plan 程序直接向 Filter 实现传递原始输入、字典和参数数组，不增加 NVMe/SNIA 协议或主机控制标志。

参数引用保留 128 位数值与实际 scale。对过滤计划的私有副本更新相关表达式 scale，SIMD 不能安全表示参数时使用精确的宽整数比较。原始 PG 计划不变。UTF-8、NULL、负/零起点、负长度错误、LIKE 转义、CASE 未执行分支与投影字符串生命周期都有针对性测试。

继续改造时仍需逐项完成：Join/分组聚合的列批次接口、局部/共享状态与 Combine/Finalize 覆盖、跨块持续执行、实际总内存预算与 spill 配额。当前保持 64 MiB FDM；仅在测量证明它约束吞吐后才尝试扩容，不以加大 FDM 掩盖执行器问题。最新性能结论以 performance.md 和其关联的规范 SQL 原始日志为准。

## 全量分组索引扫描优化（2026-10-24）

参考 DuckDB Hash Aggregate，将不要求结果顺序的全量非覆盖索引分组路径改为并行 Heap 扫描和有界哈希聚合，不改 PG 的 Join 顺序，不移除选择性索引或覆盖索引。私有节点记录增加 HAVING 前的分组数估计，避免用过滤后的小结果数估算聚合表；父节点仍使用原输出行数。

整数分组表按查询实际剩余预算扩容，重哈希保留原键与 NULL 语义；预算不足继续 spill。驻留和 spill 归并路径均跳过未通过 HAVING 的分组，直到下一条有效结果或真正输入结束。相关8项设备CTest、Guest布局测试及规范 Q13/Q17/Q18 冷启动通过；Q18两轮 CSD 为2067.959/2260.278ms。详细证据见 performance.md。此阶段未修改 FEMU/SNIA 协议，也未扩容FDM。

## 分组聚合列输入（2026-10-24）

沿用 DuckDB 将分组键和聚合参数作为列批次输入的方式，纯 Scan/Projection 输入的共享哈希分组直接消费现有表达式向量。列批次保留独立游标；共享表满时才还原未接收的行，交给已有协调端扩容/spill路径，不改变总预算和返回协议。Join 等输入仍保留行适配，后续继续改造。

多键测试使用低估的初始分组数，覆盖中途表满接续、精确小数和NULL；8/16/64槽及4MiB预算用例通过。规范Q13/Q17/Q18结果一致且实际卸载。Q18平均活跃CU由前一profile的2.74变为本次4.20，整查询普通延迟仍有波动，不把该值解释成全面8CU满载；详见performance.md。

## 分组探测与局部合并（2026-10-24）

共享分组表在一个 CU 批次内不扩容也不删除键。参照 DuckDB 将键查找和聚合状态更新分开的方式，已发布的键只读比较，碰撞桶不再加锁；仅新分组发布和匹配分组更新需要原子保护。首次发布使用 release/acquire，同一桶正在创建时重试，不读取尚未写完的键。协调端仍在 CU 批次之间扩容或 spill。

列批次内连续相同键先合并为一个局部 SUM/count 状态，再更新共享表，不分配每 CU 完整哈希表。先检查至多16行的相邻键；没有发现重复时直接更新，避免分散键输入逐行付出比较成本。局部状态保留 NULL、numeric scale/dscale 和 AVG 的 sum/count；全局表满时仍按原输入行恢复，继续原重入路径。

现有多键测试分别使用连续整数键和分散字符串键，覆盖低估基数下的扩容、4MiB预算下spill、NULL、SUM/AVG/COUNT及资源回收。原生及8/16/64槽测试通过；并行测试重复检查也通过。业务测试保持规范SQL、冷启动和原容量，性能记录见performance.md。

表达式向量导入时，从重绑定的子表达式推导 ADD/SUB/MUL/CASE 的实际 scale，不使用 PG 通用 numeric 元数据替代运行时 scale。多键回归测试刻意令声明 scale 与实际输入不同，检查 SUM/AVG 与后续比较仍保持精确；该修复覆盖 Q15 初始化最大值与后续收入聚合的比较。

## Hash Join借用列输入（2026-10-24）

参照 DuckDB 的探测结构，将纯 Scan/Projection 的列批次保留到匹配链消费完毕。先批量计算哈希，再独立读取桶头；仅将Join键还原到当前输入，匹配后按列掩码补齐payload，不重复复制键。无匹配的INNER/SEMI和无额外Join条件的已匹配ANTI省去payload复制。匹配链位置与输入向量位置分别保存，多批输出不会重新领取输入。

探测工作区按表达式批次行数分配并计入查询workmem（当前256行，与Heap解码窗口分开），无法分配则保留原路径。分区spill和不支持的列输入也保留原适配；Join输出仍是行接口，后续继续改造。批量预计算只记录表达式错误，消费到该行才报错；LIMIT未消费的后续行不能提前报错，已消费但未匹配的行也不能丢失其输入错误。新增测试同时验证128位payload和这些错误时机。

4项设备CTest及规范Q10/Q14/Q17/Q19冷启动回归通过。没有重复完整22条验收；全量基线仍为e1e925ae2。不同样本的墙钟和请求驻留波动明显，不能宣称本阶段稳定加速，详细证据见performance.md。未扩容FDM或修改FEMU/SNIA协议。

## Hash Join列输出与阻塞依赖预算（2026-10-25）

Hash匹配链推进与结果投影分开，按所需列收集256行批次，接续Project/局部聚合；中间列保留int128、每值scale/dscale和NULL，安全时走窄向量/SIMD。LOAD/spill请求先完成，尚未完成的列批次保留在原workmem中；输入游标与匹配链分别恢复。延迟报告未来行错误，聚合左输入只消费最终结果。

无过滤Scan的字符串payload借用当前Heap字节窗口，仅匹配后才物化；字符串键仍物化。Index消费者需要保留窗口时先交出已有批次，Index自身的逐行/I/O生产边界暂不穿透。PG保持Join树，对合法的单聚合分组选择有界Hash并保留函数依赖输出字段及HAVING语义。宽键初始容量使用真实分组估计，但遇未完成的阻塞子节点先从小表起步，避免父表预占子节点预算。

12项设备CTest与Guest布局测试通过，含混合scale、128位payload、字符串未匹配不入字典、LIMIT错误时机、聚合结果探测、Hash→Index跨块、8/16/64槽及4MiB预算spill回收。最终规范22条冷启动均正确、整条卸载且快于同轮host（52.720s→24.355s）；原始记录为results/tpch.qn4rAB。Q10总CSF CPU与Scan CPU均明显下降，Q18无spill、峰值约196MiB，FDM仍64MiB。详细当前证据与未完成边界见performance.md。没有增加固件SQL逻辑、SNIA/NVMe协议或配置项。

## Index Join列输出与接续（2026-10-25）

参照DuckDB Join在匹配游标之外交付结果批次的处理（`physical_nested_loop_join.cpp` 的结果Slice和HAVE_MORE_OUTPUT），计划执行器让Index消费者直接收集融合表达式所需的列。匹配链、Outer/Anti未匹配尾部与列数据分别保留；Semi仍逐候选更新skip标记，最多汇集本次64个输入键对应的左行后执行向量表达式。列化路径不再分配Index逐行结果缓冲，分配失败或表达式不支持时仍使用原接口。所有新增工作区计入原查询workmem，FDM未扩容。

Index可以位于Hash/Project/聚合的列生产链中；Heap窗口优化只沿不含Index的Scan/Hash/Project链判断。Index可能尚有查找或I/O待推进，不能凭远端Scan行已耗尽提前宣布边界。已收集输出前缀的后续错误延迟到前缀消费后报告，EOF也必须记录到生产者，防止释放列批次后误走未初始化的逐行缓冲。

NativePlan与8/16/64 CU测试通过，覆盖多层Index/Hash、NULL外连接、重复键、宽numeric、LIMIT错误时机、4MiB预算及清理。规范Q2/Q8/Q17/Q18/Q21针对性冷启动回归全部正确、整条卸载并快于同轮host；本阶段没有重复完整TPC-H，当前证据见performance.md。PG、SNIA、FEMU本阶段均未增加接口或SQL语义。

## 字典预留的物理上界（2026-10-25）

参照DuckDB只为需要保留的字符串建立存储的方向，先修正PG对既有AFDM字典的预留。`qp_dictionary_size`根据实际Heap字节、所需字符串列和常量求保守上界；最小tuple头24字节、line pointer4字节约束条目数，同表全部内联字符串总字节不超过Heap大小。CASE/COALESCE等只引用已有值；会创建新文本的表达式仍保留原容量，不使用NDV或过滤后行数缩小容量。新增EXPLAIN字段报告字典预留容量。

容量及NativePlan/8/16/64 CU测试、Guest布局测试通过。规范Q7/Q11/Q15/Q21/Q22冷启动结果一致、整条卸载且快于host。Q15对照证实字典AFDM约32MiB降至4.80MiB，允许4MiB输入块、减少主机续交，未增加FDM。Q7原已是小字典，Q21仍因orders状态列的大输入保留最大容量、只有6工作槽；本阶段没有解决共享字典在AFDM中的运行时存储，也不宣称所有查询充分使用8 CU。当前证据见performance.md，未重复全量TPC-H。

## 运行时字典纳入查询workmem（2026-10-25）

参照DuckDB StringHeap保留字符串存储的方式，通用树将字典从AFDM迁入根查询workmem。PG只提供原有条目/字节上限及一页配置；设备SQL程序附上统一分配器，在第一次插入时建立小表，后续按需要增长条目、桶及文本区。没有新增SNIA参数或固件SQL逻辑。query.h是私有SQL数据布局，本阶段PG和全部相关设备SO一起重新构建部署。

已有字符串查找仍不加插入锁；增长在原有插入锁内复制数据并发布新存储，旧存储不移动、不释放，直至根查询结束。读者可以继续使用旧字节视图，未找到的新值在插入锁内重新检查；全部历史存储也计入同一workmem预算。已编码输入按原ID导入，排序/Join/spill中保存的ID不失效。增长失败时不发布部分状态，已有值仍可读取。字典本身还不支持spill，也保留既有容量上限。

新增并发增长用例检查重复插入、实际条目数、扩容前字节视图、分配失败和整体回收；原有物化输入及4MiB预算测试覆盖迁移后的接续。21项相关设备CTest和Guest布局CTest通过。SF1规范22条冷启动均正确、完整卸载且快于同轮host，合计51.872s→20.697s，FDM仍64MiB。Q21实际377项/6724字节文本，现可8槽/4MiB读取块，平均活跃CU约2.60；Q18无spill、总峰值约196MiB，但customer字符串构建阶段仍需优化。当前记录见performance.md，不能据此宣称所有逐行边界或CU利用率问题都已完成。

## 字典插入等待（2026-10-25）

现有共享字典仍以短插入锁发布新值，但等待者不再反复执行交换写入；先读取锁状态，空闲时才尝试acquire，x86等待时PAUSE，release发布完成状态。已有值查找无锁，ID和存储增长/生命周期不变。参照DuckDB将字符串分配与算子工作状态分离的方向，本阶段先减少现有共享ID表示下的写入竞争，不另建每CU字典或修改行表示。

独立8线程/150000个18字节字符串的插入与读回微测中，3次中位墙钟529.345→273.627ms，累计CPU3881.735→1947.642ms；临时源码及二进制已清理。现有增长测试提升为8线程，11项相关CTest通过，含旧字节视图、重复ID、分配失败、回收与4MiB接续。规范Q10/Q18/Q21冷启动全部正确、整条卸载且快于host。本次没有重复完整22条，完整基线仍为a98575e1c。

Q18 customer构建profile墙钟846.351→338.897ms、累计CPU2935.097→1186.894ms；workmem峰值完全相同且无spill，确认改善来自竞争减少。没有改PG/SNIA/FEMU接口、预算或FDM容量。当前普通延迟和原始记录见performance.md，字典spill及其他逐行边界仍未完成。

## SNIA异步请求复用线程（2026-10-25）

SNIA公共请求层保留原请求快照、64个在途请求名额、取消和event/callback完成规则，将逐次创建线程替换为进程内按需创建、空闲复用的工作线程。同步请求不排队；不持工作队列锁执行请求或callback。callback可能等待异步子请求，因此不把工作线程数固定为设备CU数；库卸载时停止接收请求并等待工作线程退出。PG、设备SQL程序、FEMU和插件契约未修改。

六项SNIA CTest通过，新增512次callback链验证线程生命周期复用，以及64个callback全部运行时的同步调用、异步子请求等待和重配续交。Guest libsnia.so已重编安装。规范Q4/Q14/Q21冷启动均正确、完整卸载且快于同轮host；Q21在300ms取消后同连接重新执行完整查询，全部100行匹配host，临时SQL已删除。

mock链的512次请求由512个线程降到26个，单次微测27.535→7.071ms；Q4仍约2600次控制读取/恢复，profile并未证明续交等待明显下降。后续仍需减少跨请求往返，不以线程复用替代设备自主推进；本阶段没有重复完整22条，当前数据与边界见performance.md。

## 索引缓存保留根页（2026-10-25）

参考DuckDB缓冲管理对重复访问页的保留，在设备SQL程序每个索引游标的原128页缓存中保留根页，其他页仍按轮转次序复用。根页与待LOAD槽位不被覆盖，不增加AFDM页数；游标元数据仍在原4KiB预算内。保留连续槽位复用，使现有LOAD批次可以合并目标地址，不为页缓存增加新的协议、查询参数或PG/SNIA调度逻辑。

现有索引压力测试新增根页读取断言：513个查找、466次Heap读取，根页读取4→1次，恢复18→15次。索引、NativePlan和8/16/64 CU共6项CTest通过，含4MiB spill、重复键、NULL、HOT重定向及取消清理。已重编部署pg-sina-plan.so与pg-sina-join.so，PG/SNIA/固件未修改。

规范Q4/Q17/Q21冷启动均正确、整条卸载且快于同轮host。Q4 profile页加载62619→62175、续交3036→2582、LOAD请求3494→3047，但墙钟受推进等待波动影响，本阶段仅确认减少重复I/O和请求，不宣称固定延迟收益或充分利用8 CU。当前成绩见performance.md，未重复完整TPC-H。

## Index Join接入并行哈希聚合（2026-10-25）

参照DuckDB Join输出批次直接进入聚合sink的路径，移除PG计划转换中排除Index Join输入的旧限制；复用现有列输入、共享分组状态、满表接续和spill。Q4保留PG索引连接，将排序从匹配明细移到分组结果之后，worker可以经既有callback连续推进，无需按每批明细返回协调端。

规范Q4/Q13冷启动均正确、完整卸载且快于同轮host；已重新编译安装PG扩展，不改SNIA、FEMU或设备程序。Q4 profile排序累计CPU84.217→0.054ms、控制写入553→1次、workmem峰值4649040→2561104字节，前后无spill。当前聚合sink仅20.511ms累计CPU，平均活跃CU约1.06，因此没有额外增加每CU局部表，后续仍需优化索引读取与任务推进。本阶段未重复完整22条，原始记录见performance.md。

## 并行聚合后的分组结果排序（2026-10-25）

参照DuckDB哈希聚合先生成分组结果的路径，修正PG Finalize GroupAggregate与Partial HashAggregate合并时丢失Gather Merge顺序的问题。归一化在完成聚合上方保留原排序，按表达式映射分组输出位置，保留排序操作符、collation和NULL位置；无法映射时不编译该树。随后复用现有哈希聚合和Sort，不为有序下游排序全部明细。计划副本的父子指针可替换，原生PG执行计划不变。

Q15主路径由明细排序聚合变成Hash Aggregate → Sort分组结果；profile排序累计CPU121.962→3.281ms，workmem峰值25250832→4943968字节，均无spill。规范Q15/Q3/Q18冷启动结果一致、整条卸载且快于同轮host，Guest构建及布局CTest通过。临时分组用例检查ASC/DESC、NULLS FIRST均匹配host且完整卸载，临时SQL已删除。本阶段未改SNIA/FEMU或运行完整22条，普通成绩与profile边界见performance.md。

## 页请求回调续交与输入完成标志（2026-10-25）

PG把独立worker的namespace页请求延续到已有SNIA回调：读取控制快照，使用查询开始前准备好的extent映射构建LOAD→计算batch。有输出、EOF或共享表维护需求时仍返回协调端；回调不调用PG API。LOAD构建改用普通分配/状态返回，主线程和回调复用同一实现；跨线程累计的LOAD时间/次数及控制读取次数使用原子计数，控制快照不再重复读取。

cparam1在QP_SCAN_INPUT位未设置时，用位9表示前一个页请求已由LOAD完成；计算入口设置ready，cparam2的spill重入不重复应用。它是PG/SQL程序私有协议，不是新的SNIA参数或固件能力。删除了逐次ready FDM写入，保留扫描morsel页号编码和旧ready字段调用。PG和pg-sina-plan.so已一起重编部署，未改SNIA/FEMU。

NativePlan与8/16/64 CU测试、Guest布局测试通过；新增4MiB Index排序确认spill重入中的参数保留。规范Q2/Q8/Q17/Q18/Q21冷启动均正确、整条卸载且快于host，本阶段未重复完整TPC-H。Q21诊断中ready独立写入消除，控制写入4137→2次（剩余为协调结果），回调准备平均619.294→275.497μs；仍约4千次回调提交，不能误报为计算提交消失。Q21执行中300ms取消后，同连接恢复查询的全部100行匹配host，期间未重启PG/Guest。原始记录与现存边界见performance.md，临时SQL已清理。
