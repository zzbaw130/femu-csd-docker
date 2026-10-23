# pg_sina_csd

这是只读实验用 PostgreSQL 12 扩展。Guest 中的 PG 负责解析 SQL、规划和调用 SNIA；容器中编译的设备 SO 由 FEMU CU 执行扫描、表达式、连接、聚合和排序。原始 Heap 和中间结果不传回 Guest 执行器，只有最终结果转换为 PG Datum。

## 计划从哪里来

`pg_sina_csd.exe_path=host` 使用原生 PG。设置为 `csd` 后，扩展先进行保证语义的相关子查询去关联，再让 PG 正常选择计划，最后将它选中的物理 Plan 转换为设备执行树。连接的左右输入和树形结构来自 PG，不再使用扩展原来的贪心连接排序，也没有针对 Q13、Q18 的专用计划。

转换会删除下游不需要的物理扫描输出列，并同步调整 Sort、Aggregate 的列位置。PG 的部分/最终聚合合并为设备端的一次聚合；单个 SUM/COUNT/AVG 可用有界 Hash Aggregate，支持多键、字符串和聚合参数表达式，输入可来自扫描或连接；超限时归并 SUM/count 部分状态。原计划利用分组排序提供 ORDER BY 时，将该排序移至聚合后，按聚合输出列的位置保留顺序；符合条件的 PG HashAggregate 保留 Hash 算法。Gather 不再启动 Guest worker，扫描内存预留使用 PG 的全表过滤后行数估计，避免把单 worker 的估计误作总量。参数化 B-tree Nested Loop 在原位置转换为设备 Index Join；其他等值连接使用 Hash Join 或 Merge Join。INNER Hash build 扩容失败时，保留已经消费的输入，按键分区落盘并继续 Hash；构建侧结束、子节点释放空间后，若实际行数对应的表能驻留，则直接多 CU 扫描 Probe 侧，否则共享当前分区的表并动态领取 Probe 临时块；其他形态保留外部归并，不重新扫描 Heap。每个 CU 的重入 spill 使用独立的 64 KiB AFDM，AFDM 总预算不增加。

支持的节点包括投影/过滤、INNER/LEFT/SEMI/ANTI 等值连接、SUM/AVG/COUNT/MIN/MAX、COUNT DISTINCT、HAVING、Sort、Limit/Offset、派生表、共享 CTE、标量 InitPlan 和整数 IN/NOT IN 集合。相关标量聚合目前仅将严格 WHERE 条件中的 SUM/AVG/MIN/MAX 按整数等值关联键去关联；COUNT 的空输入、OR 等不安全形态保留 PG 执行。不能转换的计划整体回退原生 PG。`auto` 尚未实现，不接受该配置值。

在 PG 计划上只做满足语义条件的局部优化：不要求输出有序的等值连接采用 Hash；只返回唯一键的连接，可将键集合过滤下推到分组之前；按左侧主键分组的 LEFT JOIN COUNT 可先统计右表，再连接计数并把未匹配值补为零。COUNT DISTINCT 在精确键投影下先对“分组键＋参数”去重，再统计非 NULL 参数，复用现有有界 Hash Aggregate 和 spill。它们都不依赖 TPC-H 查询编号。覆盖索引支持前两个整数列；无需索引顺序时，多个 CU 动态领取物理叶页范围，有序消费者仍沿叶链读取。

这里的“设备执行树”只是 `include/plan.h` 定义的本应用参数，由 `pg-sina-plan.so` 解释；FEMU 和 SNIA 不解释 SQL，也没有新增设备 runtime。

通用树中可并行的 Heap 顺序扫描由 PG 按当前空闲槽动态分配互不重叠的输入块，通过现有 batch 提交 `LOAD→计算`。计算任务从 CompValue 返回“有输出/输入块耗尽/worker 完成”，PG 无需逐块读取控制记录或写回 ready。下游尚未消费输出缓冲时不加载下一块；共享 Hash/聚合表需要扩容时仍保留原有暂停边界。索引读取、临时分区读取及串行路径继续按需返回页请求。各 worker 之间可并行 LOAD/计算，同一个 worker 仍先 LOAD 后计算，本次没有增加双缓冲预读。

开启 `pg_sina_csd.profile=on` 后，通用执行树的 EXPLAIN ANALYZE 还输出 `CSD Pipeline <节点号>`：每个并行输入流水线的轮次、墙钟、协调端 CPU、worker 累计 CPU、平均计算 CU 数和 CPU 利用率。墙钟包含该流水线输入及消费期间的内部 LOAD/调度等待；平均 CU 数为两端 CPU 之和除以墙钟，利用率再除以设备 CU 数，不能把峰值 4 当作全程满载。Hash 构建/Hash 聚合协调端 CPU 单独记录，扣除已计时的子扫描、排序和嵌套阻塞计算，仍包含协调端接收输入行的处理成本。计时在 CSF、阻塞调用及流水线边界进行，普通输入行不逐条读时钟；关闭 profile 时不采集，完成后只额外读取一次阶段记录。`sink` 是该流水线 worker 写入共享 Hash/聚合表的累计 CPU，已经包含在 `workers` 中；`sink_rows` 是直接在 worker 消费、无需交回协调端逐行处理的行数。

```text
Guest PG：SQL → 安全去关联 → PG 选择 Plan → 转换 → CustomScan
Guest PG：读取预先准备的 extent，直接调用 libsnia
CSD：按计划读取 Heap/索引 → 连接/聚合/排序 → 分批最终结果
Guest PG：最终结果 → Datum → 客户端
```

通用树按 PG 选定的连接树划分可并行的 Scan→Filter→Join Probe 流水线，包含 InitPlan 和 Hash Build 输入中的流水线。Guest 根据设备 CU 数和 FDM 容量分配至多 64 个执行槽，通过普通 SNIA 请求提交；各设备任务从共享游标动态领取数据块，完成的槽立即继续推进，不等待固定四分支轮次。并行构建输入由各 CU 按批预留紧凑行空间、原子更新桶链，Hash 表只构建一次并共享。支持的 Hash 聚合/去重也由各 CU 直接更新同一张有界表，只锁当前桶；AVG 保留精确 sum/count，DISTINCT 在全局按完整键去重。表满时保留未消费的 AFDM 行，等当前 CU 任务均停止使用该表后，由协调端扩容或沿现有路径 spill；不会按 CU 数复制完整表。Sort 和不支持该方式的聚合保留原阻塞边界；不同流水线依次复用缓冲。扫描块根据 FDM 容量在 4/2/1 MiB 中选择，优先保留 CU 并行度；小表只分配其实际文件大小的缓冲。工作内存仍受同一个查询根 context 限制。

扫描输出列批次，复用已有 Filter 的 selection、SIMD 和复杂条件短路；价格等 numeric 列仅为存活行解码。内部批次只存当前流水线需要的列，字符串已有值的查找不再争抢插入锁。索引自连接可以复用同一个执行槽仍保留的原始扫描块，块外页按需 LOAD。树内中间值仍为精确 int128，连接/投影/聚合的组合仍有按行处理部分；不表示所有节点已完全向量化。Q1/Q6 继续使用已有的单表列向量快路径。

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

设备可在连接树任意位置使用参数化整数前导键 B-tree 等值查找，支持复合索引的前导键，剩余条件仍在设备判断。一次最多收集 64 个外侧键；需要保留外侧顺序时按单键推进。缺页时只将页号写入控制 AFDM，PG 根据 extent 合并连续目的缓冲和相邻 LBA，通过 SERIAL batch 提交内部 NVM→AFDM LOAD→继续计算；不同执行槽可以独立推进。SEMI/ANTI 在确认匹配后立即停止该键的查找。索引页和 Heap Tuple 仍在设备解析。

需要顺序的、已准备映射的 PG12 整数前导键 B-tree 支持正向叶页遍历，按索引顺序回表并直接交给有序聚合，不再补 Heap 排序。索引页做有界预读；PG 相关性统计大于 0.9 时，Heap 也做有界正向预读。游标受 workmem 约束，页留在原 AFDM 缓存中。其他独立扫描仍可能降为 Heap；反向有序扫描仍补排序。Index Only Scan 当前仅支持所需列完全由已映射索引的整数前导键覆盖的形态，直接读取索引键，不回表；其他形态回退原路径。仍没有 Bitmap Scan、通用索引范围游标、部分/表达式索引或 PG13+ posting list 支持。

```sql
SET pg_sina_csd.max_memory=0;      -- 默认值，自动使用设备 workmem 总容量
-- SET pg_sina_csd.max_memory='16MB'; -- 可选：进一步缩小查询预算，非零值最低 4MB
SET pg_sina_csd.profile=on;       -- 开发时开启，默认 off
```

扩展在每个 PG backend 首次规划已映射设备的 CSD 查询时，读取并保存该设备的 CU 数、FDM 容量及 workmem 总容量，供规划和两条执行路径共用。同一 backend 后续查询复用这些容量；设备重新配置后建立新 PG 连接即可重新发现，不需要修改扩展源码。CU/FDM 通过 `csQueryDeviceProperties` 获取；当前 SNIA 资源结构没有本项目分配器的 workmem 字段，因此调用 `pg-sina-plan.so` 中零参数的 `pg_sina_workmem_capacity`，通过 64 位 CompValue 返回 `cs_work_get_stats().capacity_bytes`。这是一条普通设备计算请求，不创建辅助进程，也不读取 Guest 的内存配置。

自动模式下，CSD 查询总内存预算等于设备 workmem；显式设置非零 `max_memory` 时取两者较小值。例如设备为 32 MiB，即使设置 64 MiB，查询也只能使用 32 MiB。EXPLAIN 显示 `CSD Device CUs`、`CSD Device Workmem` 和 `CSD Query Memory Limit`，普通 EXPLAIN 即可核对。查询的所有并行 CU 和阻塞节点共享该总预算。实际执行槽数还受 FDM、内存及数据量约束；支持 1–64 个 SQL 执行槽，与当前 FEMU 允许的 CU 范围一致，使用 `min(设备 CU 数, 64)` 作为初始候选，按实际缓冲和算子需要减少。完成标记采用 64 位位图，64 槽完整掩码单独处理，避免移位溢出。设备 CU 和 workmem 增加后，新连接自动重新编排，不需要修改扩展源码。

Hash Probe 和索引游标仅为实际用到的槽分配，避免扩大槽容量后每个节点都预留 64 份游标；共享索引状态的预算按槽数增加。Sort source 归并时会跳过未生成 run 的空闲槽。

工作内存来自共享 `libcs-workmem`，查询根 context 和子排序/索引 context 同时记账，总量不超过查询设置和物理池容量。不是为每个节点各给一个完整的 `max_memory`。FDM 只放页输入、列批次、字符串交接和最终输出；Hash 表、run 描述和游标来自 workmem。排序优先保留内存 run，分配下一段将超过配额时请求 spill，而非等待某个固定的空闲百分比。

spill 使用现有 SNIA batch 的 NEED_STORE/NEED_LOAD/NEED_FREE 重入协议，经设备内部 AFDM↔NVM 短路径存取临时 token。多路归并按缓存预算分轮合并；输入缓存不足时先 spill 驻留 run，为读缓存留出空间。查询正常完成、提前 Limit、取消和错误都要回收工作内存，关闭本查询 SNIA 会话时回收剩余临时 token。

设备程序索引 510 供通用树使用，旧快路径使用 501–509，511 仅在首次发现资源时短暂加载容量查询函数并随即卸载；当前查询持有 namespace 文件锁，尚未支持多个 PG backend 同时卸载。SNIA 不能强制中断正在执行的 CSF，取消要等待当前请求完成再回收。

## 精度和回退范围

输入限定为 PG12/x86-64/8 KiB、全部 visible/frozen 的不可变 Heap。支持整数、date、bool、有限 numeric 和本实验字符串。Heap numeric 需要至多两位小数且转换后可表示为 int64；后续算术/聚合保持 int128、NULL 和 PG numeric 显示精度/舍入。字符串排序要求可按字节比较的 C/C.UTF-8 collation，运行期字符串字典仍有容量限制。

浮点聚合和返回 bpchar 的 MIN/MAX 保留原生 PG，避免浮点舍入或去空格字典改变结果。最低 4MB 是配置下限，不表示所有复杂计划都能在 4MB 中容纳节点控制区及最小归并缓存。

计划时不支持的类型/表达式回退 PG；运行时越界、工作内存不足或数据超出设备表示范围会明确报错，不伪装成已经完成的查询。当前覆盖的是仓库 TPC-H 查询及回归用例，不是完整 PostgreSQL SQL 语法。

## 唯一验收入口

TPC-H 验收只读取仓库 `tests/guest/queries/1.sql` 至 `22.sql` 及对应 `.explain.sql`。Guest 挂载路径为 `/home/femu/tests/queries`。扩展目录不再保存另一套 TPC-H、参考改写 SQL、临时 timed.sql 或 SQL fixture。开发中的小 SQL 仅放在临时目录定位问题，不作为验收依据；设备 C/C++ 单元测试和 `tests/heap_layout.c` 保留。

```sh
# 仓库主机：八表映射已准备时，不必每次重新 prepare
bash tests/pg-sina-csd-test.sh tpch
# Guest：默认全部 22 条，也可以只测指定编号
sh /home/femu/tests/tpch-test.sh 7 9 13
```

只保留标准规定的 Q2=100、Q3=10、Q10=20、Q18=100、Q21=100 行上限，其余查询不额外添加 LIMIT。查询参数、子查询写法和 Q15 的 CREATE VIEW/SELECT/DROP VIEW 均保持该目录原样。host 与 CSD 接收同一个文件，超时或错误直接记录为失败，不用手写参考 SQL 替代。

每次测量前 `scripts/guest/cold-query.sh` 停止 PG、sync、清除 Guest page cache 并重启 PG；不重启 FEMU，以免丢失 RAM-backed NVM。profile 默认关闭。Q15 的计时是原始脚本中各 SQL 耗时之和，包含创建/删除视图，host/CSD 规则相同。

结果保存到 Guest `/home/femu/results/tpch.*`，对应主机 `results/tpch.*`：`timings.csv` 分别记录完整结果是否一致、是否实际卸载和性能状态。较慢的 CSD 查询会另行冷启动，读取相同查询的 explain 文件，仅增加 ANALYZE/计时选项采集设备 CPU、内读、工作内存和 spill；该诊断耗时不混入关闭 profile 的性能比较。

**结果一致不代表性能验收合格。** 只有实际设备工作内存不足且必须频繁 spill 造成的延迟增加可以接受。单 CU/向量化不足、重复扫描或复制、碎片化页请求、保守节点配额等实现问题造成的退化均不合格。脚本将较慢样本标记为 `needs_analysis` 并返回非零；必须结合实际占用和计时判断，不能看到 spill 就自动认定内存不足。单次测量不用于宣称稳定加速比。

通用树 EXPLAIN 显示来自 PG 的连接树、实际 worker 数、并行流水线次数、顺序扫描行数、索引 Heap Tuple 数、内部读取、工作内存峰值及 spill 字节。开启 profile 后增加整个 CSF 的累计 CPU/墙钟/调用次数、实际同时执行的 CU 峰值，以及扫描解码、向量过滤和 Sort sink/source 的 CPU 时间。CPU 是各 CU 的累计值，可能大于查询墙钟；扫描/过滤/排序已包含在 CSF 总 CPU 中，不能再次相加。`CSD LOAD Preparation` 仅为 PG 构造请求的时间，异步 LOAD 的等待不计入该字段。单表快路径保留已有的逐算子计时。

通用树的 profile 同时记录 PG 调度线程的墙钟：`CSD Host Completion Copy` 是每次请求完成后同步读取 AFDM 控制记录及结果头的时间，`CSD Host Submit` 是准备和提交后续请求的时间。其中 `Queue`、`Control Write` 及已有的 `LOAD Preparation` 均包含在 Submit 中，不能再次相加。`Poll` 是检查事件的时间，`Idle Sleep` 是没有可推进任务时的休眠时间。`Pending Requests Sum` 从请求提交计到 PG 观察完成，跨并发请求求和，包含设备执行、I/O 和主机调度等待，不是查询延迟或纯 I/O 时间；Submissions/Completions 用于核对完成次数。关闭 profile 时不采集这些计时。

`CSD Host Assigned LOADs` 记录提前分配的顺序输入块数；开启 profile 时，`Completion Reads` 和 `Control Writes` 分别记录调度期间实际进行的控制记录读写次数，不包含初始化、结束清理和最终字符串结果复制。

设备入口见 [设备算子说明](../../csd-programs/pg_sina_csd/README.md)。新验收结果见 [性能记录](doc/performance.md)，旧记录仅为历史，不作为当前验收依据。
