# filter-only 执行与返回

该模式只卸载基表扫描、过滤和必要的列提取；Join、Sort、Aggregate 继续由 PG 执行，不按查询编号选择算法。扫描参考仓库 DuckDB `src/execution/operator/scan/physical_table_scan.cpp` 的全局扫描状态、线程局部状态和投影列处理，复用现有 CU 输入块任务池。

## 调用流程与职责

1. PG 规划器收集后续表达式、连接、分组和排序所需的列，删除无用的 Scan 投影，并更新直接消费者的位置引用；整行引用保留原输出。
2. 查询主进程配置 SNIA batch、申请一次设备流所需的 AFDM 并提交初始请求。实际分配路径仍为 `csAllocMem → SNIA FEMU 适配层 → NVMe 0xb0 → FEMU CSD`；PG 不分配设备物理内存，FTL 也不承担 AFDM 分配。
3. 已有 SNIA 请求线程调用完成回调，推进完成状态处理、后续 batch 和结果读取。CU 从同一次 Scan 的输入任务池领取不同块。对无顺序要求、没有 InitPlan 且使用多个 CU 的 Scan，各 CU 直接编码 HOST 批次，HOST 读回后立即续交该 CU；协调端只启动和最终收尾，不逐批汇合或确认。其他执行树保留原有交付方式。回调只使用不可变映射、libc 和 SNIA，不调用 PG API；复用 SNIA 已有请求线程池，扩展不自行创建线程、辅助进程、代理或服务。
4. 每个设备流有两个有界 HOST 结果批次。回调直接读入空批次，继续处理后续请求。共享队列锁只保护批次元数据和领取，设备读取、请求配置及续交在锁外执行；独立的进程内互斥只串行修改该设备流的回调状态，不阻挡 PG 消费已发布结果，也不串行化其他查询。缓冲满时在条件变量上休眠，HOST 取走一批后只通知空位，不提交 LOAD、不恢复 CSF。暂停的回调不占用 CU，已申请的 AFDM 仍属于该查询。该队列是有限反压，不承诺消除所有 HOST 消费瓶颈。
5. 独立扫描由查询后端直接消费。原生 parallel-aware SeqScan 的两个批次放在 PG DSM（并行执行共享内存）中，由查询主进程在启动 PG workers 前初始化。PG 原生 workers 只领取最多 256 行到各自本地结果缓冲，执行 HOST 上层算子；不打开设备、不申请 AFDM、不续交请求。没有选举“PG 提交者”，设备推进不依赖 leader 是否参与取行。PG 没有选择并行计划时也不额外创建 workers。
6. Gather 下需要每个 PG worker 完整重读的私有输入仍保留 HOST；并行索引扫描也保留原生分片/顺序语义。保留这些边界，避免把共享分片流误当成可完整重放的输入。原生 HOST 上层并行不能一概删除。
7. 多个同时存活的设备输入遇到真实 AFDM 分配失败时，释放未提交的分配，先缩小输入块，再降低活动 CU 槽数重试；不按 PG worker 数重复申请。

这些职责属于 PG 扩展和设备程序对标准 SNIA batch/callback 的使用约定，不是新增的 SNIA API。FEMU 仍只解释通用设备请求，不解释 SQL。

## 返回布局

其他 pipeline 内部仍使用原有 `qs_cell`；独立并行 Scan 在各 CU 内直接完成最终编码。CSD→HOST 边界复用已有 `qs_result` AFDM，按列保存实际类型：bool/int2/int4/date/int8/float8 分别使用 1/2/4/4/8/8 字节，每列附 NULL 位图；Numeric、text、varchar、bpchar 使用 4 字节偏移，正文放入既有 64KiB 变长空间。

变长正文使用 PG 原生 varlena 字节。无谓词、无计算且保持类型的顺序扫描投影直接保留 Heap Numeric/字符串正文，不为只需返回的字符串构建字典；其他 Numeric 结果在设备端编码。PG 可以引用本地结果缓冲中的 Numeric/字符串生成 TupleTableSlot，不再逐行重建和分配 Numeric。bpchar 保留 typmod 的补空格语义。设备指针不会传给 HOST，原始 Numeric 的临时设备引用在输入块复用前复制为返回字节。

128KiB 固定值空间按实际类型计算批次容量，变长空间满时提前发布，NULL 位图随实际行数收紧。直接交付的 CU 在输入块末尾也发布剩余行，HOST 读回后才允许复用该输出/输入；其余路径仍可合并多个 CU 的输出。直接交付将变长正文紧接有效列数据，HOST 用一次 AFDM 读取取回，再本地展开到已有 `str_data` 位置，没有传输中间空洞。有效行数和变长长度通过现有 64 位 CompValue 返回，避免另读控制记录；原交付路径继续使用控制记录。HOST 仅读取有效部分，单次传输上限 256KiB。完整卸载的通用执行器也使用该布局；已有单表聚合专用 CSF 保持原接口。

PG 并行消费者只复制领取范围内的变长值并重定位偏移，不复制整个字符串池。NULL 位图按字节移位复制，固定列不再逐行重建 NULL；每个本地批次只解析一次列位置和类型宽度。本地批次在这些行消费完之前不会复用，保证 TupleTableSlot 的 Datum 生命周期。规划器的 HOST 边界成本同步按类型宽度、变长估计和实际批次容量计费；不是把所有输出继续估成 32 字节 cell。

## 清理与观察

正常 EOF、提前结束、重扫和查询取消都先停止连续回调，再排空请求、关闭设备状态并释放 AFDM。PG 在 ERROR 时先解除 DSM 映射，随后才执行扩展的 ResourceOwner 回调，因此共享队列另注册 `on_dsm_detach`，在映射仍有效时完成清理；初始化尚未返回就被取消也适用。查询内存上下文删除时注销回调并回收私有队列。

EXPLAIN 的 `CSD Direct CU Output` 标明各 CU 直接交付，`CSD Result Delivery` 标明回调驱动的双批次缓冲；不再有 `CSD Scan Producer PID`。共享设备流的返回字节、行数和请求数不能把 PG worker 的副本相加，`CSD Counter Scope` 会标明这一点。`CSD Workers` 是设备执行槽数，物理 CU 数由设备查询给出。

## 针对性验证

配置为 4 CU、256MiB workmem、64MiB FDM；HOST 为 4 worker 上限、2GiB shared_buffers、16MiB work_mem。本轮只测正式 Q17，另以 Q6/Q10 验证类型返回和原完整卸载路径。每条每种模式均重启 PG、清除 Guest page cache；未运行完整 22 条基准。

关闭 profiling 的正式 Q17 两轮均与同轮 HOST 结果一致：

| 冷启动轮次 | HOST（ms） | filter-only（ms） |
| --- | ---: | ---: |
| 1 | 3548.698 | 3585.698 |
| 2 | 3659.051 | 3619.445 |
| 均值 | 3603.875 | 3602.572 |

来源为 `results/tpch.8SI2Wl/`、`results/tpch.AjXZhq/`。本轮基本持平，单轮差异约正负 1.1%，不能宣称稳定快于 HOST。上一阶段 filter-only 单次冷启动为 4781.314ms（`tpch.AwUQQw`），本轮均值降低约 25%，但这是跨轮记录，不是统计置信区间。两个 lineitem 流各完整返回 6001215 行，设备 spill 为零。

Q6/Q10 三模式控制验证见 `results/filter-only.Ocvt1N/`，均与 HOST 一致；100ms/1s 的正式 Q17 取消后，同一连接可继续查询，FDM_USED 为 0。完整 CSD Q10 仍比同轮 HOST 慢，其性能不在本轮 Q17 交付优化中判为合格。

PG 的 `pgSinaHeapLayout`、`pgSinaPlannerCost` 通过；后者覆盖完成值的 17 位行数（含 65536 以上）、64KiB 正文，以及连续包跨过原固定 `str_bytes` 字段时的完整展开。设备 `pgSinaDirectScanOutput64MiB` 覆盖 4/8 CU 异步交付、长字符串/NULL、过滤、跨输入块续交，逐行校验内容和唯一性，并要求协调端输出零数据行；`pgSinaPackedOutput64MiB`、`pgSinaIndexJoin64MiB` 验证原返回路径与索引连接。设备组件只在至少 64MiB workmem 下验证。

## 直接交付的生命周期

PG 扩展用既有 `qp_plan.packed_result=3` 选择独立 Scan 的 CU 直接交付，普通类型返回仍为 2。这是 SQL 设备程序和 PG 扩展的私有记录格式，不修改 SNIA API 或 FEMU NVMe 命令。完成值保留原工作状态低 5 位，并携带 17 位行数、17 位正文长度及直接输出标志；SNIA 只透传该值。

每个 CU 仍持有自己的输入、列向量和输出区域，没有新增跨 Scan 共享 FDM。完成回调读取该 CU 的连续结果包，读完才续交下一次 Compute/LOAD；已有全局块计数负责分配同一 Scan 的不同输入范围。HOST 按轮转次序领取已完成 CU，避免一个快 CU 长期占据读回机会。

单个 CU 的 EOF 只说明它没有后续输入；其最后一批行先交付，HOST 数据流仍不结束。所有 CU 都到 EOF、最后一批均已读回后，才调用协调端收集统计并返回全局 EOF。取消/错误仍先停止和排空完成回调，再关闭设备上下文和释放 AFDM；不会因省去逐批协调而提前回收仍被读回的输入/结果。

直接包将变长正文移动到列数据之后；该连续区域可能覆盖旧固定 `str_bytes` 位置。因此长度必须取自 CompValue，HOST 完成展开后才写回本地 `str_bytes`。设备上的指针和原始 Numeric/字符串引用不会传入 HOST。

纯列投影只遍历输出列，直接从已有列批次提取对应值；不再构造一份全部属性的 cell 数组、再次遍历属性转换借用值，然后经通用投影拷贝。谓词和复杂表达式仍走原有扫描/过滤逻辑。此处参考 DuckDB 的线程局部列批次和局部扫描处理，不按 SQL 编号选择算法。

## 交付计时

`profile=on` 时增加按批次的计时，不在逐行 Datum 转换上加时钟。`CSD Consumer Queue Lock Sum` 是所有 PG 消费者获取结果队列锁的累计等待；`Consumer Batch Copy Sum` 是领取行时的复制累计时间；`Consumer Empty Sleep Sum` 单独统计发现队列为空后的休眠。`Delivery Advance Wall` 统计连续回调处理完成、读控制/结果、配置并提交请求的串行工作；`Result Read Wall`、`Control Read Sum` 是其中的读取部分。`Delivery Full Wait Sum` 包含多个完成回调等待 HOST 腾出空间的累计时间。这些累计量存在并行、嵌套和反压，不能直接相加或除以 SQL 总延迟当作阶段占比。

修改前的细分诊断为 `results/tpch.T9D9zb/`：Q17 两个 lineitem 流等队列锁累计 5576.427ms、复制 559.929ms、结果读取 403.161ms；Q18 orders/customer 分别等锁 884.629/149.390ms，结果读取 47.685/10.890ms。确认完成回调持队列锁执行同步设备读与续交，会阻挡 PG 消费已就绪结果。这些是 filter-only 特有的交付成本；两边共有的 Q17 两次扫描/排序及 Q18 私有索引聚合不作为相对退化原因。


## Q17 的协调与请求对照

分项比较使用上一阶段 `results/tpch.kGjwNC/` 与本阶段 `results/tpch.dX5eoF/` 的 profiling 记录；下表只合计 Q17 两个 lineitem 流。聚合、排序、两次扫描的逻辑仍与原生 PG 一致，不把双方共有的工作作为相对退化原因。

| 指标 | 原协调交付 | CU 直接交付 |
| --- | ---: | ---: |
| 协调 CPU 累计 | 1213.843ms | 0.018ms |
| SNIA batch 提交（含自动续交） | 13232 | 2867 |
| 控制记录读取 | 5607 | 4 |
| 消费确认写入 | 5605 | 2 |
| 结果读取请求 | 5128 | 2865 |
| 返回数据 | 293574806B | 293793180B |

每条流只有启动/收尾两次控制记录读取和一次收尾确认。返回字节略增约 0.07%，来自输入块末尾发布部分批次的头部；单次读回没有传输固定数组与正文之间的空洞。两个流的 FDM 分别维持 19454552/19389016B，workmem 峰值均约 286KB，没有通过增加内存、改变输入块或省略数据换取性能。

结果读取累计约 679ms，包含直接包的本地展开；它不能直接作为相对 HOST 的额外墙钟与其他阶段相加。当前已消除明显的协调串行汇合和频繁控制往返，本轮总延迟接近 HOST；仍存在实际数据读回、PG 元组消费与有限反压，尚不能据两轮结果宣称稳定加速。共享 FDM 与缓存复用不在本次修改范围。
