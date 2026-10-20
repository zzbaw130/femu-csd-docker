# pg_sina_csd：基于 SNIA 的 PostgreSQL CSD 扩展

分支 `pg_sina_csd_dev` 从标签 `sina-femu-csd` 开始。Git 标签、分支和扩展名按项目约定保留 sina 拼写；调用的标准及库名称为 SNIA / snia。

目标是把 PG 执行计划转换为 SNIA batch，由 Guest 调度 CSD 算子。只读实验使用与原生 PG 相同的全表 visible/frozen heap；实验前准备一次 schema、extent 和 LBA 映射，后续持续使用。文件、schema 或设备变化以及重启后重新准备，不设置守护连接。

当前完成阶段0：扩展安装、静态映射和设备 heap 行数校验。尚未接入 planner/CustomScan，普通 SQL 仍由原生 PG 执行。

## 执行路径与冷启动

```sql
LOAD 'pg_sina_csd';
SET pg_sina_csd.exe_path=host; -- 默认：PG原生路径
SET pg_sina_csd.exe_path=csd;  -- 优先卸载支持的子计划，不支持时保留PG执行
```

auto将在成本模型实现后开放，目前只接受host/csd。切换后应重新规划语句；不要复用切换前已经生成的prepared plan。

TPC-H测量每条语句之前使用Guest的cold-query.sh：停止PG清空shared buffers，sync并drop_caches，再启动PG（测试期间autovacuum=off）。host和csd同样处理；不是只清除OS缓存。脚本在Guest执行：

```sh
/home/femu/scripts/cold-query.sh host /path/to/query.sql
/home/femu/scripts/cold-query.sh csd /path/to/query.sql
```

每个文件只放一条被测查询；EXPLAIN ANALYZE也会实际执行，应单独冷启动。普通PG服务重启不改变冻结表文件，可复用映射；Guest/FEMU重启或恢复/改动文件后重新prepare。

## 执行位置与目录

| 路径 | 用途及执行位置 |
| --- | --- |
| src/extension.c | Guest PG 扩展入口与版本函数 |
| src/prepare.c | Guest PG 内检查 visible/frozen，checkpoint，保存 schema/extent/LBA |
| src/check.c | Guest 诊断工具，链接 libpq 和 libsnia；提交 LOAD→heap_count，与原生 COUNT 比较 |
| include/heap_count.h | Guest 与设备共同使用的计数结果布局 |
| sql/ | 扩展安装 SQL；sina_csd schema 保存准备结果 |
| tests/ | PG12 实际页布局与准备功能测试 |
| doc/ | 分阶段开发记录 |
| ../../csd-programs/pg_sina_csd/ | 容器编译、FEMU CU 执行的设备算子 |

普通应用仍只包含 cs.h 并链接 -lsnia。这里使用 PostgreSQL 头文件和 libpq，是因为本项目本身是 PG 扩展及其诊断工具，而不是 SNIA 新的使用要求。

## 编译与准备

Guest 已安装 PG12.22、SNIA，且 /data 已挂载。可用 scripts/guest/init-csd-env.sh 准备分区与 SNIA；首次或标签不匹配会清除模拟盘数据，请在恢复数据库前运行。

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

工具启动时检查 Guest boot ID 和 relfilenode；设备或文件变化后仍须主动重新准备。当前固定程序索引500，单次独占使用；不是跨PG backend程序目录管理方案。诊断工具通过sudo访问NVMe，不提升PG服务权限。后续SQL卸载阶段还需落实PG请求到NVMe的权限路径。

PG 会另行执行 COUNT 校验结果，因此本工具用于正确性验收，不是 SQL 性能基准。后续阶段与验收记录见[开发记录](doc/development.md)。
