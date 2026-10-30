# filter-only 执行与返回

该模式只卸载基表扫描、过滤和必要的列提取；Join、Sort、Aggregate 继续由 PG 执行，不按查询编号选择算法。扫描参考仓库 DuckDB `src/execution/operator/scan/physical_table_scan.cpp` 的全局扫描状态、线程局部状态和投影列处理，复用现有 CU 输入块任务池。

## 调用流程与职责

1. PG 规划器收集后续表达式、连接、分组和排序所需的列，删除无用的 Scan 投影，并更新直接消费者的位置引用；整行引用保留原输出。
2. 查询主进程配置 SNIA batch、申请一次设备流所需的 AFDM 并提交初始请求。实际分配路径仍为 `csAllocMem → SNIA FEMU 适配层 → NVMe 0xb0 → FEMU CSD`；PG 不分配设备物理内存，FTL 也不承担 AFDM 分配。
3. 已有 SNIA 请求线程调用完成回调，推进完成状态处理、后续 batch 和结果读取。CU 从共享输入任务池领取块。回调只使用不可变映射、libc 和 SNIA，不调用 PG API；复用 SNIA 已有请求线程池，扩展不自行创建线程、辅助进程、代理或服务。
4. 每个设备流有两个有界 HOST 结果批次。回调直接读入空批次，继续处理后续请求。共享队列锁只保护批次元数据和领取，设备读取、请求配置及续交在锁外执行；独立的进程内互斥只串行修改该设备流的回调状态，不阻挡 PG 消费已发布结果，也不串行化其他查询。缓冲满时在条件变量上休眠，HOST 取走一批后只通知空位，不提交 LOAD、不恢复 CSF。暂停的回调不占用 CU，已申请的 AFDM 仍属于该查询。该队列是有限反压，不承诺消除所有 HOST 消费瓶颈。
5. 独立扫描由查询后端直接消费。原生 parallel-aware SeqScan 的两个批次放在 PG DSM（并行执行共享内存）中，由查询主进程在启动 PG workers 前初始化。PG 原生 workers 只领取最多 256 行到各自本地结果缓冲，执行 HOST 上层算子；不打开设备、不申请 AFDM、不续交请求。没有选举“PG 提交者”，设备推进不依赖 leader 是否参与取行。PG 没有选择并行计划时也不额外创建 workers。
6. Gather 下需要每个 PG worker 完整重读的私有输入仍保留 HOST；并行索引扫描也保留原生分片/顺序语义。保留这些边界，避免把共享分片流误当成可完整重放的输入。原生 HOST 上层并行不能一概删除。
7. 多个同时存活的设备输入遇到真实 AFDM 分配失败时，释放未提交的分配，先缩小输入块，再降低活动 CU 槽数重试；不按 PG worker 数重复申请。

这些职责属于 PG 扩展和设备程序对标准 SNIA batch/callback 的使用约定，不是新增的 SNIA API。FEMU 仍只解释通用设备请求，不解释 SQL。

## 返回布局

CU 内部仍使用原有 `qs_cell`。CSD→HOST 边界复用已有 `qs_result` AFDM，按列保存实际类型：bool/int2/int4/date/int8/float8 分别使用 1/2/4/4/8/8 字节，每列附 NULL 位图；Numeric、text、varchar、bpchar 使用 4 字节偏移，正文放入既有 64KiB 变长空间。

变长正文使用 PG 原生 varlena 字节。无谓词、无计算且保持类型的顺序扫描投影直接保留 Heap Numeric/字符串正文，不为只需返回的字符串构建字典；其他 Numeric 结果在设备端编码。PG 可以引用本地结果缓冲中的 Numeric/字符串生成 TupleTableSlot，不再逐行重建和分配 Numeric。bpchar 保留 typmod 的补空格语义。设备指针不会传给 HOST，原始 Numeric 的临时设备引用在输入块复用前复制为返回字节。

128KiB 固定值空间按实际类型计算批次容量，变长空间满时提前发布；不足一批的输出可跨 CU 完成继续累积。NULL 位图随实际行数收紧。控制记录提供有效行数及变长长度，HOST 仅读取有效部分，单次传输上限 256KiB。完整卸载的通用执行器也使用该布局；已有单表聚合专用 CSF 保持原接口。

PG 并行消费者只复制领取范围内的变长值并重定位偏移，不复制整个字符串池。NULL 位图按字节移位复制，固定列不再逐行重建 NULL；每个本地批次只解析一次列位置和类型宽度。本地批次在这些行消费完之前不会复用，保证 TupleTableSlot 的 Datum 生命周期。规划器的 HOST 边界成本同步按类型宽度、变长估计和实际批次容量计费；不是把所有输出继续估成 32 字节 cell。

## 清理与观察

正常 EOF、提前结束、重扫和查询取消都先停止连续回调，再排空请求、关闭设备状态并释放 AFDM。PG 在 ERROR 时先解除 DSM 映射，随后才执行扩展的 ResourceOwner 回调，因此共享队列另注册 `on_dsm_detach`，在映射仍有效时完成清理；初始化尚未返回就被取消也适用。查询内存上下文删除时注销回调并回收私有队列。

EXPLAIN 的 `CSD Result Delivery` 标明回调驱动的双批次缓冲；不再有 `CSD Scan Producer PID`。共享设备流的返回字节、行数和请求数不能把 PG worker 的副本相加，`CSD Counter Scope` 会标明这一点。`CSD Workers` 是设备执行槽数，物理 CU 数由设备查询给出。

## 针对性验证

配置为 4 CU、256MiB workmem、64MiB FDM；HOST 为 4 worker 上限、2GiB shared_buffers、16MiB work_mem。本轮只验证正式 Q17/Q18，以及 Q6/Q10 类型返回控制用例；每条每种模式均重启 PG、清除 Guest page cache。未运行完整 22 条基准。以下为关闭 profile 的最终结果，全部与同轮 HOST 一致。

| 查询 | HOST（ms） | filter-only（ms） |
| --- | ---: | ---: |
| Q6 | 1200.457 | 625.100 |
| Q10 | 2120.353 | 1850.854 |
| Q17 | 3493.722 | 4781.314 |
| Q18 | 8673.920 | 8943.110 |

记录在 `results/tpch.AwUQQw/`（Q17/Q18）、`results/filter-only.Mi51FH/`（Q6/Q10）。控制用例完整 CSD 路径 Q6/Q10 分别为 521.871/2330.709ms，结果一致；完整 CSD Q10 慢于 HOST，不在本轮 filter-only 性能改进中判为合格。

**Q17/Q18 filter-only 仍慢于 HOST、设备 spill 为零，性能未达标。** 两边共有的 Q17 两次扫描/排序、Q18 私有索引聚合重复不构成相对退化证据。下文只归因于 filter-only 新增的交付、结果编码与控制请求；共享缓存页也不能当成共享扫描结果。

设备 `pgSinaPackedOutput64MiB` 覆盖类型打包、长字符串/NULL、并行排序，以及异步多 CU 的完整、不重复输出和旧确认位清理；`pgSinaIndexJoin64MiB` 验证索引连接。PG 的 `pgSinaPlannerCost` 验证类型容量、部分批次收紧、跨 NULL 位图的共享切片和变长偏移重定位，`pgSinaHeapLayout` 验证 Heap 布局；Numeric 模块的 14 个边界值用例通过。所有设备组件测试使用至少 64MiB workmem。

初始化阶段 100ms 和执行阶段 1s 的 Q17 取消检查已加入 `tests/guest/filter-only-test.sh`：原连接均能继续执行 SQL，另用只读 GET_INFO 核对 FDM_USED 为 0。关闭 leader 扫描参与后的 Q6 输出一致；临时功能 SQL 验证 Scan 实际重扫三次（25 行×3，总和 900）和 LIMIT 提前结束的 EXPLAIN，结束后 FDM_USED 同样为 0。这些是生命周期检查，不作为 TPC-H 性能验收结果。

## 交付计时

`profile=on` 时增加按批次的计时，不在逐行 Datum 转换上加时钟。`CSD Consumer Queue Lock Sum` 是所有 PG 消费者获取结果队列锁的累计等待；`Consumer Batch Copy Sum` 是领取行时的复制累计时间；`Consumer Empty Sleep Sum` 单独统计发现队列为空后的休眠。`Delivery Advance Wall` 统计连续回调处理完成、读控制/结果、配置并提交请求的串行工作；`Result Read Wall`、`Control Read Sum` 是其中的读取部分。`Delivery Full Wait Sum` 包含多个完成回调等待 HOST 腾出空间的累计时间。这些累计量存在并行、嵌套和反压，不能直接相加或除以 SQL 总延迟当作阶段占比。

修改前的细分诊断为 `results/tpch.T9D9zb/`：Q17 两个 lineitem 流等队列锁累计 5576.427ms、复制 559.929ms、结果读取 403.161ms；Q18 orders/customer 分别等锁 884.629/149.390ms，结果读取 47.685/10.890ms。确认完成回调持队列锁执行同步设备读与续交，会阻挡 PG 消费已就绪结果。这些是 filter-only 特有的交付成本；两边共有的 Q17 两次扫描/排序及 Q18 私有索引聚合不作为相对退化原因。


## 本轮定位与优化

HOST CPU 采样见 `results/filter-cpu.iJAFxP/`。Q17 扩展 `execute` 占 PG 进程 CPU 样本约 9.26%，包含内联的切片复制；源码定位到重复计算类型列偏移和逐行搬运 NULL。这里是 CPU 样本比例，不是 SQL 墙钟占比，也不包含宿主 FEMU 的 CU CPU。已改为按批次缓存列视图和按字节复制 NULL 位图，保留原生 TupleTableSlot 接口及变长值边界检查。

设备协调端接收并行 Scan 输出时，已改为借用一批已有 canonical 行，再顺序编码，而非每行重新经过通用行调度器和搜索就绪 CU。确认消费及输入块释放仍在整批编码之后，待编码的 Numeric/字符串正文不会引用已复用输入。并且缓存每个返回批次的列位置，避免逐值重算布局。参考 DuckDB `ColumnDataCollection::Scan` 的短全局索引领取和局部批次处理方式，不改变扫描、过滤和 HOST 上层执行计划。

Q17 的分项对照为 `tpch.T9D9zb` 与 `tpch.kGjwNC`：两条 lineitem 协调 CPU 合计约 1677→1214ms，消费者切片复制累计约 560→369ms，队列锁等待累计约 5576→814ms。缩锁本身没有明显降低查询总时延；批次编码优化后才观察到总延迟下降，不能用等待累计下降直接宣称查询加速。Q17 仍返回约 280MiB，两条流仍有约 1.2s 协调 CPU、约 1.18s 主机续交/读回累计工作；结果读取约 414ms 已包含在后者。它们存在重叠、反压和串行依赖，尚未做完整关键路径分解，不能将这些累计量全部解释为相对 HOST 的额外墙钟，但可以确认扫描→串行汇合编码→逐批读回还有实现开销。

Q18 的实际节点对照见 `results/filter-nodes.6mjyWd/`（去字典前，开启节点插桩，不作为性能验收）：orders 平均节点时间 HOST 285ms/filter-only 478ms，customer 41/151ms；公共私有索引聚合在 filter-only 中反而略快，不是此次差额的来源。随后确认 customer 的纯投影曾把 150000 个姓名（约 2.7MB 正文）插入字典再反查。直接返回 Heap 字符串后，`results/tpch.pe7tmz/` 显示该流字典条目/正文降至 0，workmem 峰值约 6.62MB→0.286MB，CSF CPU 185→53ms，流水线墙钟约 253→75ms。这不是通过增加内存或省略结果实现的，输出字节及 150000 行均保持一致。

Q18 剩余差额主要仍需继续检查 orders 的逐批协调和请求交接：该流约 1.5M 行、43MB 返回产生约 2509 次提交，协调 CPU 181ms、连续推进累计 157ms（其中结果读 42ms、控制读 36ms），小块协议工作不能只按返回字节除以 PCIe 带宽估算。长达数秒的 orders 满队列等待发生在 HOST 尚未消费时，不能将那段双方共有的 Hash 构建等待算作额外传输成本。更精确的剩余归因需要缩短/追踪这些依赖链，不能靠开启更多 CU 或把全部慢归结为两个公共重复分支。
