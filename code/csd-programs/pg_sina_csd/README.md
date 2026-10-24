# pg_sina_csd 设备算子

这里的 SO 在容器内编译，由 FEMU CU 执行，逻辑上属于 CSD CPU。Guest PG 扩展在 `code/apps/pg_sina_csd`，负责生成设备参数、直接调用 SNIA 和转换最终结果；设备程序不访问 Guest 文件描述符、不调用 NVMe、不解释 SQL 文本。

## PG 原生计划执行

`plan.c` 编译成 `pg-sina-plan.so`。Guest 以 PG 已选中的连接树为基础，进行分组键过滤下推、计数预聚合等局部改写，转换为 `apps/pg_sina_csd/include/plan.h` 中的节点数组。该格式只属于本应用，FEMU 和 SNIA 没有为 SQL 增加协议。

同一 SO 还提供 `pg_sina_workmem_capacity`：无 AFDM、无标量参数，正常返回共享 `libcs-workmem` 的总容量（字节），由 SNIA 通过 64 位 CompValue 交给 Guest。该调用不创建查询 context，也不分配查询工作内存。CU 数和 FDM 容量仍由 Guest 通过 SNIA 资源查询取得。

入口 `pg_sina_plan` 按顺序接收五个 AFDM：

1. 节点计划与执行器分配的缓冲偏移。
2. 控制记录数组：整树任务及各流水线任务的私有状态句柄、所需页地址、完成/结束/关闭标记。
3. 结果缓冲数组：首项是最终 `qs_result`，其余是设备内部交接批次；内部行紧密存储所需列，不传回 PG。
4. 原始页输入、扫描列批次和字符串字典配置的交接区域。
5. 每个执行槽各 64 KiB 的 spill I/O 缓冲。

本项目约定：`cparam1` 的最低位表示计时开关，其余位为执行槽编号；0 推进整树，1–64 执行当前并行流水线。可用和已消费槽的位图为 64 位，最高槽使用第 63 位；64 槽的完整掩码为 UINT64_MAX。执行槽不是固定的物理 CU 绑定，每次普通 E1 请求占用一个可用 CU。`cparam2` 仍仅由 SNIA 用于 spill 重入。参数顺序由双方代码约定，不改变 SNIA/FEMU 接口，也没有参数解码框架。

并行 Heap 顺序扫描由 Guest 按空闲 worker 提前分配不重叠的数据块，提交 `LOAD→CSF`。CSF 的现有 cparam1 低位保留 profile 和 worker 编号，位 8 表示此次附带已安排的输入，位 9 以上为 Heap 页号；cparam2 仍仅用于 spill 重入。这是本应用的计算参数约定，不改变 SNIA/FEMU 接口。设备接收输入后连续执行当前算子链，CompValue 的低三位分别表示有输出、输入耗尽、worker 完成，省去每块数据的控制记录读取和 ready 写入；有输出尚未被下游消费时不覆盖缓冲。共享 Hash/聚合 Sink 能消费下一批时，在同一次 CSF 内继续处理当前输入块。

通用树的共享字典由设备 SQL 程序在查询根 workmem 中建立，按需增长，所有 CU 引用同一组稳定 ID。旧存储保留到查询结束以保证借用的字节仍有效，增长的全部存储计入查询总预算；AFDM 中只传配置。已带字典 ID 的输入导入后保持原 ID。独立算子仍可接收原有平铺字典输入。字典本身尚不支持 spill，分配失败不会破坏旧数据，但会返回设备错误。

索引、临时分区及串行 Scan 仍在需要数据页时写控制记录并正常返回，Guest 把地址转换为 `csQueueStorageRequest(NVM_TO_AFDM)` 再继续调用。扫描块由 Guest 根据 FDM 容量在 4/2/1 MiB 中选择，索引按需读取 8 KiB 页。原始数据不经过 Guest PG 缓冲。设备执行中发现 spill 需要时直接返回 `csNeedStore/Load/Free` 完成值，由已有 SNIA batch 调度器处理并重入，PG 不接收或解释中间行。

Heap 每次 LOAD 的块可包含多个列批次。通用树将列向量限制为至多 2048 行，扫描前用页内 line pointer 数保守选择能放入向量的完整页，再复用原 Scan/Filter ABI。页指针包含无效项时仍保守计数，单页始终容纳得下；实际页与 Tuple 校验继续由 Scan 完成。已加载字节数和解码进度保存在各扫描节点的 workmem 状态中，控制记录可同时供 Index Join 请求缺页；筛选后需要延迟解码的列始终引用当前这批页。该块的所有向量和输出消费完成后才接受下一块，没有新增读取或跨查询缓存。

Guest 从 PG 选定的树中标记可并行的扫描/投影/Hash 或 Index Probe 段，包含初始化和 Hash Build 输入。设备先完成共享 Hash 表，再建立各 CU 独立的游标；Heap 输入由 PG 动态分配，索引及临时分区任务继续从设备原子游标领取。阻塞节点消费投影后的行批次，不重复构建 Hash 表。Guest 通过已有事件和批次请求推进每个完成的槽，不需要设备调度 runtime 或代理。

不同流水线依次复用 AFDM；各任务的动态状态通过子 context 计入同一个查询预算。整树节点仍按行拉取组合，中间值保持精确 `qs_cell` 的 int128、精度和 NULL；Scan/Filter 则复用已有列向量和 SIMD 路径。Q1/Q6 继续使用下文的单表快路径。

## 节点与内存

- Scan/Projection：只解码父节点真正需要的列；投影、过滤、CASE、NULL 三值逻辑、短路、数值运算、LIKE、substring 和 year 在设备执行。仅用于 LIKE 的长字符串直接在扫描时判断，不先将所有文本放入字典。
- Hash Join：按 PG 的左右输入 build/probe，支持 INNER/LEFT/SEMI/ANTI 和多键等值/剩余条件。表内按 PG 类型保存整数/date 的 4/8 字节值，numeric 保留完整 128 位，避免每列承担 32 字节的对齐填充。并行构建输入的 CU 按批预留行区并原子链接桶头，省去协调端逐行接收与再次构建桶链。表满时将未消费行保留在原 AFDM，协调端在本轮 CU 都暂停后扩容；阻塞子节点的父表仍从小容量开始，随实际输出增长。INNER 表扩容失败时，将已消费的前缀和后续输入按键分为四个分区，使用现有 SNIA spill 请求保存紧凑行；两侧 Heap 均只扫描一次。构建侧结束后重新检查实际空闲内存：若全部分区能驻留，按实际行数分配行区、单独为桶取二次幂容量，直接多 CU 扫描 Probe 侧，避免将它预先落盘；否则逐分区构建共享 Hash 表，各 CU 动态领取 Probe 块，偏斜分区超限时分批构建并重复读取该分区的 Probe 临时数据，不改用全表排序。其他不能采用此路径的形态保留原 Merge 回退。所有分配仍受查询根 context 和物理池约束。
- Merge Join：分别排序输入，按键归并；重复键组可 spill，保留 NULL、重复和外连接语义。
- Index Join：复用 `index_scan.c` 的 PG12 整数 B-tree 下探与 Heap Tuple 解码，可在连接树多个位置出现。最多批量 64 个外侧键；要求保留顺序时一次一个键。缺页只通过控制记录交给 Guest 批量提交内部 LOAD。SEMI/ANTI 在消费端确认条件满足后停止查找该键；索引自连接复用当前扫描块中的 Heap 页，超出该块再读取，不建立跨查询缓存。索引游标按实际使用的槽延迟分配，单 CU 扫描只保留自身游标；共享索引上下文的额度按槽数增加。
- 有序 Index Scan：同一文件沿 PG12 B-tree 正向叶链收集 TID，批量回表，按索引顺序生成列批次。保持单游标供流式 GroupAggregate 消费；不把有序输入任意拆给多个 CU。沿叶链预读至多 16 页，Heap 在 PG 提供的正相关性大于 0.9 时预读至多 32 页，均受文件边界和原页缓存限制；无统计或相关性低时按需读取。额外 TID 列表仅为该游标分配，等值连接不承担这份开销。
- Index Only Scan：冻结镜像中支持前两个整数索引列，直接输出键和 NULL 位图，不请求 Heap 页。需要索引顺序时沿叶链读取；无需顺序时各 CU 动态领取互不重叠的物理页范围，只处理叶页，跳过内部页和已删除页。每个 CU 使用私有游标、页缓存和列批次；不支持的覆盖列仍走回表或原回退路径。
- Aggregate：扫描或等值连接输出上的单个 SUM/COUNT/AVG 可用有界 Hash 表聚合，支持多键（包括字符串、numeric、NULL）及聚合参数的算术表达式；LEFT JOIN 补出的 NULL 不计入 COUNT(列)，COUNT(*) 则计入行数。保留精确 SUM/count，AVG 在最终输出时才相除。整数和字符串字典 ID 按 64 位保存，NULL 使用位图；numeric 键保留完整精度。直接扫描和并行的纯键去重可按估计组数预留；其他多键表仍从 128 槽开始增长，避免错误估计挤占子节点的预算。并行输入的 CU 直接更新同一张表，短暂锁定当前桶保护键初始化及精确 sum/count；不复制完整局部表，也不把各 CU 的 DISTINCT 计数相加。满表的原始行保留在 AFDM，当前 CU 都暂停后协调端扩容或 spill。聚合同时预留将启动的 worker 状态及 spill 空间；实际无法容纳时该段串行执行。需要 ORDER BY 时在聚合后排序；预算不足时归并部分状态，不重读明细。COUNT DISTINCT 的精确键投影先按“分组键＋参数”做 Hash 去重，再 COUNT 非 NULL 参数；不覆盖的形态保留排序去重。任意组合的 SUM/AVG/COUNT/MIN/MAX、分组和 HAVING 仍可走原有流式或外部排序聚合。
- Sort/Limit：source 跳过没有生成 run 的空闲槽，全空输入正常返回空结果；复用 `sort.c` 的有界工作内存 run 和分批归并，完整处理输入、OFFSET/LIMIT、提前结束与回收。
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

单表快路径按设备 CU 数、FDM、workmem 和输入块数选择最多 64 个独立 SERIAL batch，完成的执行槽立即领取下一块，不等待固定四分支轮次。Scan 按需输出 int64 列、NULL 位图、显示精度和 selection；numeric 可延迟到筛选后解码。`query_vector.h` 每批 256 行，复用列引用/常量和公共表达式，只为仍存活的行计算复杂条件。AVX2 支持比较、无溢出的加减和 int32×int32→int64；其他情况用精确 int128。没有浮点近似。

动态 Hash/聚合状态属于 workmem，FDM 仅作输入输出。单表入口 `profile.c` 包装计时，默认关闭；需要计时的 AFDM 放在 spill I/O 之前。通用树在 `pg_sina_plan` 入口累计整个 CSF，同时在 Scan/Filter/Sort 的调用边界计时；各 worker 结束后合并计数。还记录实际同时运行的 CU 峰值。算子分项包含在 CSF 总时间中，不能与总时间相加，也不对逐行递归反复计时。

## 构建和验证

```sh
cmake -S code/csd-programs -B code/csd-programs/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build code/csd-programs/build -j4
ctest --test-dir code/csd-programs/build --output-on-failure
```

设备测试使用真实布局的模拟 PG 页与独立行循环校验 Join、NULL、重复键、外连接、精度、乱序多 run 归并、小内存 spill 和回收。它不模拟 NVMe 时间。实际 Guest 冷启动比较和使用限制见 [PG 扩展说明](../../apps/pg_sina_csd/README.md)。

TPC-H 验收只使用仓库 `tests/guest/queries/` 的 SQL，由 `tests/guest/tpch-test.sh` 执行；这里的 C/C++ 单元测试只校验算子行为，不替代 SQL 正确性和性能验收。
