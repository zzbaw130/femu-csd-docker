# pg_sina_csd：基于 SNIA 的 PostgreSQL CSD 扩展

分支 `pg_sina_csd_dev` 从标签 `sina-femu-csd` 开始。Git 标签、分支和扩展名按项目约定保留 sina 拼写；调用的标准及库名称为 SNIA / snia。

目标是把 PG 执行计划转换为 SNIA batch，由 Guest 调度 CSD 算子。只读实验使用与原生 PG 相同的全表 visible/frozen heap；实验前准备一次 schema、extent 和 LBA 映射，后续持续使用。文件、schema 或设备变化以及重启后重新准备，不设置守护连接。

当前支持外部行排序/TopN、高基数聚合、多表整数等值 INNER Hash Join、B-tree 索引连接及连接后的分组/排序。Q1/Q3/Q5/Q6/Q9/Q10/Q12/Q14/Q19 由 PG 表达式树生成动态 pipeline，通过 CustomScan 完整卸载；PG 负责规划、权限检查及结果 Datum 转换。其余能力按[开发计划](doc/development.md)逐步补齐，尚未支持全部22条 TPC-H。

## 执行路径与冷启动

```sql
LOAD 'pg_sina_csd';
SET pg_sina_csd.exe_path=host; -- 默认：PG原生路径
SET pg_sina_csd.exe_path=csd;  -- 卸载支持的查询计划，不支持时保留PG执行
```

`auto` 将在成本模型实现后开放，目前只接受 `host` / `csd`。切换选项会使 prepared plan 缓存失效，下一次执行按新路径重新规划。需要在规划前 LOAD 扩展；也可通过 session_preload_libraries 加载。

当前为支持的单表聚合和多表连接后聚合生成 `SniaCsdAggregate`，为单表 `ORDER BY` 行查询生成 `SniaCsdSort`；后者支持普通外部排序及有界 TopN，结果分批返回，不受256行总量限制。尚未支持的连接形式、HAVING、表达式等保留原生计划；当前也不在未支持外层查询的子查询中单独卸载，以避免多个分批source各自持有设备锁。后续再扩展到混合子计划。没有准备映射的表也保留原生路径；已有映射失效时执行报错并要求重新 prepare。可用 EXPLAIN 查看实际 SNIA Pipelines。

TPC-H测量每条语句之前使用Guest的cold-query.sh：停止PG清空shared buffers，sync并drop_caches，再启动PG（测试期间autovacuum=off）。host和csd同样处理；不是只清除OS缓存。脚本在Guest执行：

```sh
/home/femu/scripts/cold-query.sh host /path/to/query.sql
/home/femu/scripts/cold-query.sh csd /path/to/query.sql
```

每个文件只放一条被测查询；EXPLAIN ANALYZE也会实际执行，应单独冷启动。普通PG服务重启不改变冻结表文件，可复用映射；Guest/FEMU重启或恢复/改动文件后重新prepare。

## 执行位置与目录

| 路径 | 用途及执行位置 |
| --- | --- |
| src/extension.c | Guest PG 扩展入口、exe_path 与 planner hook |
| src/planner/ | Guest PG 表达式树转设备计划，选择可卸载路径 |
| src/executor/aggregate.c | Guest 聚合/排序 CustomScan 生命周期、分批取结果及最终 Datum 转换 |
| src/executor/mapping.c | Guest PG 读取已准备的 schema/extent 映射 |
| src/executor/batch.c | PG backend 直接调用 SNIA，编排 batch 并回收资源 |
| src/prepare.c | Guest PG 内检查 visible/frozen，checkpoint，保存 schema/extent/LBA |
| src/check.c | Guest 诊断工具，链接 libpq 和 libsnia；提交 LOAD→heap_count，与原生 COUNT 比较 |
| include/ | Guest 与设备共用的查询计划、列批次、结果及诊断计数布局 |
| sql/ | 扩展安装 SQL；sina_csd schema 保存准备结果 |
| tests/ | PG12 页布局、准备测试、SQL 边界用例及 Q1/Q3/Q5/Q6/Q9/Q10/Q12/Q14/Q19 |
| doc/ | 分阶段开发记录 |
| ../../csd-programs/pg_sina_csd/ | 容器编译、FEMU CU 执行的设备算子 |

普通应用仍只包含 cs.h 并链接 -lsnia。这里使用 PostgreSQL 头文件和 libpq，是因为本项目本身是 PG 扩展及其诊断工具，而不是 SNIA 新的使用要求。

## 编译与准备

Guest 已安装 PG12.22、SNIA，且 /data 已挂载。`scripts/guest/install-postgre.sh` 末尾配置 NVMe 设备权限、持久化 udev 规则，并给 postgres 设置 `cap_sys_admin=ep`；该脚本在 PG 已安装时仍会配置权限。已有 Guest 需重新运行一次并重启 PG。该 capability 权限较广，仅按本项目专用实验 Guest 的约定使用。重新安装 postgres 二进制后也需重新执行脚本恢复 capability。可用 scripts/guest/init-csd-env.sh 准备分区与 SNIA；首次或标签不匹配会清除模拟盘数据，请在恢复数据库前运行。

宿主项目根目录执行：

```sh
# 编译设备 SO、Guest 扩展、诊断工具，安装扩展，检查实际 PG 页布局
bash tests/pg-sina-csd-test.sh build
# tpch_s1 已启动且八张表均完全 visible/frozen 时，准备一次映射
bash tests/pg-sina-csd-test.sh prepare
# 真实内部 LOAD + 设备 heap_count，与八张表原生 COUNT 对照
bash tests/pg-sina-csd-test.sh check
# 临时测试表：拒绝未冻结输入、schema/extent 保存、空表
bash tests/pg-sina-csd-test.sh sql
# 每条查询分别冷启动：Q1/Q6、NULL/空输入/排序/回退、prepared plan 切换及取消回收
bash tests/pg-sina-csd-test.sh query
# Join重复键、NULL、连接后分组、字符串、小内存spill及完整Q3/Q5/Q9/Q10/Q12/Q14/Q19
bash tests/pg-sina-csd-test.sh join
# B-tree索引、不同整数宽度、4MB预算、失效回退及Q3
bash tests/pg-sina-csd-test.sh index
# Q3/Q5/Q9/Q10/Q12/Q14/Q19各3轮冷启动，以及独立的profile
bash tests/pg-sina-csd-test.sh join-bench
# 开关、逐算子计时、JSON 输出及开启计时后的结果一致性
bash tests/pg-sina-csd-test.sh profile
# Q1/Q6各3轮，交替host/csd，每次重新冷启动并核对结果
bash tests/pg-sina-csd-test.sh bench
# SF=1 lineitem 上的 TopN 与外部行排序，各3轮冷启动核对和计时
bash tests/pg-sina-csd-test.sh order-bench
```

Guest 中也可手动：

```sh
cmake -S /home/femu/apps/pg_sina_csd -B /home/femu/opt/pg_sina_csd/build \
  -DPG_CONFIG=/home/femu/opt/postgres/release/bin/pg_config -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /home/femu/opt/pg_sina_csd/build -j2
cmake --install /home/femu/opt/pg_sina_csd/build
```

在 tpch_s1 中：

```sql
CREATE EXTENSION pg_sina_csd;
-- 已安装0.1时，重新编译安装后执行：
-- ALTER EXTENSION pg_sina_csd UPDATE TO '0.2';
SELECT sina_csd.version();
-- 只对尚未满足冻结条件的实验表执行；已有冻结归档无需重新制作。
VACUUM (FREEZE, ANALYZE) lineitem;
SELECT sina_csd.prepare('public.lineitem');
SELECT * FROM sina_csd.relations;
SELECT * FROM sina_csd.columns WHERE relid='public.lineitem'::regclass;
SELECT * FROM sina_csd.indexes WHERE heaprelid='public.lineitem'::regclass;
SELECT * FROM sina_csd.extents WHERE relid='public.lineitem'::regclass ORDER BY logical_offset;
```

PostgreSQL 保留 pg_ 前缀的 schema 名，因此扩展名为 pg_sina_csd，schema 为 sina_csd。

prepare 必须由 PG superuser 调用。它检查 heap、visible/frozen 和物理列信息，等待 PG checkpoint，然后查询 FIEMAP；数据文件可以跨多个1GiB段。分区内物理偏移加上分区起始位置转换为 namespace LBA，支持当前80%数据区/20%spill布局。错误使整次准备事务回滚，不保留部分映射。

准备结果保存在数据库，不需要独立 JSON 清单或后台代理。重新准备同一关系会原子替换旧记录；当前不支持 dropped column。实验期间保证映射关系不变，不在每次查询重复检查所有文件。

## 索引访问路径

`prepare(heap)` 同时准备其可用索引，不需要另建一份键值索引。`sina_csd.indexes` 记录索引 OID、所属 Heap、前导属性、根页及键宽度；原有 `relations/extents` 表保存索引文件的物理映射。当前支持 PG12、B-tree v4、升序且 NULLS LAST 的内置 smallint/int/bigint 前导键等值查找，可使用复合索引的前导键。表达式索引、部分索引及其他排序/操作符类保留原有路径。

规划端检查原生 PG 最优路径中的参数化 IndexScan/IndexOnlyScan，当前转换形状为“零个或多个 Hash Join，最后接一个 Index Join”；原生路径不适用或索引未准备时继续使用已有 Hash Join。这里只借用 PG 的访问路径选择，设备仍需读取 Heap，没有实现 Index Only Scan。REINDEX 改变 relfilenode 后旧索引映射不会被选中，重新 `prepare(heap)` 才能再次使用；实验中仍须遵守文件不变的约定。

Q3 的执行流程为：

```text
Customer: LOAD → Scan → Filter → HashJoin.build
Orders:   LOAD → Scan → Filter → HashJoin.probe
          → IndexJoin(lineitem_pkey → TID → Heap投影/过滤)
          → Aggregate.sink
Aggregate.source → Sort/Top10 → PG Datum
```

设备每次处理最多64个查找键。索引缺页时，设备在任务 AFDM 中返回文件类别、页号及目标页槽；PG 仅将这些地址转换为 SNIA `csQueueStorageRequest`，用内部 NVM→AFDM 命令加载，再提交只含 Join 的继续执行 batch。相邻目标槽合并为一个请求，连续物理范围合并读取。它是 `query.h` 中的应用层继续执行约定，不是新增 SNIA 状态；原有 NEED_STORE/LOAD 仍只用于 spill token。

页输入保留在当前槽的 AFDM，最多128页，下一次物理扫描复用缓冲时失效，不跨查询缓存。B-tree 比较、TID 解析、Heap 解码、局部和跨表过滤都在 CSD 执行；PG 不读取索引键、中间 Tuple 或数据页。查询私有游标和页槽标签从 workmem 分配；每个索引状态预留128KiB，其余 build 预算分配给哈希表，两者仍受查询和物理池限制。

EXPLAIN 显示扫描表、索引名、内部页加载次数和实际解码的索引 Heap Tuple 数。Index Join 及其局部过滤的 CPU 时间包含在 `HashJoin.probe+Aggregate` 内；`CSD Heap Rows` 是顺序扫描的 probe 行数，索引 Tuple 另计，`CSD Internal Read` 包含两种读取。

当前没有通用 Bitmap Scan、独立常量范围 Index Scan 或任意位置的多个 Index Join。既有 Hash Join 的外部批次算法仍可能重扫 probe；本阶段使 Q3 改为可驻留的小表 build 加索引查找，没有把该算法描述为已实现分区 Hash Join。

## 内部读取校验

```sh
sudo /home/femu/opt/pg_sina_csd/build/pg-sina-csd-check \
  'dbname=tpch_s1 user=femu' public.lineitem
# 以3个页/块检查跨extent、小块和最后一个不满块的读取
sudo /home/femu/opt/pg_sina_csd/build/pg-sina-csd-check \
  'dbname=tpch_s1 user=femu' public.part 3
```

默认每块4MiB。工具使用数据库保存的映射和原生 SNIA API，构建两个节点的 SERIAL batch：csQueueStorageRequest 所描述的 LOAD → csQueueComputeRequest 所描述的 heap_count；每块只把小型计数结果读回 Guest，原始 heap 不经过工具缓冲。batch 使用 csConfigureBatchEntry 配置，再由 csQueueBatchRequest 提交。

设备输入为 MR[0] 原始页、MR[1] 计数结果；cparam1 为物理列数、cparam2 为实际有效输入字节数，保证尾块不扫描未使用的AFDM空间。算子校验页边界、tuple 边界和 frozen 标志，跳过 UNUSED/REDIRECT/DEAD line pointer。它是无状态诊断算子，不使用 spill 重入，故本入口的第二个标量可以表示长度。

工具启动时检查 Guest boot ID 和 relfilenode；设备或文件变化后仍须主动重新准备。当前固定程序索引500，单次独占使用；不是跨PG backend程序目录管理方案。诊断工具通过sudo访问NVMe，不提升PG服务权限。SQL 卸载由完成安装时权限配置的 PG backend 直接调用 SNIA，具体路径见下文。

PG 会另行执行 COUNT 校验结果，因此本工具用于正确性验收，不是 SQL 性能基准。后续阶段与验收记录见[开发记录](doc/development.md)。

## SQL 卸载的执行位置

```text
Guest PG：解析/规划 → CustomScan → 读取数据库中的静态 extent 映射
Guest PG：异步提交最多4个 SERIAL batch → 各自 LOAD → Scan → Filter → Aggregate.sink
Guest PG：接收任意 batch 完成事件 → 为该执行槽补充下一块，直到输入耗尽
Guest PG：重复提交 source SERIAL batch → Aggregate.source（合并、按需排序与LIMIT）
Guest PG：接收分批最终结果 → 转换 Datum → 返回客户端
```

箭头中的 Scan/Filter/Aggregate/Sort/Limit 是设备 SO，运行在 FEMU CU；PG backend 仅编排请求，不扫描 heap、不计算聚合、不排序。原始 heap 通过 NVM→AFDM 内部路径读取，不经过 Guest 应用缓冲。列批次保留在 AFDM，哈希表等私有状态放在受限 workmem。借鉴 DuckDB 的所需列投影、selection vector、局部聚合后合并及公共子表达式复用；Filter 的列/常量比较（单条件或复合 AND）使用类型化列循环，支持 AVX2 时一次比较4个 int64。复杂过滤和聚合共用 `query_vector.h`，每批最多256行：列引用直接使用原始列和 selection vector，常量只保存一份值，计算结果才物化为临时列。AND/OR 根据左侧结果将仍需判断的行传给右侧；共享表达式缓存每行已完成的结果，避免重复计算，也不会求值被短路忽略的表达式。AVX2 还用于无 NULL/错误批次的加减、乘法及通用比较；乘法要求两个输入均在 int32 范围内，产生精确 int64 结果，加减检查 int64 溢出。不满足条件时回退精确 int128 标量计算，保留 NULL、小数显示精度和溢出语义，不使用浮点近似。

聚合先批量计算表达式和分组位置，再按聚合输入列更新状态；同一表达式的 SUM、AVG、COUNT 共用一份 sum/count，最终按各输出类型分别计算。无分组 COUNT(*) 按批次累加。表达式临时列通过 workmem 分配并复用，未把私有计算空间移入 AFDM。Scan 为常见 numeric 增加直接整数解码，其他格式仍走通用路径；小结果 Sort 排序行号后统一重排，Limit 仅调整结果范围。

PG 估计过滤后保留的行数少于全表一半时，所需 numeric 列采用延迟解码：Scan 在列值槽中暂存原始 AFDM 数据的偏移和长度。Filter 执行 WHERE 时，仅解码当前条件仍需判断的行；简单 AND 先通过已解码列的 SIMD 比较，再依次处理存活行，同一列相邻的上下界共用一次 numeric 解码。复杂 AND/OR 也只把仍需判断的行传给后续表达式。过滤结束后，Filter 为最终存活行物化所有延迟列，供分组和聚合直接读取。Q6 的 quantity、discount、extendedprice 因此都可以延迟解码；Q1 保留绝大多数行，仍采用提前解码。`EXPLAIN` 的 `CSD Deferred Columns` 显示延迟解码列数。这是当前的简单性能启发式；统计估计失准会影响收益，不影响结果。

Filter 的第三个 AFDM 参数是当前块的原始 heap 输入，复用 LOAD 已读入的缓冲，直到 Aggregate.sink 完成后才覆盖；不增加设备命令或 AFDM 分配。原始列位置仅在本分支中使用，不传回 Guest。Filter 计时包含筛选后的 numeric 解码，分析收益应比较 Scan 与 Filter 的合计 CPU 时间。此优化减少数值转换，不减少行存 heap 的内部读取字节数。

PG 扩展直接链接 `libsnia`，在当前 backend 中调用 SNIA；没有查询辅助进程、sudo 调用、socket 或代理。安装时，udev 规则让 femu 用户可打开 /dev/nvme0 和 /dev/nvme0n1，postgres 的 CAP_SYS_ADMIN 满足 Linux vendor NVMe passthrough 的检查。PG 仍以 femu 用户运行，不以 root 启动。

PG 在完成事件轮询之间检查取消。SNIA 调用期间暂缓 PG 中断抛出，避免从 C++ 调用栈跳转；观察到取消或设备错误后停止补充任务，尝试撤销未开始请求，并收完全部在途 batch 的完成事件，再释放聚合状态、AFDM、batch 和程序。当前SNIA后端不能强制终止已经执行的请求，因此必须等它们结束后再让 PG 报告原始中断。

PG 查询设备 FDM/CU 容量，通常按4MiB块分配最多4个执行槽；每个槽是一套独立的输入、列批次缓冲及局部聚合状态，容量不足时减少槽数或块大小。每槽配置一个 SERIAL batch，SERIAL只约束该batch内部顺序，四个batch之间并发。PG通过共享的SNIA event取得完成槽，为它更新下一块LBA并立即重新提交；没有每四块一次的轮次等待。槽不固定绑定CU；同槽当前块完成后才复用缓冲处理下一块，没有双缓冲或预取。全部块已分配且在途batch数为零后，才进入Aggregate.source。`EXPLAIN` 的 `CSD Scheduling` 显示 `independent serial batches`。

此处参考本地DuckDB `Pipeline::LaunchScanTasks` 和 `RowGroupCollection::NextParallelScan` 的任务领取方式。所有块由PG backend维护同一个输入游标，不新增任务队列框架；SNIA库线程只执行设备请求，不调用PG接口。块长度相同时不重复向设备写 input_bytes，仅初始化和尾块变化时更新。当前程序索引501–509由整个查询持有 namespace 文件锁，设备查询串行进入；这是单查询性能实验的实现，尚未支持多 PG 会话共享程序目录。

EXPLAIN ANALYZE 会显示投影列数、设备 CU 数、扫描/匹配行数、内部读取量、AFDM 用量及 workmem 峰值。计时包含程序加载、SNIA 编排与回收，不只统计设备计算。具体类型和规模限制见[设备算子说明](../../csd-programs/pg_sina_csd/README.md)。

## 外部行排序与 TopN

单表行查询例如：

```sql
SET pg_sina_csd.exe_path=csd;
SELECT l_orderkey,l_extendedprice
FROM lineitem
ORDER BY l_extendedprice DESC,l_orderkey
LIMIT 100;
```

PG 根据表达式生成两条设备流水线：

```text
LOAD → Scan → Filter（可选）→ Sort.sink  # 多个独立执行槽，重复处理输入
Sort.source                            # 等待全部sink完成，归并并按批输出
```

这里的 Sort.sink/source 都是 `csd-programs/pg_sina_csd/sort.c` 中的设备 CSF，Guest 不排序。借鉴 DuckDB 的局部排序、阻塞边界及 TopN 堆：`OFFSET+LIMIT` 足够小时，每个sink只保留最优的这些行；否则生成有序段，优先保留在 workmem，预算不足以容纳下一段时通过 NEED_STORE 请求 SNIA 内部 STORE。source 用小根堆归并各执行槽的有序段，通过 NEED_LOAD 读取 spill 数据，完成后通过 NEED_FREE 释放 token。排序输入按实际输出列数紧凑存储；排序目前沿用已支持的 numeric、整数、date、bool、bpchar(1)，不代表已经支持任意字符串。

每次source最多输出256行，PG保留当前查询的 SNIA 设备、batch、结果AFDM和状态句柄，继续提交同一个source batch，直到设备报告结束。256是结果块容量；GROUP BY也已支持超过256组，见下文。OFFSET/LIMIT在设备source处理，不在Guest重复切分。

原始输入AFDM在当前块的Scan/Filter结束后可作为sink的spill I/O缓冲，列结果仍保留在独立AFDM；没有把排序工作区放到AFDM。输入结束后，仅保留source需要的句柄、I/O和结果缓冲，其他输入槽立即释放。单个有序段至多1MiB，spill I/O块为64KiB；这里的64KiB用于归并时按需读入，多个块属于同一个有序段，不会增加归并段数。读缓冲不足时可释放已有spill副本的读缓冲，保留段头，再按需加载。workmem容量由设备配置决定；每个局部上下文按查询上限和实际执行槽数限额。

正常结束先释放token再销毁私有上下文。错误、取消、重扫或提前关闭游标时，先排空在途请求并释放设备私有内存，再关闭本查询的SNIA设备以回收其临时空间；即使失败batch已回滚部分token，内存回收也不依赖再次释放这些失效token。PG查询内存上下文的清理回调负责兜住异常退出，无辅助进程。

`EXPLAIN ANALYZE` 增加 `CSD Sort Runs`、`CSD Spilled Runs`、`CSD Spill Write/Read`；开启profile后分别展示 `Sort.sink` 和 `Sort.source`。累计调用包含I/O重入，不等于输入块数。

## 开发阶段的算子计时

```sql
LOAD 'pg_sina_csd';
SET pg_sina_csd.exe_path=csd;
SET pg_sina_csd.profile=on;          -- 默认off；运行时生效，不必重新规划
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF)
SELECT sum(l_extendedprice*l_discount)
FROM lineitem
WHERE l_shipdate>=date '1995-01-01' AND l_shipdate<date '1996-01-01'
  AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity<25;
SET pg_sina_csd.profile=off;         -- 正常测量及发布时关闭
```

TPC-H 仍须每条查询先冷启动。将 `SET pg_sina_csd.profile=on;` 和一条 EXPLAIN ANALYZE 写入 SQL 文件，再使用 `cold-query.sh csd 文件路径`；不要在同一文件反复执行查询来冒充冷启动。普通 SELECT 开启后也会收集统计，但不额外打印日志；查看统计使用 EXPLAIN ANALYZE，支持 TEXT/JSON 等格式。`TIMING OFF` 关闭 PG 逐节点计时，不关闭本开关控制的设备计时。

开启后增加 CSD Profile：

| 指标 | 含义 |
| --- | --- |
| Setup Wall | PG 执行端准备 SNIA 设备、程序、缓冲和首批请求；包含 namespace 锁等待 |
| Build Wall | 仅Join出现：build表内部LOAD、Scan/Filter及HashJoin.build；最后一个spill批次的完成和载入计入Pipeline 1 |
| Pipeline 1 Wall | 整个块循环的实际墙钟时间，包含内部 LOAD、SNIA 编排/等待和 Scan/Filter/Aggregate.sink |
| Pipeline 2 Wall | 收集局部状态、编排与执行 Aggregate.source/Sort/Limit，并读取最终结果 |
| Cleanup Wall | 读取计时统计，释放 batch、设备状态、AFDM、程序与设备句柄 |
| Total Wall | 上述阶段之和（Join包含Build Wall）；不包含 PG 规划、静态映射读取及结果 Datum 转换，故不等于整个 SQL Execution Time |
| 每个算子的 calls | 实际数据算子调用次数，不包含专用于取消/回收的调用 |
| wall_sum / CPU Sum | 入口到返回的墙钟时间 / 执行该算子的 CU 线程 CPU 时间，跨块及 CU 累计 |
| wall_max | 最慢的一次算子调用的墙钟时间 |

设备端计时覆盖 Scan、Filter、Aggregate.sink/source、Sort、Limit。多 CU 累计时间可能超过查询墙钟时间，不能将 wall_sum 相加当作查询延迟，或直接用累计值除以 Total Wall 得到耗时占比。wall_sum 包含运行期间被宿主调度挂起的时间；CPU Sum 反映 FEMU 宿主 CU 线程实际占用，不代表真实 SSD 芯片周期。入口计时不含 NVMe 排队/完成或模拟 I/O 延迟；内部 LOAD 当前包含在 Pipeline 1 Wall 中，未单独测量，剩余时间不能全部解释为 I/O。

计时统一实现于设备 `profile.c` 入口包装与 PG `executor/batch.c`，没有在设备算法的行循环中插入代码。关闭时直接调用原实现，不读时钟、不分配/读取计时 AFDM；仍保留入口的一次条件判断。开启后每个 CU 增加一个计数输出 AFDM（当前按4KiB分配），一次调用读取两种时钟各两次；有额外开销，性能结论应同时保留关闭时的冷启动测量。

Q1/Q6三轮统计、perf热点及优化顺序见[性能分析](doc/performance.md)。复现：`bash tests/pg-sina-csd-test.sh analyze`；该入口另采集host/csd/profiled三组EXPLAIN及SNIA日志，结束后恢复普通PG环境。

### 查询工作内存上限

```sql
SET pg_sina_csd.max_memory = '64MB';
-- 对同一条查询测试较小预算：
SET pg_sina_csd.max_memory = '16MB';
```

这是本项目的PG参数，默认64MB、最小4MB，限制一条设备查询全部执行槽的workmem，不包括独立的AFDM输入输出缓冲、Guest编排内存或FEMU进程本身。参数在执行时读取，已PREPARE的查询也使用当前值。各槽平均分配预算，设备再以共享池实际容量限额；设备池的硬上限仍由启动时的`CSD_WORKMEM_SIZE_MB`决定，参数设得更大不会扩充设备内存。修改设备池容量须在下一次启动FEMU时生效，不能在有存活状态时调整池大小。

例如查询预算16MB、4槽，每槽最多4MB。每槽先为可能并发使用的Filter表达式临时列及其分配元数据留出空间，剩余预算交给Sort/聚合私有上下文。这样各槽Filter与sink即使同时运行，也不会因分别使用完整预算而超限。当前槽间采用静态均分，尚未实现DuckDB的动态预算再分配；复杂表达式的不可spill工作区若连最低预算都无法容纳，会报告设备执行错误。

排序不采用“空闲低于50%”的固定触发线。参考本地DuckDB `src/common/sort/sort.cpp`的预算/线程分摊和外部排序判定：当前实现先保留内存段，准备下一段时检查“已用内存＋新段＋64KiB归并I/O余量＋分配开销”是否超过槽预算，超过则将已排序内存段spill。实际分配失败也会触发spill，以处理共享池竞争或碎片。预算减小时单段容量从1MiB逐步缩小，最低64KiB；归并仍按64KiB加载所需块，并可丢弃已有NVM副本的读缓存。

`EXPLAIN ANALYZE`的`CSD Query Memory Limit`显示执行时配置的查询上限，实际仍受设备池容量限制；`CSD Workmem Peak`来自设备共享分配器，是该池生命周期的历史峰值，不应用它判断某条低预算查询是否超限。独立进程的设备测试会核对实际峰值及清理后的零占用。运行`bash tests/pg-sina-csd-test.sh memory`可冷启动核对4/16/64/128MB预算下Q1、Q6和外部排序结果，以及已PREPARE查询的参数切换。


## 高基数 GROUP BY 与 spill

所有支持的GROUP BY均使用自适应路径，不依赖PG对分组数的估计来保证正确性。每个执行槽先在workmem内用小哈希表预聚合；表满时，把部分聚合状态交给已有外部排序算法，生成按分组键排序的run。状态包含分组键、SUM、非NULL计数、显示精度；AVG保留sum/count，最后合并后才做除法，不会平均各批次的平均值。

```text
LOAD → Scan → Filter → Aggregate.sink         # 各槽局部哈希预聚合
Aggregate.source                              # 合并相同键的部分状态，分批输出
  └─ 若SQL有ORDER BY：结果Sort.sink → Sort.source（包括OFFSET/LIMIT）
```

这些步骤都在设备SO中执行。PG仍从表达式树生成分组键、聚合、排序及LIMIT描述；设备不解析SQL。`pg-sina-aggregate.so`直接复用同一份`sort.c`算法作为私有函数，不发送嵌套NVMe请求、不额外占用CU，也不增加固件/runtime。输入、结果和spill传输使用AFDM，哈希表、部分状态和归并缓存仍放workmem。小分组在所有局部表未flush且合并容量足够时，直接走原小哈希表快路径，不生成run；排序和LIMIT也在source内完成。

每槽预算先扣除过滤临时空间，再为哈希表、表达式和状态块保留固定空间。有ORDER BY时，把余下预算分给部分状态排序和最终结果排序；两者可能同时存活，总额不能分别使用一整份查询上限。没有ORDER BY则将余下预算用于部分状态run。阈值按所需空间确定，不用全局空闲比例。局部表暂时仍是256组，满表仅意味着flush，不再是总分组数上限。中间记录目前要求“分组键数＋聚合数”不超过16，已知超出或不支持的语义在规划时回退PG。

source每次最多返回256行，PG复用现有分批游标。若SQL要求排序，先完整归并聚合，再输出排序结果；否则可按组连续输出。取消、报错、提前关闭时，回收部分状态排序和结果排序的所有私有上下文，再由本查询的SNIA设备回收剩余token。

`EXPLAIN`显示`CSD Aggregation: adaptive partial-state merge`。开启profile时，`Aggregate.sink/source`包含其内部排序的CPU时间及I/O重入；这些不是额外提交的CSF，不能重复计入`Sort.sink/source`。独立行排序仍单独计时。spill读写与run数汇总两轮内部排序。

本实现参考DuckDB的局部哈希预聚合、部分状态合并和算子内存预算；采用现有排序基础设施完成有界归并，没有实现DuckDB完整的radix分区哈希聚合。run描述仍驻留内存，极低预算/极多run仍可能耗尽元数据；受预算约束的多轮归并仍待实现。

```sh
bash tests/pg-sina-csd-test.sh query
bash tests/pg-sina-csd-test.sh aggregate-bench
```

`aggregate-bench`使用`tests/high_aggregate.sql`在SF=1 lineitem上筛选1995年数据，按partkey聚合销售额、平均数量和计数，再按销售额取前100；这是算子验证语句，不是新增已经完成的TPC-H查询编号。

## Hash Join 与连接后分组

当前支持已准备且属于同一namespace的heap表组成整数等值INNER连接链、连接后跨表复合过滤，以及连接后的标量或分组SUM/AVG/COUNT、结果排序和LIMIT/OFFSET。重复键产生所有匹配组合，NULL键不匹配。PG先按原始行数选择较小build侧，分别编译所需列和本地过滤；若该侧过滤后投影的估计空间超过build预算，则也编译另一方向，比较两侧过滤后的记录和哈希桶空间。估计只选择方向，实际超限仍走分批spill；没有按查询编号选择算法。多表连接、连接后普通行输出、没有整数等值键的连接和外连接暂时保留原生计划。

```text
Guest PG编排；以下数据算子在FEMU CU执行：
build表：多个独立槽 LOAD → Scan → Filter（可选）
        完成一块 → 单写入者 HashJoin.build → 该槽领取下一块
每种build批次组合：
    HashJoin.load（驻内存时无需I/O；spill批次通过NEED_LOAD载入）
    probe表 LOAD → Scan → Filter（可选）→ HashJoin.probe → Filter(join，可选) → Aggregate.sink
Aggregate.source → 分组归并/结果排序（按需）或标量数值表达式 → 结果
```

build侧复用最多4个独立执行槽并行LOAD/Scan/Filter；完成一个槽后，PG用该槽的列批次提交一次HashJoin.build，再补入下一块，不等待其余槽。HashJoin.build仍单写入：追加或spill期间，其他槽可扫描/过滤，但不能同时修改共享哈希表。所有build输入消费完毕后才开始probe；错误/取消先排空在途扫描，再释放设备状态。`EXPLAIN`中的`CSD Build Scheduling`说明这个调度方式。没有为单槽加入双缓冲预取。

HashJoin.build只保存投影后的键和载荷，哈希桶和记录位于workmem。查询预算一半给共享build状态，另一半由probe/聚合执行槽分摊，实际设备池仍是硬上限。估计行数只决定初始容量；满表先在build上下文限额内扩容记录和桶数组，失败或达到最大容量后才以64KiB块返回NEED_STORE，SNIA执行内部AFDM→NVM；重入后把token保存在设备私有批次描述中。PG只读回批次数，不读取哈希表或中间行。所有probe完成后才能载入下一批，避免并发改写共享哈希表。

每批probe使用最多4个独立执行槽领取输入块，HashJoin.probe生成256行的连接列批次，并直接调用编译进同一`pg-sina-join.so`的聚合函数。该调用不提交嵌套NVMe命令、不额外占用CU；聚合状态与`pg-sina-aggregate.so`的source共用布局。多批build时重扫probe表，聚合状态一直保留到最终source。这个有界实现可处理重复键和倾斜，但没有实现DuckDB的radix分区/动态重分区；小预算下重复扫描会增加内部I/O。批次描述仍驻内存，极多批次可能耗尽元数据预算。

连接后GROUP BY复用现有局部哈希/部分状态排序。聚合请求spill时，probe保存当前输入行、重复键链中的下一项及待提交的列批次，返回同一个NEED_STORE/LOAD给SNIA；恢复后先完成待提交聚合，再继续生成连接行，不重复计数。最后一个AFDM作为聚合I/O暂存，复用已完成Scan/Filter的原始输入空间。全部probe结束后立即释放build哈希表，其原预算用于source的最终结果排序；probe期间每槽余量用于部分状态run。4MB查询预算时，分组Join减少为两个执行槽，为每槽保留约1MiB用于固定状态、表达式和spill缓冲。极复杂表达式或过多run仍可能耗尽不可spill元数据。

Q14还使用向量化搜索CASE：只计算选中分支，NULL条件进入ELSE；前缀`LIKE 'PROMO%'`直接在Scan中提取为布尔列。当前只支持C/C.UTF-8排序规则下的ASCII常量前缀、末尾一个`%`，同列只能使用一个前缀，不支持通用字符串输出/排序或其他通配符。聚合后的numeric加减乘除在设备上按PG显示精度和舍入规则执行，使用受溢出检查的int128，超出表示范围报错。

开启profile会显示`HashJoin.build`、`HashJoin.load`和`HashJoin.probe+Aggregate`；最后一项包含内部聚合，不能再重复相加为另一个CSF。`EXPLAIN`显示`CSD Build Relation`，ANALYZE还显示Build Rows、Build Batches、Join Rows和spill字节数。Build Rows为build扫描原始行数，Heap Rows/Matched Rows为各轮probe累计数，Internal Read包含build和所有probe轮次。方向在规划时选择；已PREPARE的计划会使用新的运行时内存上限，但不保证随预算重新选择方向。错误或取消先排空probe，再释放聚合/build工作区，最后关闭本查询SNIA设备回收全部临时token。

## 字符串常量比较与 Q12

Q12按shipmode分组，并根据orderpriority的两个常量计算CASE。PG将SQL中这些字符串常量保存在查询计划的有限字典中，Scan把匹配值输出为整数编号；不匹配的非NULL值用0，NULL仍用原NULL位图。下游继续使用8字节列槽，可向量化执行等于/不等于、IN及CASE，连接和spill只传编号，不保存设备指针或完整字符串。字典按列和字节顺序排列，因此同列编号可用于分组结果排序。PG只在最终Datum转换时用计划中的常量恢复字符串。

原始字符串的投影、GROUP BY或ORDER BY仅在PG证明该列受到顶层`= 常量`或`IN (常量...)`过滤时开放，此时每个存活值都有已知编号。藏在OR/CASE中的比较不构成这个证明；字符串间比较、一般范围比较、未受约束字符串输出仍回退PG，避免将不同未知值都当作0。支持C/C.UTF-8下的ASCII text/varchar/bpchar常量，当前每条计划最多32项、每项127字节；bpchar比较去掉尾空格，text/varchar保留尾空格，最终bpchar恢复列的长度。其他排序规则、非ASCII或更长常量保留原生路径。

这借鉴了DuckDB字典编码和选择向量的思路，是针对SQL字面量的分类，不是任意字符串字典或通用字符串引擎。没有新增文件格式、映射表或SNIA接口。`EXPLAIN`显示`CSD String Literals`，完整示例见`tests/q12.sql`。

### Q19与连接后复合过滤

PG从两表INNER JOIN的条件中选择一个直接整数列等值条件作为哈希键，把其余跨表条件编译成已有表达式树。单表条件仍由对应物理Scan/Filter执行。设备probe先按等值键生成最多256行的列批次，再调用编译进同一`pg-sina-join.so`的Filter实现，用selection vector执行跨表比较及AND/OR，只有存活行进入聚合。Filter使用现有向量求值、NULL三值逻辑和短路选择；不是逐行返回Guest判断。

这次私有Filter调用不提交NVMe、不增加CU；计时包含在`HashJoin.probe+Aggregate`内，EXPLAIN显示`Filter(join)`。本地过滤已完成延迟numeric解码，连接后Filter直接读取已物化的列值。聚合请求spill时，保留过滤后的选择向量和probe位置；重入只恢复待提交聚合，不重做过滤或重复统计。Join Rows为跨表过滤后的连接行数。

常量IN除字符串外，支持smallint/int/bigint/numeric数组，编译为现有比较与OR表达式，单数组最多32项，NULL保留SQL语义。不支持的数组形式仍保留原生PG计划。Q19完整验证入口为`tests/pg-sina-csd-test.sh join`，性能入口`join-bench`。`tests/q19.sql`使用项目已有qgen参数及原始OR写法；`tests/q19_variant.sql`使用另一组参数并将公共连接条件提到OR之外；不按SQL文本或Query编号选择执行路径。

连接与聚合仍共享同一查询内存上限。聚合常驻时，单表Filter与连接后Filter的较大临时空间会从每槽预算中预留。带跨表过滤时，小预算下减少执行槽，为每槽保留约2MiB用于表达式与聚合状态；分组Join无跨表过滤时维持约1MiB。没有提高查询预算或物理workmem池上限。


### 多表连接与Q3

目前支持最多8个已准备heap表组成的连通INNER等值连接链，受既有64个物理属性、128个表达式、8个分组键等布局上限约束。无索引形状时，设备按“单列唯一键优先、唯一键候选中过滤后较大关系优先”的顺序选择probe链；两表查询仍保留原来的方向选择。其他跨表谓词在最终列批次上执行，尚未实现任意树形连接计划或代价优化。

每张build表有独立workmem哈希状态和控制AFDM，PG依次提交其并行LOAD/Scan/Filter与单写入build。probe时使用一个控制AFDM汇总这些不透明状态句柄、批次编号和统计，不读取哈希表。当前FEMU计算参数不接受非零AFDM偏移，因此控制AFDM保持独立，不修改SNIA或设备接口。

一个probe计算请求在同一CU内依次执行各Join。每级输出最多256行的列批次，保存在该槽joined AFDM的独立区域，直接交给下一级；只有最后一级送入Filter(join)/Aggregate.sink。每级游标保留在各自build workmem中。聚合请求NEED_STORE/LOAD时，整条调用链返回该完成值，重入逐级恢复未完成的子计算，不重复输出。没有把中间行传回Guest，也没有为每级Join另发NVMe计算请求。

查询预算仍一半给build、一半给probe/聚合；build份额按每表过滤后行数、投影列数和2的幂容量估算单批字节后按比例分配，小表保留1MiB下限，设备还按实际物理池容量限制每张表的份额。每张表先在自身份额内增长，超限spill。PG加载各表批次的一种组合，排空所有probe请求后才切换组合；因此多张表都spill时，probe重扫次数为各表批次数的乘积。`CSD Build Batches`表示组合数，Build Rows为各build表原始行数之和；`CSD Join Count`、Build Relation列表和SNIA Pipelines显示实际连接链。估算失准仍可能让小表闲置或大表多批，后续再改进预约分配或分区连接，不能把本实现称作DuckDB的完整内存调度器。

Q3 SQL位于`tests/q3.sql`，使用原有qgen参数。三表、四表、后级键来自前级build载荷、重复键、NULL、空表、小内存spill及断开连接图的回退均纳入join回归。任意字符串输出、非INNER连接、HAVING和子查询仍需后续实现。

## 运行期字符串字典与 Q5/Q10

Q5/Q10 的字符串没有顶层等于/IN 约束，不能使用 SQL 常量字典。查询级运行期字符串字典 AFDM：Scan 把 `char/varchar` 的空格处理结果插入字典并输出 8 字节 ID；Hash Join、分组和排序只传 ID；Aggregate.source 在最终结果块离开设备前恢复字节。Q5 使用 512 项/64KB 小字典，短字符串内联在 16 字节 cell；Q10 使用 1,048,576 项/20MB 大字典，超过 15 字节的字符串通过 `qs_result` 的 64KiB arena 回传。两种档位都不改变 `qs_cell`、Hash Join 记录、Sort run 或 spill 布局。

Q10 的 SQL 虽然 GROUP BY 7 列，PG 会按主键函数依赖去掉 5 列；规划端把这些 FD 输出列重新加入分组键，因此设备仍只哈希整数 ID，不需要变长 group record。大字典在 Scan 阶段会插入 customer 全部 150k 行的字符串（约 19.6MB），超过容量时设备报错，不回退。字符串 `ORDER BY` 仍保留原生路径；Q5/Q10 都按 revenue 排序。

Q5 的多表连接顺序也做了调整：候选表按“单列唯一键优先、唯一键候选中过滤后较大的关系优先”加入，避免先按 `c_nationkey=s_nationkey` 连接 customer/supplier 产生巨大中间结果。Q5 实际链为 lineitem→orders→customer→supplier→nation→region，最后执行 `c_nationkey=s_nationkey` 残差。Build 预算按每表过滤后行数、投影列数和 2 的幂容量计算单批所需字节，再按比例分配；默认 64MB 下 Q5 五张 build 表均为 1 批，lineitem 只扫描一次。

Q5 默认 64MB 冷启动三轮（Q9 提交后复核）：host 2275.099ms、CSD 1212.940ms（均值，约 1.88 倍）；Q10 host 2287.015ms、CSD 1531.757ms（约 1.49 倍）。两条查询结果均与 host 逐字一致。Q5/Q10 16MB 查询预算下分别为 11102.431ms 和 5062.922ms，结果一致，属于内存受限 spill。取消后同一 backend 可继续执行 CSD COUNT。`tests/q5.sql`、`tests/q10.sql` 为完整查询，入口为 `tests/pg-sina-csd-test.sh join|join-bench`。

Q9 进一步增加复合连接键 `(l_partkey,l_suppkey)=(ps_partkey,ps_suppkey)`、`Q_YEAR`、`%` 通配 Scan 和短字符串排序。175 行结果与 host 一致；64MB 三轮 host=3434.695ms、CSD=7588.709ms（约2.21倍），Build Batches=6，仍有6次lineitem重扫。32MB 下CSD=42491.859ms且结果一致；16MB超过55s未返回，当前最低验收内存32MB。Q9的性能仍未达到“接近host”的目标，后续需要分区Hash Join或probe物化，详见[开发记录](doc/development.md)。
