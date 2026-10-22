# pg_sina_csd

这是只读实验用 PostgreSQL 12 扩展。Guest 中的 PG 负责解析 SQL、规划和调用 SNIA；容器中编译的设备 SO 由 FEMU CU 执行扫描、表达式、连接、聚合和排序。原始 Heap 和中间结果不传回 Guest 执行器，只有最终结果转换为 PG Datum。

## 计划从哪里来

`pg_sina_csd.exe_path=host` 使用原生 PG。设置为 `csd` 后，扩展先进行保证语义的相关子查询去关联，再让 PG 正常选择计划，最后将它选中的物理 Plan 转换为设备执行树。连接的左右输入和树形结构来自 PG，不再使用扩展原来的贪心连接排序，也没有针对 Q13、Q18 的专用计划。

转换会删除下游不需要的物理扫描输出列，并同步调整 Sort、Aggregate 的列位置。PG 的部分/最终聚合合并为设备端的一次聚合；Gather 不再启动 Guest worker。参数化 B-tree Nested Loop 在原位置转换为设备 Index Join；其他等值连接使用 Hash Join 或 Merge Join。Hash build 超过设备配额时，保留已经消费的输入并改用外部归并，不重新扫描 Heap。

支持的节点包括投影/过滤、INNER/LEFT/SEMI/ANTI 等值连接、SUM/AVG/COUNT/MIN/MAX、COUNT DISTINCT、HAVING、Sort、Limit/Offset、派生表、共享 CTE、标量 InitPlan 和整数 IN/NOT IN 集合。相关标量聚合目前仅将严格 WHERE 条件中的 SUM/AVG/MIN/MAX 按整数等值关联键去关联；COUNT 的空输入、OR 等不安全形态保留 PG 执行。不能转换的计划整体回退原生 PG。`auto` 尚未实现，不接受该配置值。

这里的“设备执行树”只是 `include/plan.h` 定义的本应用参数，由 `pg-sina-plan.so` 解释；FEMU 和 SNIA 不解释 SQL，也没有新增设备 runtime。

```text
Guest PG：SQL → 安全去关联 → PG 选择 Plan → 转换 → CustomScan
Guest PG：读取预先准备的 extent，直接调用 libsnia
CSD：按计划读取 Heap/索引 → 连接/聚合/排序 → 分批最终结果
Guest PG：最终结果 → Datum → 客户端
```

通用树目前在一个 CU 内执行，扫描输出是列批次，树内中间值通过精确的 128 位单元传递，树内组合仍按行拉取。它尚未继承原来所有向量化和多 CU 优化。单表简单聚合（包括 Q1、Q6）继续使用已有的最多四槽列向量流水线；该快路径不改变任何连接顺序。不能把通用路径描述为已全部向量化或并行。

## 编译、安装和准备

先完成 Guest 的 SNIA/PG 安装及 NVMe 权限配置，扩展直接链接 `libsnia`。PG 不为查询启动辅助进程、代理或守护进程。实验 Guest 的 postgres capability 和设备访问权限由安装脚本配置。

```sh
# 仓库主机：构建设备 SO、Guest 扩展、工具并安装
bash tests/pg-sina-csd-test.sh build
# Guest 也可以单独构建、安装扩展
cmake -S /home/femu/apps/pg_sina_csd -B /home/femu/opt/pg_sina_csd/build \
  -DPG_CONFIG=/home/femu/opt/postgres/release/bin/pg_config -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /home/femu/opt/pg_sina_csd/build -j2
cmake --install /home/femu/opt/pg_sina_csd/build
```

在 `tpch_s1` 中准备一次只读映射：

```sql
CREATE EXTENSION IF NOT EXISTS pg_sina_csd;
-- 只有尚未冻结的实验表需要先制作冻结镜像：
-- VACUUM (FREEZE, ANALYZE);
SELECT sina_csd.prepare(c.oid)
FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
WHERE n.nspname='public' AND c.relkind='r';
LOAD 'pg_sina_csd';
SET pg_sina_csd.exe_path=csd;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT count(*) FROM lineitem;
```

`prepare` 由 PG superuser 调用，检查 visible/frozen、schema、relfilenode、Heap/索引文件和 FIEMAP，将 extent 转换为 namespace LBA。支持跨 1 GiB 文件段和数据分区起始偏移；准备结果存入 `sina_csd.relations/columns/indexes/extents`。期间没有写请求，不需要守护会话。重启 FEMU/容器、重新加载数据库文件、REINDEX 或文件布局变化后必须重新准备，不能继续使用旧映射。

## 访问路径和内存

设备可在连接树任意位置使用参数化整数前导键 B-tree 等值查找，支持复合索引的前导键，剩余条件仍在设备判断。一次最多收集 64 个外侧键；需要保留外侧顺序时按单键推进。缺页时只将页号写入控制 AFDM，PG 根据 extent 提交内部 NVM→AFDM LOAD。索引页和 Heap Tuple 仍在设备解析；没有实现真正的 Index Only Scan。

独立、非参数化的 Index Scan/Index Only Scan 当前转换为按条件扫描 Heap；需要索引顺序时另加设备排序。没有 Bitmap Scan、通用索引范围游标、部分/表达式索引或 PG13+ posting list 支持。

```sql
SET pg_sina_csd.max_memory='64MB'; -- 查询设备工作内存上限，最低 4MB
SET pg_sina_csd.profile=on;       -- 开发时开启，默认 off
```

工作内存来自共享 `libcs-workmem`，查询根 context 和子排序/索引 context 同时记账，总量不超过查询设置和物理池容量。不是为每个节点各给一个完整的 `max_memory`。FDM 只放页输入、列批次、字符串交接和最终输出；Hash 表、run 描述和游标来自 workmem。排序优先保留内存 run，分配下一段将超过配额时请求 spill，而非等待某个固定的空闲百分比。

spill 使用现有 SNIA batch 的 NEED_STORE/NEED_LOAD/NEED_FREE 重入协议，经设备内部 AFDM↔NVM 短路径存取临时 token。多路归并按缓存预算分轮合并；输入缓存不足时先 spill 驻留 run，为读缓存留出空间。查询正常完成、提前 Limit、取消和错误都要回收工作内存，关闭本查询 SNIA 会话时回收剩余临时 token。

设备程序索引 510 供通用树使用，旧快路径使用 501–509；当前查询持有 namespace 文件锁，尚未支持多个 PG backend 同时卸载。SNIA 不能强制中断正在执行的 CSF，取消要等待当前请求完成再回收。

## 精度和回退范围

输入限定为 PG12/x86-64/8 KiB、全部 visible/frozen 的不可变 Heap。支持整数、date、bool、有限 numeric 和本实验字符串。Heap numeric 需要至多两位小数且转换后可表示为 int64；后续算术/聚合保持 int128、NULL 和 PG numeric 显示精度/舍入。字符串排序要求可按字节比较的 C/C.UTF-8 collation，运行期字符串字典仍有容量限制。

浮点聚合和返回 bpchar 的 MIN/MAX 保留原生 PG，避免浮点舍入或去空格字典改变结果。最低 4MB 是配置下限，不表示所有复杂计划都能在 4MB 中容纳节点控制区及最小归并缓存。

计划时不支持的类型/表达式回退 PG；运行时越界、工作内存不足或数据超出设备表示范围会明确报错，不伪装成已经完成的查询。当前覆盖的是仓库 TPC-H 查询及回归用例，不是完整 PostgreSQL SQL 语法。

## 测试和性能解释

```sh
bash tests/pg-sina-csd-test.sh prepare
bash tests/pg-sina-csd-test.sh check   # 内部读取与 PG COUNT 对照
bash tests/pg-sina-csd-test.sh plan    # 原生 Plan、子查询和 NULL 等回归
# Guest：逐条冷启动核对并计时，省略编号时为全部 22 条
sh /home/femu/scripts/pg-sina-plan-bench.sh 7 9 13
```

每条 host/CSD 查询都由 `scripts/guest/cold-query.sh` 停止 PG、sync、清除 Guest page cache 并重启 PG；不重启 FEMU，以免丢失 RAM-backed NVM。主机与设备性能测试不能并发运行。

通用树 EXPLAIN 显示来自 PG 的连接树、扫描行数、内部读取和 spill 字节数；开启 profile 后增加整个 CSF 的 CPU/墙钟/调用次数。该累计 CPU 不等于查询延迟，通用树计时也不是逐节点独占 CPU。单表快路径仍提供已有的逐算子计时。

Q17、Q20 的原始 host 相关子查询可能超过脚本的 120 秒上限。此时只用 `tests/reference/` 中独立写出的等价 PG 查询核对结果，CSV 的 `host_ms` 留空、`host_timed_out=1`，参考查询耗时单列为 `reference_ms`。CSD 始终接收原始 SQL，不能将参考 SQL 的耗时当成原始 host 延迟。

原生 Join 顺序避免了旧实现按多个 build 组合重扫大表，但不保证单 CU 通用树比 PG 更快；归并、行式中间值及设备请求次数仍需分别分析。具体实测和历史记录见 [开发记录](doc/development.md) 与 [性能记录](doc/performance.md)。设备入口与参数见 [设备算子说明](../../csd-programs/pg_sina_csd/README.md)。
