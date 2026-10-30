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

变长正文使用 PG 原生 varlena 字节。无计算且保持类型的顺序扫描投影，在没有谓词或只有固定宽度列与常量的单个比较时，直接保留 Heap Numeric/字符串正文，不为只需返回的字符串构建字典。简单比较仍先生成选择向量，过滤后保留投影列的原始正文；Numeric/字符串谓词和其他复杂条件继续使用原有物化路径。其他 Numeric 结果在设备端编码。PG 可以引用本地结果缓冲中的 Numeric/字符串生成 TupleTableSlot，不再逐行重建和分配 Numeric。bpchar 保留 typmod 的补空格语义。设备指针不会传给 HOST，原始 Numeric 的临时设备引用在输入块复用前复制为返回字节。

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

纯列投影只遍历输出列，直接从已有列批次提取对应值；不再构造一份全部属性的 cell 数组、再次遍历属性转换借用值，然后经通用投影拷贝。简单固定列比较使用现有向量 Filter，原始正文不再被 Filter 转为中间值；其他谓词和复杂表达式仍走原有扫描/过滤逻辑。此处参考 DuckDB 的线程局部列批次和局部扫描处理，不按 SQL 编号选择算法。

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

## Q2/Q20 变长返回修复与 Q1 原值投影

非共享交付从回调队列取完整批次时，原代码只复制头部、有效列和正文，漏掉了布局尾部的 `str_bytes`；本地批次的长度仍为零，导致 Q2/Q20 报 `invalid CSD variable result`。现在同时复制正文长度。共享交付原本由行切片函数重建长度，因此之前没有暴露这一错误。

Q1 虽然只按日期过滤，其 Numeric/字符投影也因存在谓词而失去原值路径，经过中间数值解码、通用投影和 PG Numeric 再编码。现在简单固定列比较只生成选择向量，后续原列投影直接编码保留的 Heap 正文。这是按算子形态选择的通用规则，没有按查询编号分支；没有改为卸载聚合，也没有使用共享 FDM 或增加内存。

只复测正式 Q1/Q2/Q20，配置保持 4 CU、256MiB workmem、64MiB FDM；每个组合重启 PG、清除 Guest page cache，正式对比关闭 profiling：

| 查询 | 修复前 filter-only（ms） | 本轮 HOST（ms） | 本轮 filter-only（ms） | 本轮结果 |
| --- | ---: | ---: | ---: | --- |
| Q1 | 11849.759 | 2814.106 | 2469.982 | 一致，比 HOST 低 12.23% |
| Q2 | 变长结果错误 | 966.181 | 847.820 | 一致，比 HOST 低 12.25% |
| Q20 | 变长结果错误 | 3326.249 | 2646.070 | 一致，比 HOST 低 20.45% |

修复前记录为 `results/tpch.gb3c93/`；修复后 Q1 为 `results/tpch.2QxLsX/`，Q2/Q20 为 `results/tpch.laAqGW/`。这是单轮正式测量，不代表所有运行条件下的稳定加速。

Q1 的独立冷启动诊断使用 `tpch.0aL9ts` 与 `tpch.rDLmIJ`，不混入正式延迟：

| 指标 | 修改前 | 修改后 |
| --- | ---: | ---: |
| 设备 CSF CPU 累计 | 9105.913ms | 3299.673ms |
| Filter CPU 累计 | 1074.907ms | 49.526ms |
| CSF 调用 | 22177 | 4839 |
| HOST batch 提交 | 24457 | 4839 |
| 结果读取 | 6156 | 4838 |
| 返回字节 | 541396744 | 455050270 |
| 返回行数 | 5884033 | 5884033 |
| workmem 峰值 | 739344B | 285888B |

两次设备 spill 均为零，协调 CPU 均约 0.011ms。原来的主要问题是失去原值投影后的转换和通用路径开销，不能归因于物理内存不足。各项累计时间存在并行和包含关系，不作为可相加的 SQL 墙钟占比。

PG 两项组件测试通过；设备的原类型返回、4/8 CU 直接交付和索引连接三项测试通过。直接交付用例补充了固定列比较后保留变长正文、NULL、过滤排除行及唯一性交付检查；Numeric/字符串谓词继续走原物化路径的边界也有覆盖。三条查询结束后 FDM_USED 为零。本轮未重跑完整 TPC-H。

## Q16/Q21 的供给、反压与 Q11/Q22 未卸载原因

本轮只诊断，不修改执行代码。正式全量记录`results/tpch.BW5CB4/`中Q16为HOST 601.408ms、filter-only 849.303ms；Q21为1911.633/2173.111ms。补充使用原SQL、逐次冷启动的设备/交付profiling，记录在`results/tpch.YSbPWD/`；另用auto_explain开启ANALYZE/TIMING/BUFFERS/VERBOSE，保存在`results/filter-only-node-times.A6lOfa/`。两组诊断都逐行比较HOST/filter-only一致，插桩延迟不作为新的正式成绩。设备仍为4 CU、256MiB workmem、64MiB FDM，结束后FDM_USED=0。

当前两条的IN/EXISTS并没有全部卸载：Q16仅卸载part的品牌、类型、尺寸过滤，partsupp的NOT IN及supplier子查询留在HOST；Q21仅卸载l1的`l_receiptdate > l_commitdate`，l2/l3的EXISTS/NOT EXISTS仍由HOST执行参数化lineitem_pkey探测，orders也留在HOST。因此不能把它们概括成设备执行了完整的IN/EXISTS过滤。

| 设备/交付诊断 | Q16 | Q21 |
| --- | ---: | ---: |
| 设备扫描行数 | 200000 | 6001215 |
| 返回行数 | 29182 | 3793296 |
| 返回字节 | 1731802 | 31615400 |
| 扫描pipeline墙钟 | 218.797ms | 1956.036ms |
| CSF CPU累计 | 242.988ms | 1818.259ms |
| Heap解码CPU累计 | 26.819ms | 1078.419ms |
| Filter CPU累计 | 187.941ms | 193.679ms |
| 平均活跃CU | 1.11 | 0.93 |
| HOST等空队列累计 | 857.696ms | 16.614ms |
| 回调等满队列累计 | 0 | 1786.479ms |
| 结果读取墙钟累计 | 4.685ms | 112.764ms |
| 消费队列复制累计 | 1.375ms | 38.124ms |
| 消费队列锁等待累计 | 2.804ms | 7.872ms |

这些累计量有并行和包含关系，尤其HOST空队列等待合计多个PG参与者，不能与pipeline墙钟相加。`CSD Tree Wall`还包括源扫描结束后HOST执行期间，不能当作设备扫描计算时间；Q21的pipeline墙钟本身也包含HOST反压，不能将约2秒全部算作设备过滤。

Q16由设备供给及其与PG并行Hash构建的衔接拖累，未表现出HOST消费不及。满队列等待为0，HOST则明显等数据，读回/复制/队列锁均远小于源阶段。节点插桩中，相同part输入的平均扫描时间（5个PG参与者，含leader）从原生54.219ms变为CustomScan 153.911ms；Gather结束从187.793ms推迟到435.142ms。其后HOST输入排序自身约289.6/290.1ms，聚合自身约61.8/62.5ms，基本不变，不能将两边共有的排序、DISTINCT或NOT IN归为新增开销。设备Filter占CSF累计CPU大头，字符串比较/LIKE仍走`query_vector.h`的逐值通用求值，不走`filter.c`的纯数值比较SIMD快路径；实际平均活跃也只有约1.1 CU。现有计时尚不能把所有供给/启动间隙归到某一个锁或函数。

Q21表现为HOST索引消费受阻，进一步使设备反压。与原生相比并没有增加EXISTS探测次数；关键差异是没有经HOST扫描填入基表页，破坏了本次查询内部的索引缓存复用。节点诊断如下：

| Q21 HOST节点指标 | 原生PG | filter-only |
| --- | ---: | ---: |
| l3索引探测次数 | 141445 | 141445 |
| l3 shared_buffers缺失页 | 16634 | 97838 |
| l3 shared_buffers命中页 | 551668 | 470464 |
| l3文件读取I/O时间累计 | 487.554ms | 6452.679ms |
| l2索引探测次数 | 12602 | 12602 |
| orders索引探测次数 | 7601 | 7601 |

原生l1扫描逐渐将lineitem Heap页放入shared_buffers及文件页缓存，后续l3可复用；filter-only的内部NVM读取没有将这些页带回HOST，只交付过滤后的列值，且该模式按定义关闭HPCP。冷启动只清空查询开始前的缓存，不禁止查询内复用。多出的81204次PG缺失块读取约合634.4MiB逻辑页访问；这里的页数不是NVMe命令数，I/O时间也包含多个PG参与者的并发，不与SQL墙钟相加。结果交付仍有额外成本，但不能用原生也存在的三组索引探测或设备约2秒的受反压源阶段，替代这个缓存差异解释。

修复前Q11/Q22虽有Seq Scan，仍被`src/planner/cost.c:estimate`的`eligible && (top || root->init_plans==NIL)`规则挡住：只要所在PlannerInfo有InitPlan，所有非根CSD子树都被跳过。Q11的HAVING阈值、Q22的AVG阈值都被PG编成`InitPlan 1 (returns $1)`。这条隔离HOST参数的旧规则连不使用$1的扫描一起拒绝；此外Gather下非parallel-aware的私有Hash输入仍按既有规则保留HOST，这与共享流不是可重放源有关。

当时确认的修复边界是区分扫描是否真正依赖HOST参数。Q11的partsupp扫描、Q22不依赖$1的输入可独立处理；Q22的`c_acctbal > $1`确实需要HOST参数绑定或保留该剩余谓词。不能只去掉判断：旧编译器会遍历并编译所在root的全部InitPlan，直接放开会把聚合/连接子计划偷偷塞入filter-only扫描程序。后续实现与验证见本文“Q11/Q22：按实际依赖下推扫描，绑定HOST InitPlan结果”。

## Q16复杂过滤、CU供给和并行启动优化

实现按谓词及投影形态选择，不判断SQL编号。参考DuckDB的`execute_conjunction.cpp`选择向量处理和`expression_heuristics.cpp`的便宜条件优先原则：在顶层WHERE合取中，先用现有数值比较及OR树筛选存活行，再将选择向量交给通用表达式求值。常量IN已由PG编译为OR树，无需新增opcode或参数；数值OR可用AVX2，NULL/NOT/不支持的形态仍保留精确求值，可能抛错的算术及substring不重排。LIKE的连续字面量前缀改为一次memcmp，通配符、转义和UTF-8字符仍由原匹配流程处理。

独立扫描的纯列返回，在支持的固定数值/字符串谓词过滤后直接复制保留Heap原值。原来Q16会把存活字符串写入共享字典，再取回用于HOST输出，造成CU间字典争用和重入；现在只保留两个不可变字符串字面量，行数据不进入字典。数值转换谓词、状态型IN集合及参数仍走原投影路径，通用表达式或内存回退时也恢复原路径。保留原AFDM/输出协议和生命周期。

进一步确认PG计划常量携带固定96节点容量：即使Q16仅有一个扫描节点，也序列化1093416B，再随并行计划传给PG参与者。现在Const只保留头部和实际节点（本例13320B），执行时补零恢复完整结构，再按原设备布局传输。文件映射和运行计时仍在运行时填写，不修改NVMe/SNIA或设备计划ABI。同时提前跳过本来就会被拒绝的并行IndexScan/IndexOnlyScan候选，避免先编译其过滤/SubPlan再放弃；其余卸载规则不变。

只运行正式Q16，每模式单独冷启动。最终关闭profiling记录为`results/tpch.mOqydc/`，五种路径均与HOST一致：

| 模式 | SQL耗时（ms） |
| --- | ---: |
| host-only | 631.431 |
| filter-only | 623.329 |
| csd-without-hpcp | 383.290 |
| csd-with-hpcp | 377.835 |
| SQLerate（完整CSD） | 439.631 |

filter-only比此前全量记录849.303ms低26.6%，但两轮HOST本身也有波动；当前只表明接近HOST，本轮约1.3%的领先不作为稳定加速结论。原型各步的中间成绩不作为验收结果。

独立诊断用修改前`tpch.YSbPWD`和设备优化后`tpch.rB9Fuw`，只比较相同插桩口径，不混入正式延迟：

| Q16设备/交付指标 | 修改前 | 修改后 |
| --- | ---: | ---: |
| Filter CPU累计 | 187.941ms | 35.919ms |
| Heap解码CPU累计 | 26.819ms | 27.265ms |
| CSF CPU累计 | 242.988ms | 73.310ms |
| 扫描pipeline墙钟 | 218.797ms | 34.476ms |
| 平均活跃CU / CPU利用率 | 1.11 / 27.7% | 2.12 / 52.9% |
| CSF调用 | 79 | 30 |
| HOST等待数据累计 | 857.696ms | 59.233ms |

扫描仍为200000行、返回29182行/1731802B，29次结果读取，4 CU、4任务槽、4MiB输入、FDM预留19536304B均不变。workmem实际峰值1287760B，spill为0；没有靠增加设备资源或减少结果换取加速。不能把累计CPU或多PG参与者的等待相加成SQL墙钟。

PG的布局和规划成本两项测试通过；设备向量过滤、表达式、延迟解码、64MiB分页过滤、普通类型返回及4/8 CU直接交付六项通过。覆盖稀疏IN选择跨向量边界、NULL/NOT/OR、LIKE前缀/转义/UTF-8、复杂过滤后的原值返回和资源释放。结束后FDM_USED=0，未运行其他TPC-H查询。

## Q11/Q22：按实际依赖下推扫描，绑定HOST InitPlan结果

不再因为所在规划上下文存在InitPlan就拒绝所有扫描。编译器检查候选扫描的qual、投影及索引条件，只登记真正使用、由HOST InitPlan产生的执行参数；外层逐行参数或内嵌SubPlan仍拒绝，不会将它们误当成独立常量。编译扫描时不遍历并编译root的InitPlan；HOST初始化中的独立扫描另按同一规则检查，其聚合、连接仍由PG执行。私有Hash输入与共享结果流的原限制保持不变。

参数登记在PG标准`custom_exprs`中，PG自己的初始化、并行依赖及重扫机制均能看到。PG owner在流启动时求值一次，按原类型转成`qs_cell`，附在计划AFDM后传入设备已有params数组；回调不访问PG，CU不调用HOST初始化。保留NULL和numeric的int128值/实际scale/显示scale，支持的标量类型见`planning.md`。EXPLAIN新增`CSD HOST Parameter Count`和`CSD Device Initializers`，分别确认绑定数量和没有夹带设备初始化子计划。

最终只对正式Q11/Q22各模式独立重启PG、清Guest page cache，关闭profiling，记录在`results/tpch.Ic7q9X/`；设备仍为4 CU、256MiB workmem、64MiB FDM，PG参数不变，十次结果全部与对应HOST一致：

| 查询 | host-only | filter-only | CSD无HPCP | CSD有HPCP | SQLerate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Q11 | 453.826ms | 468.324ms | 260.660ms | 264.802ms | 253.875ms |
| Q22 | 835.202ms | 536.040ms | 388.270ms | 365.153ms | 364.936ms |

Q11的main/InitPlan两次partsupp扫描都实际下推，各返回800000行，HOST聚合和HAVING保留；两条设备流参数数及初始化数均为0。Q22中HOST AVG继续保留，其customer输入扫描、主customer扫描及orders扫描下推；主customer扫描绑定1个HOST参数、返回19085行，平均值输入扫描返回38297行，orders返回1500000行。所有设备程序只含Scan，设备初始化数均为0，spill读写也均为0。

Q22 filter-only比同轮HOST低35.8%。Q11完成了功能修复，但单轮仍慢3.2%，不把它判为已获得稳定性能收益，也不归因于内存不足。独立开启设备/交付计时的诊断保存在`tpch.a4awxP`，不与正式延迟合并或取最小值。

PG布局、规划成本及已有数值边界入口通过，新增HOST numeric转换覆盖超过64位的正负数、scale、int128边界及NULL。设备新增`pgSinaHostParameters64MiB`覆盖精确参数、NULL、扫描/连接引用、截断输入拒绝与清理；原类型返回、CU直接交付、索引连接三项通过。会话内功能测试还覆盖根扫描保留InitPlan、多个参数及generic prepared plan多次重新绑定，并检查确实出现CSD扫描而不是回退；未新增测试SQL文件。最终FDM_USED=0。

其余22条只做现有EXPLAIN脚本的计划检查，未重跑全量基准；相关SubPlan结论见`planning.md`当前支持边界。
