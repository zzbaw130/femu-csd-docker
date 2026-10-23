# pg_sina_csd 设备算子

这里的 SO 在容器内编译，由 FEMU CU 执行，逻辑上属于 CSD CPU。Guest PG 扩展在 `code/apps/pg_sina_csd`，负责生成设备参数、直接调用 SNIA 和转换最终结果；设备程序不访问 Guest 文件描述符、不调用 NVMe、不解释 SQL 文本。

## PG 原生计划执行

`plan.c` 编译成 `pg-sina-plan.so`。Guest 将 PG 已选中的 Plan 转换为 `apps/pg_sina_csd/include/plan.h` 中的节点数组，保留连接树和左右输入。该格式只属于本应用，FEMU 和 SNIA 没有为 SQL 增加协议。

入口 `pg_sina_plan` 按顺序接收五个 AFDM：

1. 节点计划与执行器分配的缓冲偏移。
2. 控制记录数组：整树任务及各流水线任务的私有状态句柄、所需页地址、完成/结束/关闭标记。
3. 结果缓冲数组：首项是最终 `qs_result`，其余是设备内部交接批次；内部行紧密存储所需列，不传回 PG。
4. 原始页输入、扫描列批次和共享字符串字典的交接区域。
5. 每个执行槽各 64 KiB 的 spill I/O 缓冲。

本项目约定：`cparam1` 的最低位表示计时开关，其余位为执行槽编号；0 推进整树，1–4 执行当前并行流水线。执行槽不是固定的物理 CU 绑定，每次普通 E1 请求占用一个可用 CU。`cparam2` 仍仅由 SNIA 用于 spill 重入。参数顺序由双方代码约定，不改变 SNIA/FEMU 接口，也没有参数解码框架。

设备 Scan 需要数据页时写控制记录并正常返回；Guest 仅把这些地址转换为 `csQueueStorageRequest(NVM_TO_AFDM)` 再继续调用。扫描块由 Guest 根据 FDM 容量在 4/2/1 MiB 中选择，索引按需读取 8 KiB 页。原始数据不经过 Guest PG 缓冲。设备执行中发现 spill 需要时直接返回 `csNeedStore/Load/Free` 完成值，由已有 SNIA batch 调度器处理并重入，PG 不接收或解释中间行。

Guest 从 PG 选定的树中标记可并行的扫描/投影/Hash 或 Index Probe 段，包含初始化和 Hash Build 输入。设备先完成共享 Hash 表，再建立各 CU 独立的游标；任务从原子游标动态领取输入块。阻塞节点消费投影后的行批次，不重复构建 Hash 表。Guest 通过已有事件和批次请求推进每个完成的槽，不需要设备调度 runtime 或代理。

不同流水线依次复用 AFDM；各任务的动态状态通过子 context 计入同一个查询预算。整树节点仍按行拉取组合，中间值保持精确 `qs_cell` 的 int128、精度和 NULL；Scan/Filter 则复用已有列向量和 SIMD 路径。Q1/Q6 继续使用下文的单表快路径。

## 节点与内存

- Scan/Projection：只解码父节点真正需要的列；投影、过滤、CASE、NULL 三值逻辑、短路、数值运算、LIKE、substring 和 year 在设备执行。仅用于 LIKE 的长字符串直接在扫描时判断，不先将所有文本放入字典。
- Hash Join：按 PG 的左右输入 build/probe，支持 INNER/LEFT/SEMI/ANTI 和多键等值/剩余条件。表内按 PG 类型保存整数/date 的 4/8 字节值，numeric 保留完整 128 位，均保留精度与 NULL 信息，避免每列承担 32 字节的对齐填充。阻塞子节点的父表从小容量开始，随实际输出增长，避免提前挤占子节点的内存。Hash 表使用查询预算扣除两份排序配额后的上限，实际分配仍受查询根 context 约束。超出配额时缩小并保留已经消费的投影行，交给外部 Merge Join，不回头重扫原始 Heap。
- Merge Join：分别排序输入，按键归并；重复键组可 spill，保留 NULL、重复和外连接语义。
- Index Join：复用 `index_scan.c` 的 PG12 整数 B-tree 下探与 Heap Tuple 解码，可在连接树多个位置出现。最多批量 64 个外侧键；要求保留顺序时一次一个键。缺页只通过控制记录交给 Guest 批量提交内部 LOAD。SEMI/ANTI 在消费端确认条件满足后停止查找该键；索引自连接复用当前扫描块中的 Heap 页，超出该块再读取，不建立跨查询缓存。
- 有序 Index Scan：同一文件沿 PG12 B-tree 正向叶链收集 TID，批量回表，按索引顺序生成列批次。保持单游标供流式 GroupAggregate 消费；不把有序输入任意拆给多个 CU。沿叶链预读至多 16 页，Heap 在 PG 提供的正相关性大于 0.9 时预读至多 32 页，均受文件边界和原页缓存限制；无统计或相关性低时按需读取。额外 TID 列表仅为该游标分配，等值连接不承担这份开销。
- Index Only Scan：对于冻结镜像中仅需整数前导键的覆盖扫描，沿叶链直接输出键和 NULL 位图，不请求 Heap 页；不支持的覆盖列仍走回表或原回退路径。
- Aggregate：扫描输入上的单整数键、单个 SUM/COUNT/AVG 可用有界 Hash 表聚合；保留精确 SUM/count，AVG 在最终输出时才相除。预算不足时将部分状态交给现有 Sort/spill 归并，不重读明细；不覆盖的形态沿用原算法。支持任意组合的 SUM/AVG/COUNT/MIN/MAX、分组和 HAVING；按组有序输入流式计算，否则先外部排序。COUNT DISTINCT 支持一组共享的 DISTINCT 输入列，排序后跳过重复/NULL。
- Sort/Limit：复用 `sort.c` 的有界工作内存 run 和分批归并，完整处理输入、OFFSET/LIMIT、提前结束与回收。
- CTE/InitPlan：共享 CTE 在设备计算一次并暂存，读者各自持有游标；标量初始化结果保存在设备参数数组。整数 IN/NOT IN 集合保留 NULL 和空集合语义。

所有动态算法状态从 `libcs-workmem` 分配。通用查询先建立受 `max_memory` 限制的根 context；排序、索引子 context 同时计入根配额和全局池。Hash 表、run、归并游标不能放入 AFDM 来绕开工作内存上限。

`sort.c` 每个 run 最大约 1 MiB，spill 块为 64 KiB。内存优先保留 run，下一段超过预算才 STORE；归并 fan-in 受缓存预算限制，必要时分轮合并并释放旧 token。需要读缓存时优先 spill 驻留 run，避免驻留数据挤占缓存、每输出几行便重新读取整个块。每个 run 的归并头按实际投影列宽分配，与 token 一起计入 workmem，避免小预算被固定 16 列的描述占满。非 NULL 整数/date 和 run 内相同 scale 的 numeric 键使用精确 int128 radix sort，支持多列升降序并跳过常量字节；scratch 计入 run 所有者的配额，放不下时保留原有排序，不额外制造 spill。NULL、字符串和混合 scale 继续走原有精确比较。字符串比较使用字典；短最终字符串排序标记为 `UINT8_MAX`，不能与合法的 numeric 显示精度重叠。

正常完成或提前 Limit 后释放私有 context；取消/错误时 Guest 再提交 close，随后关闭当前 SNIA 会话，释放仍存活的临时 token。没有后台代理或独立设备执行进程。

## 单表列向量快路径和独立入口

原有实现保留供单表聚合及独立算子使用：

| 源文件 / SO | 主要入口和 AFDM 顺序 |
| --- | --- |
| scan.c / pg-sina-scan.so | `scan`：原始 Heap、计划、列批次、任务，必要时附字符串字典 |
| filter.c / pg-sina-filter.so | `filter`：列批次、计划、原始 Heap；生成 selection 并物化延迟列 |
| aggregate.c / pg-sina-aggregate.so | `aggregate_sink`：列批次、计划、任务；`aggregate_source`：句柄组、结果；分组时追加 spill I/O |
| sort.c / pg-sina-sort.so | `sort`：小结果、计划；`sort_sink`：列批次/行批次、计划、任务、spill I/O；`sort_source`：句柄组、结果、spill I/O |
| limit.c / pg-sina-limit.so | 小结果、计划；调整结果范围 |
| join.c / pg-sina-join.so | 原有独立连接链入口；PG 通用计划已由 plan.c 执行，不再用它选择连接顺序 |
| heap_count.c / pg-sina-heap-count.so | 诊断：原始页、计数；cparam1 为物理列数，cparam2 为有效字节 |

单表快路径最多四个独立 SERIAL batch，完成的执行槽立即领取下一块，不等待固定四分支轮次。Scan 按需输出 int64 列、NULL 位图、显示精度和 selection；numeric 可延迟到筛选后解码。`query_vector.h` 每批 256 行，复用列引用/常量和公共表达式，只为仍存活的行计算复杂条件。AVX2 支持比较、无溢出的加减和 int32×int32→int64；其他情况用精确 int128。没有浮点近似。

动态 Hash/聚合状态属于 workmem，FDM 仅作输入输出。单表入口 `profile.c` 包装计时，默认关闭；需要计时的 AFDM 放在 spill I/O 之前。通用树在 `pg_sina_plan` 入口累计整个 CSF，同时在 Scan/Filter/Sort 的调用边界计时；各 worker 结束后合并计数。还记录实际同时运行的 CU 峰值。算子分项包含在 CSF 总时间中，不能与总时间相加，也不对逐行递归反复计时。

## 构建和验证

```sh
cmake -S code/csd-programs -B code/csd-programs/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build code/csd-programs/build -j4
ctest --test-dir code/csd-programs/build --output-on-failure
```

设备测试使用真实布局的模拟 PG 页与独立行循环校验 Join、NULL、重复键、外连接、精度、乱序多 run 归并、小内存 spill 和回收。它不模拟 NVMe 时间。实际 Guest 冷启动比较和使用限制见 [PG 扩展说明](../../apps/pg_sina_csd/README.md)。

TPC-H 验收只使用仓库 `tests/guest/queries/` 的 SQL，由 `tests/guest/tpch-test.sh` 执行；这里的 C/C++ 单元测试只校验算子行为，不替代 SQL 正确性和性能验收。
