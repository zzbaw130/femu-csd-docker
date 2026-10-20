# pg_sina_csd：基于 SNIA 的 PostgreSQL CSD 扩展

分支 `pg_sina_csd_dev` 从标签 `sina-femu-csd` 开始。Git 标签、分支和扩展名按项目约定保留 sina 拼写；调用的标准及库名称为 SNIA / snia。

目标是把 PG 执行计划转换为 SNIA batch，由 Guest 调度 CSD 算子。只读实验使用与原生 PG 相同的全表 visible/frozen heap；实验前准备一次 schema、extent 和 LBA 映射，后续持续使用。文件、schema 或设备变化以及重启后重新准备，不设置守护连接。

当前完成阶段1：Q1/Q6 由 PG 表达式树生成动态 pipeline，通过 CustomScan 卸载 Scan、Filter、表达式、SUM/AVG/COUNT、低基数分组、结果排序与 LIMIT/OFFSET。Q1/Q6 的 SQL 数据算子都在 CSD 执行；PG 负责规划、权限检查及结果 Datum 转换。其余能力按[开发计划](doc/development.md)逐步补齐，尚未支持全部22条 TPC-H。

## 执行路径与冷启动

```sql
LOAD 'pg_sina_csd';
SET pg_sina_csd.exe_path=host; -- 默认：PG原生路径
SET pg_sina_csd.exe_path=csd;  -- 卸载支持的查询计划，不支持时保留PG执行
```

`auto` 将在成本模型实现后开放，目前只接受 `host` / `csd`。切换选项会使 prepared plan 缓存失效，下一次执行按新路径重新规划。需要在规划前 LOAD 扩展；也可通过 session_preload_libraries 加载。

当前只为单表聚合生成完整 CustomScan；不支持的连接、HAVING、表达式等保留原生计划。后续再扩展到混合子计划，不会将不支持的算子伪装成设备执行。没有准备映射的表也保留原生路径；已有映射失效时执行报错并要求重新 prepare。可用 EXPLAIN 查看 `SniaCsdAggregate` 及实际 SNIA Pipelines。

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
| src/executor/aggregate.c | Guest CustomScan 生命周期及最终 Datum 转换 |
| src/executor/mapping.c | Guest PG 读取已准备的 schema/extent 映射 |
| src/executor/batch.c | PG backend 直接调用 SNIA，编排 batch 并回收资源 |
| src/prepare.c | Guest PG 内检查 visible/frozen，checkpoint，保存 schema/extent/LBA |
| src/check.c | Guest 诊断工具，链接 libpq 和 libsnia；提交 LOAD→heap_count，与原生 COUNT 比较 |
| include/ | Guest 与设备共用的查询计划、列批次、结果及诊断计数布局 |
| sql/ | 扩展安装 SQL；sina_csd schema 保存准备结果 |
| tests/ | PG12 页布局、准备测试、SQL 边界用例及 Q1/Q6 |
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
# Q1/Q6各3轮，交替host/csd，每次重新冷启动并核对结果
bash tests/pg-sina-csd-test.sh bench
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
SELECT sina_csd.version();
-- 只对尚未满足冻结条件的实验表执行；已有冻结归档无需重新制作。
VACUUM (FREEZE, ANALYZE) lineitem;
SELECT sina_csd.prepare('public.lineitem');
SELECT * FROM sina_csd.relations;
SELECT * FROM sina_csd.columns WHERE relid='public.lineitem'::regclass;
SELECT * FROM sina_csd.extents WHERE relid='public.lineitem'::regclass ORDER BY logical_offset;
```

PostgreSQL 保留 pg_ 前缀的 schema 名，因此扩展名为 pg_sina_csd，schema 为 sina_csd。

prepare 必须由 PG superuser 调用。它检查 heap、visible/frozen 和物理列信息，等待 PG checkpoint，然后查询 FIEMAP；数据文件可以跨多个1GiB段。分区内物理偏移加上分区起始位置转换为 namespace LBA，支持当前80%数据区/20%spill布局。错误使整次准备事务回滚，不保留部分映射。

准备结果保存在数据库，不需要独立 JSON 清单或后台代理。重新准备同一关系会原子替换旧记录；当前不支持 dropped column。实验期间保证映射关系不变，不在每次查询重复检查所有文件。

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
Guest PG：调用 SNIA HYBRID batch → 多分支 LOAD → Scan → Filter → Aggregate.sink
Guest PG：调用 SNIA SERIAL batch → Aggregate.source → Sort → Limit
Guest PG：接收少量最终结果 → 转换 Datum → 返回客户端
```

箭头中的 Scan/Filter/Aggregate/Sort/Limit 是设备 SO，运行在 FEMU CU；PG backend 仅编排请求，不扫描 heap、不计算聚合、不排序。原始 heap 通过 NVM→AFDM 内部路径读取，不经过 Guest 应用缓冲。列批次保留在 AFDM，哈希表等私有状态放在受限 workmem。借鉴 DuckDB 的所需列投影、selection vector、局部聚合后合并及公共子表达式复用；当前表达式循环没有显式 SIMD，不能把批处理直接等同于 SIMD。

PG 扩展直接链接 `libsnia`，在当前 backend 中调用 SNIA；没有查询辅助进程、sudo 调用、socket 或代理。安装时，udev 规则让 femu 用户可打开 /dev/nvme0 和 /dev/nvme0n1，postgres 的 CAP_SYS_ADMIN 满足 Linux vendor NVMe passthrough 的检查。PG 仍以 femu 用户运行，不以 root 启动。

取消在 batch 边界处理：SNIA 调用期间暂缓 PG 中断抛出，避免从 C++ 调用或批次线程中直接 longjmp；观察到取消后先释放设备状态、AFDM、batch 和程序，再由 PG 报告原始中断。错误路径执行同样的资源回收。

PG 查询设备 FDM/CU 容量，通常按4MiB块和最多4个 CU 分配独立分支；容量不足会减少分支或块大小。当前程序索引501–506由整个查询持有 namespace 文件锁，设备查询串行进入；这是单查询性能实验的实现，尚未支持多 PG 会话共享程序目录。

EXPLAIN ANALYZE 会显示投影列数、设备 CU 数、扫描/匹配行数、内部读取量、AFDM 用量及 workmem 峰值。计时包含程序加载、SNIA 编排与回收，不只统计设备计算。具体类型和规模限制见[设备算子说明](../../csd-programs/pg_sina_csd/README.md)。
