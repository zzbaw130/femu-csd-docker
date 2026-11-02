# SQLerate（pg_sina_csd）

这是只读实验用 PostgreSQL 12 扩展。Guest 中的 PG 负责解析 SQL、规划和调用 SNIA；容器中编译的设备 SO 由 FEMU CU 执行扫描、表达式、连接、聚合和排序。原始 Heap 和中间结果不传回 Guest 执行器，只有最终结果转换为 PG Datum。

## 计划从哪里来

`pg_sina_csd.exe_path` 提供五种执行模式：

| 模式 | 执行位置 | HPCP |
| --- | --- | --- |
| `host-only` | 原生 PG | 关闭 |
| `filter-only` | 支持的基表扫描及过滤在 CSD，其余在 PG | 关闭 |
| `csd-without-hpcp` | 尽可能完整卸载，未支持的节点在 PG | 关闭 |
| `csd-with-hpcp` | 同上 | 开启 |
| `SQLerate` | 将 HOST/CSD 候选加入 PG 的扫描、连接及上层路径搜索，选择混合计划 | CSD 子树开启 |

HPCP 全称 **Host Page Cache Population（主机页缓存填充）**。它把设备读入的数据填充到 Guest 文件页缓存；两种 CSD 模式目前都从内部 NVM 读入 FDM，不会因为命中主机缓存就默认上传。SQLerate 分别观测 PG shared_buffers 和文件页缓存：前者命中免除读盘及内核到 PG 缓冲区的复制，后者命中仍保留复制成本；CPU、谓词和索引查找成本继续保留。这里只改变估价，HPCP 仍填充文件页缓存。成本公式、参数及当前执行边界见 [规划说明](doc/planning.md)。

DCP（Database Cache Population，数据库缓存填充）把同一份设备接收数据填入 PG 的 `shared_buffers`，默认关闭。`pg_sina_csd.hpcp` 默认开启，作为原有 HPCP 模式的允许开关；关闭后，即使选择 `csd-with-hpcp` 或 `SQLerate` 也不填文件页缓存。`pg_sina_csd.dcp` 独立控制 DCP，在产生设备读取的执行路径上生效，`host-only` 不产生设备填充。

```sql
SET pg_sina_csd.exe_path = 'csd-without-hpcp';
SET pg_sina_csd.hpcp = off;
SET pg_sina_csd.dcp = on;
```

同时填充两层缓存时，选择 `csd-with-hpcp` 并开启两个布尔选项。设备只做一次 DMA；DCP 模式由执行 SQL 的 PG backend 消费接收槽，按原始8KiB页复制到缺失的数据库缓冲，随后按需把同一槽交给内核填文件缓存。现有有效页不覆盖，导入页不标脏、不产生 WAL；不调用 `pg_prewarm(buffer)` 重新读文件。该实现针对已准备映射的只读镜像，不新增进程或PG worker。

两层同时开启时，超过 `shared_buffers/4` 的大表顺序扫描采用 PG 的 `BAS_BULKREAD` 缓冲策略及256KiB驻留目标（当前32页），避免大表流把数据库缓冲占满并挤掉刚填入的文件缓存。达到该实际驻留目标后停止这张表的可选 DCP 导入，其余数据继续交给 HPCP，不将每次环内替换当作新增驻留并反复消耗预算；小表、索引探测及其 Heap 读取、只开 DCP 时保留普通分配与原目标。驻留量仍来自页面位图，不是累计填页数。DCP 配额耗尽或所有关系已达到目标后跳过逐页处理，并在释放当前槽后通过 FEMU 私有 `csConfig` 约定将同一接收环交回 SNIA 已有 HPCP 线程；不重新分配接收内存、不重绑 DMA，也不增加辅助进程。查询开始时已无须 DCP，则直接使用该后台线程。这是本项目的可选缓存准入策略，未修改 PG 原生扫描或缓冲替换算法。

无连接、无外部流式阻塞的融合扫描聚合路径（包括Q6）使用SNIA完成callback领取下一输入块并续交；PG backend只等待整条扫描结束并导入DCP，不负责逐块推进。每个执行槽独占自己的FDM和批次，回调不调用PG API；此时DCP可连续导入完整4MiB槽，不使用256KiB/250µs预算。仍需PG协调的其他路径继续优先交付结果和续交，等待时每次最多导入256KiB、按约250µs预算在完整8KiB页之间让出执行权。整个槽导入完才归还接收缓冲，不增加DMA命令；有填充工作时不额外休眠50µs。接收槽满或关闭会话时允许放弃尚未传输的数据，因此填充是尽力而为，不能把累计读取量当作缓存驻留量。首次写入 shared_buffers 可能产生大量匿名内存 minor fault，DCP 不保证加速当前 CSD 查询。profile 的 `SQLerate DCP` 单独报告接收块、导入/已有页、无效页、跳过字节、填充累计时间、单次处理上限实测、扩展额外缓冲查找（当前为0）/PG分配清零/复制耗时和 minor/major fault；EXPLAIN 显示 DCP 开关。PCIe填充成本进入既有模型，PG缓冲分配/复制的CPU尚未单独校准，当前实验宜用强制路径比较。

PG 集成保持非侵入式：不修改 PostgreSQL 源码，不替换运行时缓冲函数。DCP直接调用现有 `ReadBufferExtended(RBM_ZERO_AND_LOCK)`，不再先做一遍独立的缓冲哈希查找；已有页在PG原生pin和页锁保护下识别并跳过，不覆盖其内容。PG内部查找计入profile的 `allocate_ms`，`lookup_ms=0` 表示扩展的额外查找已删除。该接口的清零和首次物理页触碰仍保留，不能据此宣称消除了主要分配成本。

依赖更新后的 SNIA FEMU 插件及 [femu-cache 接收模块](../femu-cache/README.md)。仅检查开关隔离、原生PG读取导入页和取消恢复，可在Guest运行 `sh /home/femu/tests/database-cache-test.sh`，使用正式Q6。缓存驻留使用连接级页面位图，初始化及顶层ExecutorEnd刷新；规划阶段只读取记录。详见下文及doc/planning.md。

两种强制完整卸载模式保留 PG 选定的连接顺序。SQLerate 在 PG 枚举合法连接组合时加入设备子树，因此设备成本能影响连接顺序，HOST 上层也能根据设备输入重新选择算法；仍受 PG 的合法性、路径剪枝和 GEQO 范围限制。

去关联改写的是解析后的 `Query` 树，不修改 SQL 文件。SQLerate 同时保留原始 Query 的原生 HOST 计划、该 PG 优选树可转换的完整 CSD 计划，并与经过安全改写的 HOST/CSD 共享搜索结果比较。改写后的树同样允许全部由 HOST 执行；不把去关联当成仅供 CSD 使用的优化。`host-only` 始终是未经扩展改写的原生 PG 基线。`filter-only` 也保留原始 Query，仅转换支持的扫描叶节点。

`filter-only` 只解码、返回 HOST 后续节点需要的列，使用设备多 CU 扫描过滤，并保留 PG 原生并行聚合/连接。parallel-aware 顺序扫描通过 PG 共享计数器分配不同输入块，不重复返回整表。结果按实际列数打包，直接构造 PG Numeric；实现及针对性测量见 [filter-only 执行](doc/filter-only.md)。

转换会删除下游不需要的物理扫描输出列，并同步调整 Sort、Aggregate 的列位置。PG 的部分/最终聚合合并为设备端的一次聚合；SUM/AVG/COUNT/MIN/MAX 可共同使用有界 Hash Aggregate，支持多键、字符串和聚合参数表达式，输入可来自扫描或连接；各聚合分别保存值/count（SUM/AVG 保存和，MIN/MAX 保存极值），超限时归并这些部分状态；分组键数加两倍聚合数超过现有16列中间行容量时，保留原排序聚合。原计划利用分组排序提供 ORDER BY 时，将该排序移至聚合后，按聚合输出列的位置保留顺序；符合条件的 PG HashAggregate 保留 Hash 算法。Gather 不再启动 Guest worker，扫描内存预留使用 PG 的全表过滤后行数估计，避免把单 worker 的估计误作总量。参数化 B-tree Nested Loop 在原位置转换为设备 Index Join；其他等值连接使用 Hash Join 或 Merge Join。INNER Hash build 扩容失败时，保留已经消费的输入，按键分区落盘并继续 Hash；构建侧结束、子节点释放空间后，若实际行数对应的表能驻留，则直接多 CU 扫描 Probe 侧，否则共享当前分区的表并动态领取 Probe 临时块；其他形态保留外部归并，不重新扫描 Heap。每个 CU 的重入 spill 使用独立的 64 KiB AFDM，AFDM 总预算不增加。

支持的节点包括投影/过滤、INNER/LEFT/SEMI/ANTI 等值连接、SUM/AVG/COUNT/MIN/MAX、COUNT DISTINCT、HAVING、Sort、Limit/Offset、派生表、共享 CTE、标量 InitPlan 和整数 IN/NOT IN 集合。相关标量聚合目前仅将严格 WHERE 条件中的 SUM/AVG/MIN/MAX 按整数等值关联键去关联；COUNT 的空输入、OR 等不安全形态保留 PG 执行。不能转换的计划整体回退原生 PG。`auto` 尚未实现，不接受该配置值。

在 PG 计划上只做满足语义条件的局部优化：不要求输出有序的等值连接采用 Hash；只返回唯一键的连接，可将键集合过滤下推到分组之前；按左侧主键分组的 LEFT JOIN COUNT 可先统计右表，再连接计数并把未匹配值补为零。COUNT DISTINCT 在精确键投影下先对“分组键＋参数”去重，再统计非 NULL 参数，复用现有有界 Hash Aggregate 和 spill。它们都不依赖 TPC-H 查询编号。覆盖索引支持前两个整数列；无需索引顺序时，多个 CU 动态领取物理叶页范围，有序消费者仍沿叶链读取。

这里的“设备执行树”只是 `include/plan.h` 定义的本应用参数，由 `pg-sina-plan.so` 解释；FEMU 和 SNIA 不解释 SQL，也没有新增设备 runtime。

通用树中可并行的 Heap 顺序扫描由 PG 按当前空闲槽动态分配互不重叠的输入块，通过现有 batch 提交 `LOAD→计算`。计算任务从 CompValue 返回“有输出/输入块耗尽/worker 完成”，PG 无需逐块读取控制记录或写回 ready。下游尚未消费输出缓冲时不加载下一块；共享 Hash/聚合表需要扩容时仍保留原有暂停边界。索引和串行namespace读取继续按需返回页请求；独立worker可由回调续交，协调端仍在PG线程处理。临时token的LOAD/STORE继续由设备图内部推进。各 worker 之间可并行 LOAD/计算，同一个 worker 仍先 LOAD 后计算，本次没有增加双缓冲预读。

Heap 的 I/O 块与列向量容量分别计算：一个已加载块可包含多个至多 2048 行的向量，按整页填入并逐批交给下游，消费完后才释放该输入块。无需为整块数据的最坏行数预留列向量；`CSD AFDM` 显示本次申请的总字节数，`CSD Scan Vector Capacity` 显示 Heap 向量的行数上限，索引批次仍保持原容量。通用树的运行时字符串字典在查询 workmem 中按需增长，AFDM 只保留一页配置，不再清零大字典桶。增长前后的存储都计入同一预算，查询结束统一释放，已有字符串 ID 和字节引用保持有效。`CSD Dictionary Entries/Bytes` 报告实际内容；`CSD Dictionary AFDM` 只表示配置页。

异步 batch 使用 SNIA 标准 callback 返回状态和 CompValue。对提前安排的 Heap 输入，若当前块已耗尽、没有未消费输出且 worker 尚未结束，回调直接领取下一块，重新配置该槽的 `LOAD→计算` batch 并异步续交。输入缓冲仍每槽一份，不增加预读或双缓冲。索引及其他未提前分配输入的worker，回调读一次页请求元数据，按已准备的extent构建下一批LOAD→计算；计算参数标记输入已加载，不再单独写回ready。已有输出、需要共享表维护及阶段结束时仍交给PG协调端。回调仅使用普通内存分配、已有 extent 映射及 SNIA，不调用 PG API；PG 取消时先禁止续交，再等待所有在途回调结束后释放状态。

开启 `pg_sina_csd.profile=on` 后，通用执行树的 EXPLAIN ANALYZE 还输出 `CSD Pipeline <节点号>`：每个并行输入流水线的轮次、墙钟、协调端 CPU、worker 累计 CPU、平均计算 CU 数和 CPU 利用率。墙钟包含该流水线输入及消费期间的内部 LOAD/调度等待；平均 CU 数为两端 CPU 之和除以墙钟，利用率再除以设备 CU 数，不能把活跃 CU 峰值当作全程满载。Hash 构建/Hash 聚合协调端 CPU 单独记录，扣除已计时的子扫描、排序和嵌套阻塞计算，仍包含协调端接收输入行的处理成本。计时在 CSF、阻塞调用及流水线边界进行，普通输入行不逐条读时钟；关闭 profile 时不采集，完成后只额外读取一次阶段记录。`sink` 是该流水线 worker 写入共享 Hash/聚合表的累计 CPU，已经包含在 `workers` 中；`sink_rows` 是直接在 worker 消费、无需交回协调端逐行处理的行数。

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
SET pg_sina_csd.exe_path='SQLerate';
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT count(*) FROM lineitem;
```

`prepare` 由 PG superuser 调用，检查 visible/frozen、schema、relfilenode、Heap/索引文件和 FIEMAP，将 extent 转换为 namespace LBA。支持跨 1 GiB 文件段和数据分区起始偏移；准备结果存入 `sina_csd.relations/columns/indexes/extents`。期间没有写请求，不需要守护会话。重启 FEMU/容器、重新加载数据库文件、REINDEX 或文件布局变化后必须重新准备，不能继续使用旧映射。

HPCP 实验需先加载 Guest 内核模块并配置 `/dev/femu-cache` 权限，见 [HPCP 组件说明](../femu-cache/README.md)。PG 通过执行模式控制开关，不再要求在服务环境手工配置。普通 SNIA 应用仍可用 `SNIA_FEMU_HPCP=1`。扩展按预备 extent 注册表/索引文件，查询上下文结束或出错时关闭注册。显式 LOAD 和 CSF 内部 namespace 基表读都支持 HPCP，隐藏 spill 换入除外；不启动额外进程。

## 访问路径和内存

查询不持有 namespace 执行锁。每个 PG 后端使用独立的 CSF 程序编号段（原编号加 `BackendId * 16`）；连接级入口统一使用基础编号500–513；同时存活的子树复用同一入口，不再为子树另占程序编号。AFDM 和算子状态按执行独立分配、释放，避免不同连接覆盖彼此的程序和缓冲区。FEMU 的程序编号为 16 位，当前分段支持 BackendId 1–4063；并发请求仍受设备实际 AFDM/workmem 总容量约束，分配失败会返回错误，不会把整条查询串行化等待。

Heap 扫描从页内行指针直接取得各 tuple 的起点，不依赖前一个 tuple 的长度。读取块分配给不同 CU 并行解码；CU 内每批最多收集 64 个 tuple，按列定位字段、转换并连续写入列向量，参考 pg_duckdb 的批量 tuple 转换方式。变长字段之后的偏移仍依赖同一 tuple 的前置字段，各 tuple 各自保存偏移；这不表示 Heap 解码已全部使用 SIMD，也不在 CSF 内另起线程。

设备可在连接树任意位置使用参数化整数前导键 B-tree 等值查找，支持复合索引的前导键，剩余条件仍在设备判断。一次最多收集 64 个外侧键；需要保留外侧顺序时按单键推进。缺页时只将页号写入控制 AFDM，PG 根据 extent 合并连续目的缓冲和相邻 LBA，通过 SERIAL batch 提交内部 NVM→AFDM LOAD→继续计算；不同执行槽可以独立推进。SEMI/ANTI 在确认匹配后立即停止该键的查找。索引页和 Heap Tuple 仍在设备解析。

需要顺序的、已准备映射的 PG12 整数前导键 B-tree 支持正向叶页遍历，按索引顺序回表并直接交给有序聚合，不再补 Heap 排序。索引页做有界预读；PG 相关性统计大于 0.9 时，Heap 也做有界正向预读。游标受 workmem 约束，页留在原 AFDM 缓存中。其他独立扫描仍可能降为 Heap；反向有序扫描仍补排序。Index Only Scan 当前仅支持所需列完全由已映射索引的整数前导键覆盖的形态，直接读取索引键，不回表；其他形态回退原路径。仍没有 Bitmap Scan、通用索引范围游标、部分/表达式索引或 PG13+ posting list 支持。

```sql
SET pg_sina_csd.max_memory=0;      -- 默认值，自动使用设备 workmem 总容量
-- SET pg_sina_csd.max_memory='64MB'; -- 可选：进一步限制预算，实验至少使用 64MiB
SET pg_sina_csd.profile=on;       -- 开发时开启，默认 off
```

扩展在每个连接加载 C 库时完成设备初始化：通过 SNIA 发现当前实验的唯一 CSD namespace，打开设备，读取 CU/FDM 容量，下载并激活全部14个 CSF 入口，再通过 `pg_sina_workmem_capacity` 获取共享分配器总容量。连接正常退出时统一停用、卸载 CSF 并关闭设备。当前实验要求只有一个可发现的 CSD namespace，不增加额外设备路径配置。所有已映射表仍须属于这个 namespace。

`LOAD 'pg_sina_csd'` 会立即完成上述初始化；为了在建立连接时自动完成，可由管理员执行以下配置，新连接生效：

```sql
ALTER DATABASE tpch_s1 SET session_preload_libraries='pg_sina_csd';
```

如还使用 `auto_explain`，保留为 `'pg_sina_csd,auto_explain'`。不要使用 `shared_preload_libraries` 创建会被 fork 继承的 SNIA 连接和线程。PG 并行 worker 只注册扩展方法，不重复创建连接级设备资源。启用数据库级预加载前须先安装设备程序并配置设备权限。

planner 和执行器复用连接持有的设备句柄、CSF ID及资源快照，不再打开/关闭设备或下载/卸载程序。查询仅分配自己的 AFDM、batch、参数和执行状态，并在完成、取消或出错时回收；多个设备子树可调用同一 CSF，其状态仍由各自 AFDM 隔离。设备重新配置后建立新连接即可重新发现。CU/FDM 通过 `csQueryDeviceProperties` 获取；当前 SNIA 资源结构没有分配器 workmem 字段，因此零参数容量 CSF 通过64位 CompValue返回 `cs_work_get_stats().capacity_bytes`。不创建辅助进程。

开启 profile 时，初始化单独输出 `SQLerate connection init total_ms=... programs=14 ...`，不计入查询规划计时。可通过连接选项 `PGOPTIONS='-c pg_sina_csd.profile=on'` 在预加载前启用；已有冷启动脚本已处理。初始化仍有实际成本，论文应单列连接初始化时间，不能将计时前移解释为消除了冷启动总成本。

自动模式下，CSD 查询总内存预算等于设备 workmem；显式设置非零 `max_memory` 时取两者较小值。例如设备为 256 MiB，设置 64 MiB 后该查询只能使用 64 MiB。EXPLAIN 显示 `CSD Device CUs`、`CSD Device Workmem` 和 `CSD Query Memory Limit`，普通 EXPLAIN 即可核对。查询的所有并行 CU 和阻塞节点共享该总预算。实际执行槽数还受 FDM、内存及数据量约束；支持 1–64 个 SQL 执行槽，与当前 FEMU 允许的 CU 范围一致，使用 `min(设备 CU 数, 64)` 作为初始候选，按实际缓冲和算子需要减少。完成标记采用 64 位位图，64 槽完整掩码单独处理，避免移位溢出。设备 CU 和 workmem 增加后，新连接自动重新编排，不需要修改扩展源码。

Hash Probe 和索引游标仅为实际用到的槽分配，避免扩大槽容量后每个节点都预留 64 份游标；共享索引状态的预算按槽数增加。Sort source 归并时会跳过未生成 run 的空闲槽。

工作内存来自共享 `libcs-workmem`，查询根 context 和子排序/索引 context 同时记账，总量不超过查询设置和物理池容量。不是为每个节点各给一个完整的 `max_memory`。FDM 只放页输入、列批次、字符串交接和最终输出；Hash 表、run 描述和游标来自 workmem。排序优先保留内存 run，分配下一段将超过配额时请求 spill，而非等待某个固定的空闲百分比。

spill 使用现有 SNIA batch 的 NEED_STORE/NEED_LOAD/NEED_FREE 重入协议，经设备内部 AFDM↔NVM 短路径存取临时 token。多路归并按缓存预算分轮合并；输入缓存不足时先 spill 驻留 run，为读缓存留出空间。查询正常完成、提前 Limit、取消和错误都要回收工作内存，算子关闭阶段释放自身临时 token；最后一个活跃设备子树释放后，通过 `csReset(device, CS_VENDOR_SPECIFIC_TYPE, 0)` 关闭临时设备图会话，兜底回收遗留 token；设备连接和 CSF 保留。此厂商资源约定见 [SNIA 说明](../snia/README.md)。若另一个子树或游标仍持有设备资源，则延迟到它释放，避免误删其 token。

连接级程序使用基础索引500–513，分别覆盖通用树、扫描/过滤、聚合、排序、Limit、连接及容量查询；实际编号按前述 BackendId 规则分段，不持有 namespace 文件锁。SNIA 不能强制中断正在执行的 CSF，取消要等待当前请求完成再回收。

通用执行器中的 Scan 和无额外过滤的连续 Project，直接在列批次上求值：输入列保留引用，常量保留常量向量，算术表达式复用现有向量/SIMD 求值器，仅在需要行接口的边界组装 `qs_cell`。不支持的表达式或 scratch 分配失败时保留原来的逐行路径；scratch 由查询 workmem context 记账，输入结束后释放。

字符串过滤可以直接引用保留的 Heap 字节，按字符处理 SUBSTRING、LIKE 和字节序比较；只用于过滤的列不写入字典，离开 Scan 的字符串结果才物化。InitPlan 参数作为常量向量引用，保留 int128 值及实际 numeric scale，不为适配向量计算截断为 int64。substring 采用 PG 的起点和负长度规则，保持 NULL、CASE/AND/OR 短路。

无分组、无 DISTINCT 的 SUM/AVG/COUNT/MIN/MAX 使用每 CU 的局部状态，流水线结束后在协调 CU 合并。AVG 合并 sum/count，最后才相除；纯 Scan/Project 输入直接由列向量累加，Join 等输入暂时通过行批次适配。分组与 DISTINCT 聚合保留原有执行方式，没有为并行关闭其 SQL 语义。

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
# Guest：只运行原生 PG，用于 HOST 配置对照
TPCH_MODES=host-only sh /home/femu/tests/tpch-test.sh
# Guest：每条SQL先以HOST预热一次，再测试目标模式三次
TPCH_CACHE_MODES=warm sh /home/femu/tests/tpch-test.sh
# Guest：仅Q19，从冷态连续执行五次，相邻执行固定间隔1秒
TPCH_CACHE_MODES=repeat sh /home/femu/tests/tpch-test.sh 19
```

`scripts/guest/start-postgre.sh` 将原生 PG 对照配置写入数据目录：`max_parallel_workers_per_gather=4`、`shared_buffers=2GB`、`work_mem=16MB`。后续冷启动也沿用这些值；`work_mem` 是每个执行节点/worker 的额度，并非整条查询的总内存。每次基准的 `environment.log` 记录运行配置。
与原默认配置的完整冷启动对照见 [原生 PG 配置对照](doc/host-configuration.md)。

只保留标准规定的 Q2=100、Q3=10、Q10=20、Q18=100、Q21=100 行上限，其余查询不额外添加 LIMIT。查询参数、子查询写法和 Q15 的 CREATE VIEW/SELECT/DROP VIEW 均保持该目录原样。host 与 CSD 接收同一个文件，超时或错误直接记录为失败，不用手写参考 SQL 替代。

`scripts/guest/cold-query.sh` 在每组开始时停止 PG、sync、清除 Guest page cache 并重启 PG；不重启 FEMU，以免丢失 RAM-backed NVM。cold每次执行均重新准备，warm/repeat组内不清缓存、不切模式。单独调用该脚本时profile默认关闭，正式TPC-H入口为记录计划默认开启，各模式保持一致。Q15 的计时是原始脚本中各 SQL 耗时之和，包含创建/删除视图，host/CSD 规则相同。

warm每条SQL、每种模式独立从冷态开始，先用host-only执行同一正式SQL一次预热，再在同一连接中用目标模式执行三次；不统一预热全表，不要求实际驻留100%。预热和连接初始化不计正式延迟。

repeat每条SQL、每种模式独立从冷态开始，直接用目标模式执行五次。测试脚本仅在相邻执行之间用psql的shell命令停顿1秒，给异步观测缓冲时间；不执行pg_sleep等额外SQL，不在规划器中等待。五次共有四段停顿，停顿不计单次查询及五次查询时间之和；`protocol.txt`记录该规则。cold/warm不增加此间隔。新repeat结果不能与旧的零间隔组当成同一测试条件。

profile开启时，`SQLerate query IO`在主进程统一采样所选命名空间的普通NVMe完成读字节、Linux扫描/回收页面及文件页refault计数，并保留执行前后的空闲、文件和共享内存页量。普通NVMe计数包含PG并行worker，不能再次相加；CSD内部LOAD和spill仍分别读取已有设备指标，不能把普通读为零解释为设备没有读NVM。这些计数用于单查询独占实验，其他并发访问会混入全局设备/回收统计。

默认对五种 exe_path 各执行完整 22 条查询。结果保存到 Guest `/home/femu/results/tpch.*`，对应主机 `results/tpch.*`：`timings.csv` 每行记录查询编号、模式、延迟、结果是否一致和执行是否成功。Guest 需安装 PG 自带的 `auto_explain`；五种模式统一记录实际执行计划为 JSON（各查询的 `.err`），不启用 ANALYZE/TIMING 节点插桩。SQLerate 的实际 HOST/CSD 边界、成本估计和设备状态可从这些计划提取，无需另跑暖缓存 EXPLAIN 猜测测量时的选择。计时包含实际计划记录开销，单次微小差异不作为稳定优劣结论。

**结果一致不代表性能验收合格。** 只有实际设备工作内存不足且必须频繁 spill 造成的延迟增加可以接受。单 CU/向量化不足、重复扫描或复制、碎片化页请求、保守节点配额等实现问题造成的退化均不合格。脚本对执行错误或结果不一致返回非零；运行完成只表示正确性检查结束，不能当作性能验收通过。延迟必须另行比较并结合实际占用和计时分析，不能看到 spill 就自动认定内存不足。单次测量不用于宣称稳定加速比。

通用树 EXPLAIN 显示来自 PG 的连接树、实际 worker 数、并行流水线次数、顺序扫描行数、索引 Heap Tuple 数、内部读取、工作内存峰值及 spill 字节。开启 profile 后增加整个 CSF 的累计 CPU/墙钟/调用次数、实际同时执行的 CU 峰值，以及扫描解码、向量过滤和 Sort sink/source 的 CPU 时间。CPU 是各 CU 的累计值，可能大于查询墙钟；扫描/过滤/排序已包含在 CSF 总 CPU 中，不能再次相加。`CSD LOAD Preparation` 仅为 PG 构造请求的时间，异步 LOAD 的等待不计入该字段。单表快路径保留已有的逐算子计时。

同一 `profile` 开关还会在 SQLerate 重新规划时输出一条 `SQLerate planner` NOTICE。只执行正式查询的 EXPLAIN 脚本即可测规划。`total_ms` 是 planner hook 墙钟；`properties_ms` 包含候选物理算法确定、支持性/映射检查与成本属性提取，`compile_ms` 只包含最终选中设备子树的执行记录编码，`estimate_ms` 是数字成本计算，`format_ms` 包含成本摘要和选中节点的 EXPLAIN 说明。它们与 `preview/retain/host_cache/stmt_copy/rewrite/pg_other` 可相加；`pg_other` 包含 PG 搜索及未单列管理工作。`native_pg_inclusive/adaptive_pg_inclusive` 是两次 PG 规划的包含式计时；`index_inclusive/resources_inclusive` 已计入属性分析等阶段，不能再次相加。与旧版本比较时，应将新的 `properties_ms + compile_ms` 对照原来的 `compile_ms`，不能把阶段拆分当成全部工作消失。

`candidates/accepted` 记录候选数，`index_calls/index_queries` 区分索引元数据调用与实际 SPI 查询数。`property_nodes` 记录属性分析访问的节点数，`encoded_nodes` 记录最终通用编码器访问的节点数；`serialized_plans/fast_records` 分别记录最终通用设备记录和专用融合记录的生成次数。全 HOST 选择时 `encoded_nodes/serialized_plans/fast_records` 应为零。成本估价不为落选候选生成执行 Const 或逐节点长文本；选中后的详细成本仍附在对应 CSD 子树上，见 [规划设计](doc/planning.md)。候选成本保留为二进制数值，只有最终选中后才格式化说明；PG Path 的成本字段仍为 PG 成本单位。关闭 profile 不读取阶段时钟，也不输出 NOTICE；复用缓存计划时不会重新产生规划计时。

需要继续定位时，同一开关还输出 `SQLerate planner breakdown` 和 `SQLerate cache observation`。前者拆开设备打开、属性读取、探测 CSF 的下载/执行/卸载、候选复制/裁列/描述、估价及保留；`native_core/adaptive_core` 从两次 PG 规划墙钟中扣除已计时的扩展工作，`hook_other` 是扩展 hook 的其余工作。`cpu_ms` 使用当前 backend 线程 CPU 时钟，区别于包含等待的墙钟。设备打开的 `open_major_faults` 用于识别首次加载插件的磁盘缺页；资源细项已包含在 `resources_inclusive`，不能重复相加。

`cache_probe`现在是本次规划读取驻留记录及PG关系元数据的时间。同一OID在规划内复用结果；`files/residency/count/pages/cachestat_calls/mincore_calls`在规划阶段应为零，PG普通统计和文件大小元数据仍正常读取。`SQLerate cache ledger refresh`报告连接初始化及ExecutorEnd的主线程维护时间；`SQLerate cache file publish`另报后台文件观测时间。查询端到端时间仍保留；另报的“查询主体时间”仅逐样本扣除主线程ExecutorEnd维护时间，再做统计，仍包含规划。后台观测与查询可以重叠，不能把后台耗时再加到或从SQL耗时中扣除。

连接初始化按物理RelFileNode建立普通用户表及索引main fork的两张位图：shared_buffers按8KiB块，文件缓存按Guest系统页（当前4KiB）。共享页位来自有效BufferTag，文件位来自`mmap(PROT_NONE)+mincore`，不读入缺失页；cachestat只有数量，不能用来恢复页面身份。规划器使用位图派生的去重页数。一个8KiB数据库页对应两个4KiB文件页，可分别驻留；这只是记录粒度，不改变8KiB数据库页或4MiB DMA。用一个8KiB缓存位会丢失半页命中状态；即使记录时聚合，mincore仍按Linux系统页返回结果。DCP每次导入后根据buffer_id的旧BufferTag清除旧页位，再置新页位，不能只累计新页。

连接初始化同步建立完整记录，随后每个连接保留一个文件观测线程。顶层ExecutorEnd在设备接收及执行器清理后，先把上版关系路径/大小的独立副本交给后台，再由PG主线程采集全部用户关系元数据、共享缓冲描述符并更新DCP位图，重叠两项工作。包含原生HOST读取的实际计划在执行期间复用该线程，每秒更新一次文件观测，减少预热后整条查询的记录滞后；纯设备计划不增加这种周期观测。后台自行执行open/mmap/mincore及文件位图处理，不调用PG API；查询结束和下次规划均不等待它。主线程在规划入口、现有DCP轮询点及下次刷新时只发布已经完成的文件结果，未完成时直接使用旧文件位图，允许短暂滞后。`generation`表示共享快照版本，`file_generation`表示该关系已发布的文件观测版本；后台发布不覆盖DCP位图。主线程仍有元数据及共享描述符采集成本，并非整个ExecutorEnd零开销。物理文件节点或长度变化时不应用旧结果。

后台最多处理一份运行任务和最新一份待处理任务，连续语句替换尚未开始的旧任务，不积压队列；只有连接退出时等待线程释放。内部SPI及parallel worker不重复刷新。记录限于本连接，不跟踪其他连接和Linux独立回收；查询异常中止后标为无效，下一次成功语句结束重建，失效期间规划按未知缓存保守估价。没有后台进程、代理、PG源码修改或跨连接同步框架。

仅正式Q6的位图核验：在Guest执行`sh /home/femu/tests/cache-residency-test.sh`。它在2GiB及临时64MiB的PG缓冲中逐页对比真实驻留，验证DCP替换旧页及HOST/HPCP后的刷新，结束后恢复PG默认配置。准确性检查模块仅在测试中等待文件版本追上已提交版本，再比较每一位；正式执行和性能测试没有这个等待。脚本另用未加载检查模块的连续语句验证异步发布、任务合并及取消恢复。设备workmem不变，不把64MiB PG替换场景当作正常性能测量。

`schema_reads/schema_reuses` 记录规划内表列布局读取和复用次数。只缓存关系锁保护的列定义与文件节点，每个候选仍复制自己的列槽和限制信息；行数、谓词、物理选择和缓存比例按原流程计算。候选标记只借用输出列描述，实际交付给PG的输出表达式在选中后构造，预览标记在setrefs前移除；不改变PG和设备候选的搜索范围。

`preview_borrows` 记录预览时只读借用已准备子树的次数；修改前仍生成独立副本，避免影响 HOST 和其他候选。预览结束后包装只保留列标签，表达式由子树保留。`prune_reuses` 记录父节点保留子树全部输出、因而跳过内部重复裁列的次数；输出缩减时重新裁列，物理算法和排序需求仍重新检查。这两个计数表示省去的重复工作，不代表少搜索了候选。

强制 CSD 与 SQLerate 的通用树共享物理准备规则：Hash 构建侧选择、无需顺序的 Merge→Hash、分组 Sort 移除，以及无过滤非覆盖索引分组转顺序扫描＋Hash Aggregate。SQLerate 仍按成本选择连接顺序；共享规则不要求不同模式选中同一棵树。编码阶段校验冻结的物理选择，包括索引连接是否存在残余过滤。`CSD Stage Estimates` 中连接的 `join` 标明类型；IndexJoin 的 `probes/index_pages/heap_pages` 分别是估计探测次数、索引读页数、Heap 回表读页数，包含有界缓存之外的重复读取。它们是估价，实际读取仍查看运行计数；设计及验证见 [规划设计](doc/planning.md)。

通用树的 profile 同时记录 PG 调度线程的墙钟：`CSD Host Completion Copy` 是请求完成后同步读取 AFDM 控制记录及结果头的时间，`CSD Host Submit` 是 PG 主线程准备和提交请求的时间，不包含 callback 续交。其中 `Queue`、`Control Write` 及已有的 `LOAD Preparation` 均包含在 Submit 中，不能再次相加。`Poll` 是检查完成标志的时间，`Idle Sleep` 是没有可推进任务时的休眠时间。`Pending Requests Sum` 累计各请求提交到 callback 的时间，最终返回 PG 的请求还包含完成观察等待；跨并发请求求和，不是查询延迟或纯 I/O 时间。Host Submissions/Completions 只计 PG 提交和观察的次数，Callback Submissions 另计自动续交次数。关闭 profile 时不采集这些计时。

`CSD Host Assigned LOADs` 记录提前分配的顺序输入块数；开启 profile 时，`Completion Reads` 和 `Control Writes` 分别记录调度期间实际进行的控制记录读写次数，不包含初始化、结束清理和最终字符串结果复制。

`CSD Hash Table <节点号>` 进一步分解协调端构建开销：`memory_cpu` 是表分配/增长的CPU，`reindex_cpu` 是清桶及重建桶链的CPU，`reindexes` 包括初次建空表和预留容量后的清桶，`rows` 是累计重新处理的已有行数，`moved_bytes` 是扩容更换地址时搬移的旧表字节数（原地扩容不计入）。两项CPU计时是 `CSD Hash Build Coordinator CPU` 的内部区间，不能再与它相加。仍只在表操作边界取时钟。该记录位于PG/设备SO私有计划结构，修改布局后必须同步编译部署PG扩展与pg-sina-plan.so，不改SNIA或固件。

`CSD Setup Wall` 与 `Cleanup Wall` 分别记录设备执行前的准备和结束清理，已包含在 Tree Wall 中；准备不包含 SQL 规划、映射读取和首次资源发现。`Completion Notice` 记录 callback 发布完成到 PG 观察到的间隔，Average/Maximum 单位为微秒，Sum 是跨请求累计值，不能当成查询关键路径或设备空闲时间。

`CSD Callback Requeue Average/Maximum` 是 callback 收到完成到提交下一块的间隔，包含请求重配；`CSD PG Scan Requeue Average/Maximum` 是返回 PG 的扫描请求完成到 PG 再提交下一块的间隔，可能包含等待下游消费输出或阶段协调，不能全算调度开销。`CSD Callback Completed Requests Sum` 是自动续交之前各请求从提交到完成的累计时间，已包含在 Pending Requests Sum 中。`CSD Shared Sink Waits/Wait` 仅记录共享 Sink 有未消费输出时等待其他 worker 排空的次数和时间，不包含最终 EOF 等待或协调任务自身执行。

设备入口见 [设备算子说明](../../csd-programs/pg_sina_csd/README.md)。性能测量只使用正式 SQL 和当前环境，运行结果保存在仓库 results 目录；不将历史开发轮次当作当前验收结论。
