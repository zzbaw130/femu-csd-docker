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
- Guest 每查询启动一个 C worker，经现有 sudo 权限提交 SNIA 请求；worker 不解析 SQL，不读取 heap 到应用缓冲，不执行 SQL 计算。原始数据通过内部 LOAD 进入 AFDM。没有常驻代理，没有改动 FEMU 或 SNIA 核心。
- 第一条 pipeline 为 LOAD→Scan→Filter→Aggregate.sink；按设备容量，构建最多4个并行分支的 HYBRID batch。分支间使用独立缓冲和私有聚合状态，重复处理4MiB块直到输入结束。
- 第二条 pipeline 为 Aggregate.source→Sort→Limit，采用 SERIAL batch；合并局部状态、精确 AVG 除法、排序、LIMIT 都在设备执行。Guest 只转换结果类型并返回客户端。
- Scan 提取所需列，Filter 生成 selection vector；表达式树复用公共子表达式。参考本地 DuckDB table_scan.cpp、physical_filter.cpp 和 physical_hash_aggregate.cpp 的投影、选择向量及局部聚合组织；不直接复用其存储格式，也没有显式 SIMD。
- numeric 以定点整数精确计算并保留显示精度，AVG 对齐 PG numeric 的精度与舍入。PG12 的 palloc 只保证8字节对齐，含 __int128 的结果显式16字节对齐。
- AFDM 仅保存输入输出、计划和句柄，聚合状态及排序工作区使用共享 workmem 分配器。错误和取消时调用设备清理入口释放状态，再释放 AFDM、batch 和程序。查询持有 namespace 文件锁，当前只支持单个设备查询进入，不属于多会话并发调度实现。
- EXPLAIN 展示真实 pipeline、投影列、CU 数、行数、内部读取和内存指标。Q1/Q6 以真实 SF=1 heap 验证，每条扫描6001215行、内部读取921624576字节。

验收：7项设备 CTest 和 Guest PG12 页布局测试通过；计数及列批次聚合的 ASan/UBSan 测试通过（环境限制关闭 LeakSanitizer）。SQL 回归覆盖 NULL/空输入/无匹配、布尔条件、精确 SUM/AVG/COUNT、排序/NULL顺序/OFFSET/LIMIT、原生回退、prepared plan 路径切换及100ms超时取消后完整设备复用。Q1四组全部列（含精度和小数位）与Q6结果逐字节一致，另验证Q1 LIMIT 1。每次执行均独立冷启动。

当前限制：最多256个分组，排序仅处理聚合结果；numeric 输入仅支持有限、至多两位小数并可表示为int64百分数的值。规划时已知不支持的能力回退PG；运行时数据越界或组数超预期报错。尚未实现通用高基数聚合/行排序spill、Join或所有22条TPC-H。

## 统一冷启动测量规则

所有 TPC-H 被测语句执行前停止 PG，清空 shared buffers；sync 并清除 Guest OS page cache，再启动 PG。测试期间关闭 autovacuum。host 与 csd 完全采用同一流程，EXPLAIN ANALYZE 也单独冷启动。设备每次新建和回收 AFDM/workmem，不复用前次输入。FEMU 模拟 NVM 字节本身是存储介质，不作为缓存清除。

普通 PG 服务重启且文件不变时复用映射；Guest/FEMU 重启、数据恢复或文件/schema 变化时重新 prepare，不为每次查询重新 FIEMAP。性能采用各3轮、交替顺序，逐次核对输出；包含 worker 启动、程序加载、查询及回收时间，不包含重启/清缓存时间。

```sh
bash tests/pg-sina-csd-test.sh query
bash tests/pg-sina-csd-test.sh bench
```

## 本阶段冷启动性能（2026-10-21）

现有 SF=1 tpch_s1，PG12.22，Guest 8 vCPU/8GiB；原生 PG 配置 shared_buffers=128MiB、work_mem=4MiB、max_parallel_workers_per_gather=2。设备4 CU、64MiB AFDM、64MiB workmem。未更改原生PG配置来制造加速。三轮交替顺序，单位为秒：

| 查询 | host 三轮 | csd 三轮 | host均值 | csd均值 | host/csd |
| --- | --- | --- | --- | --- | --- |
| Q1（四组完整结果） | 4.905493 / 4.380100 / 4.383609 | 2.005434 / 1.646425 / 1.628251 | 4.556401 | 1.760037 | 2.59 |
| Q6 | 1.500574 / 1.749342 / 2.116437 | 1.753388 / 1.868625 / 1.585608 | 1.788784 | 1.735874 | 1.03 |

Q6差异小于本次测量波动，不能据此认定有明显加速。Q1测试去掉原查询的LIMIT 1以核对四组全部结果；另做LIMIT 1正确性验证。Q6保留原LIMIT 1。原始每轮耗时由 bench 保存到 Guest 的结果目录，后续同样用该脚本生成，不把一次 EXPLAIN 的耗时当作三轮均值。

单独冷启动的 EXPLAIN ANALYZE 确认二者只有 SniaCsdAggregate，无原生 Scan/Agg/Sort；设备均4 CU、220块、扫描6001215行、内部读取921624576字节。Q1投影7列，匹配5884033行，AFDM64385024字节；Q6投影4列，匹配119810行，AFDM45248512字节。workmem峰值679872字节；AFDM与私有工作内存均未超过各自64MiB限制。这些是少量分组的聚合，不会触发spill；后续阶段必须专门覆盖内存不足。

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
