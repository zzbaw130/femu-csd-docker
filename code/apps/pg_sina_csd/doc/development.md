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
- 第一条 pipeline 为 LOAD→Scan→Filter→Aggregate.sink；按设备容量，构建最多4个并行分支的 HYBRID batch。分支间使用独立缓冲和私有聚合状态，重复处理4MiB块直到输入结束。
- 第二条 pipeline 为 Aggregate.source→Sort→Limit，采用 SERIAL batch；合并局部状态、精确 AVG 除法、排序、LIMIT 都在设备执行。Guest 只转换结果类型并返回客户端。
- Scan 提取所需列，Filter 生成 selection vector；表达式树复用公共子表达式。参考本地 DuckDB table_scan.cpp、physical_filter.cpp 和 physical_hash_aggregate.cpp 的投影、选择向量及局部聚合组织；不直接复用其存储格式；该阶段初版没有显式 SIMD，后续 Filter 向量化见性能分析。
- numeric 以定点整数精确计算并保留显示精度，AVG 对齐 PG numeric 的精度与舍入。PG12 的 palloc 只保证8字节对齐，含 __int128 的结果显式16字节对齐。
- AFDM 仅保存输入输出、计划和句柄，聚合状态及排序工作区使用共享 workmem 分配器。错误和取消时调用设备清理入口释放状态，再释放 AFDM、batch 和程序。查询持有 namespace 文件锁，当前只支持单个设备查询进入，不属于多会话并发调度实现。
- EXPLAIN 展示真实 pipeline、投影列、CU 数、行数、内部读取和内存指标。Q1/Q6 以真实 SF=1 heap 验证，每条扫描6001215行、内部读取921624576字节。

验收：7项设备 CTest 和 Guest PG12 页布局测试通过；计数及列批次聚合的 ASan/UBSan 测试通过（环境限制关闭 LeakSanitizer）。SQL 回归覆盖 NULL/空输入/无匹配、布尔条件、精确 SUM/AVG/COUNT、排序/NULL顺序/OFFSET/LIMIT、原生回退、prepared plan 路径切换及100ms超时取消后完整设备复用。Q1四组全部列（含精度和小数位）与Q6结果逐字节一致，另验证Q1 LIMIT 1。每次执行均独立冷启动。

当前限制：最多256个分组，排序仅处理聚合结果；numeric 输入仅支持有限、至多两位小数并可表示为int64百分数的值。规划时已知不支持的能力回退PG；运行时数据越界或组数超预期报错。尚未实现通用高基数聚合/行排序spill、Join或所有22条TPC-H。

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
| 2 | 通用阻塞边界：行排序/TopN、高基数聚合，workmem优先，NEED_STORE/LOAD/FREE驱动spill与重入；保留sink/source状态生命周期 | 大数据排序、分组和强制小内存正确性 |
| 3 | Hash Join build/probe、投影传递、分区spill，扩展CASE和必要字符串表达式 | Q14，再Q3/Q5/Q10/Q12/Q19 |
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
