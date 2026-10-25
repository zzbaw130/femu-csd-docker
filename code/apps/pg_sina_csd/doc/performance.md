# 当前性能与未完成项（2026-10-25）

## 当前：SNIA异步请求复用工作线程

参照DuckDB工作线程持续取任务的方向，SNIA公共请求执行器改为应用进程内按需创建、复用线程；同步请求仍直接执行。工作队列不持锁执行请求或callback，所有线程忙于callback时仍可为新请求创建线程，支持callback等待异步子请求。公开API、插件契约、64个在途请求上限及取消/完成生命周期不变；不修改PG、设备SQL程序或固件。

[SNIA六项CTest记录](../../../../results/request-workers/unit-tests.log)覆盖C头文件、发现、两个设备插件路由、分配失败重试、batch与event生命周期和设备图布局。新增512次连续callback检查实际线程生命周期（不依赖可能复用的OS线程ID）：[两次mock微测](../../../../results/request-workers/callback-sequence.csv)从512个线程/27.535ms变为26个线程/7.071ms；它只证明线程复用及该微测开销下降，不代表查询加速比例。另有64个callback同时运行后执行同步请求、等待异步子请求、重配batch并续交的检查。

Guest libsnia.so已重编安装。[规范Q4/Q14/Q21冷启动回归](../../../../results/tpch.pRqPXw/timings.csv)均正确、整条卸载且快于同轮host，仍为8 CU / 256MiB workmem / 64MiB FDM，未重复完整TPC-H。

| 查询 | host ms | CSD ms |
| --- | ---: | ---: |
| Q4 | 2233.724 | 631.993 |
| Q14 | 1512.872 | 477.839 |
| Q21 | 2242.094 | 1081.191 |

[Q21取消与恢复](../../../../results/request-workers/cancel-recovery.log)：300ms超时后，同一连接、不重启PG/Guest再次执行规范Q21，100行全部匹配host；恢复查询不计入冷启动性能成绩，临时SQL已删除。

[Q4独立profile](../../../../results/request-workers/q4-profile.log)树墙钟966.777ms，worker累计CPU606.603ms、workmem峰值2561232字节、无spill；与前阶段相近。仍有2588次回调续交、2598次控制读取，续交前平均548.635μs，平均活跃CU约0.67。因此线程复用没有消除同步控制读取和跨请求往返，不宣称这些等待已解决或8 CU持续满载。

## 索引根页保留与连续LOAD

参照DuckDB缓冲管理保留反复访问页的方向，在设备SQL程序的既有每游标128页缓存中保留B-tree根页，其余页继续轮转，待LOAD的目标槽位不淘汰。不增加缓存页、AFDM或接口；游标仍满足原4KiB预算。PG、SNIA和固件无需修改。

现有缓存压力用例使用513个查找，覆盖超出缓存的256个Heap页：[读取计数](../../../../results/index-cache/cache-churn.csv)。根页读取4→1次，Heap读取同为466次，执行恢复18→15次。测试断言根页只读一次；索引、NativePlan和8/16/64 CU共6项CTest通过，含NULL、重复键、HOT重定向、待完成LOAD、取消清理和4MiB spill。pg-sina-plan.so与pg-sina-join.so已重编部署。

[规范Q4/Q17/Q21冷启动回归](../../../../results/tpch.goNOzf/timings.csv)均结果一致、整条卸载且快于同轮host，8 CU / 256MiB workmem / 64MiB FDM不变，未重复完整TPC-H。

| 查询 | host ms | CSD ms |
| --- | ---: | ---: |
| Q4 | 2461.263 | 664.124 |
| Q17 | 4684.642 | 1245.810 |
| Q21 | 2517.044 | 1126.114 |

Q4单独冷启动profile与上一阶段比较：[轮转缓存](../../../../results/group-vector-local/q4-hash.log)、[保留根页](../../../../results/index-cache/q4-root.log)。

| 指标 | 轮转缓存 | 保留根页 |
| --- | ---: | ---: |
| Index/Heap页加载 | 62619 | 62175 |
| 回调续交 | 3036 | 2582 |
| LOAD请求 | 3494 | 3047 |
| worker累计CPU | 617.476ms | 605.260ms |
| workmem峰值 | 2561104字节 | 2561232字节 |

两次均无spill。profile树墙钟639.268→1019.506ms，回调到续交前的平均时间180.618→504.104μs，存在较大推进等待波动；不能凭请求计数下降宣称稳定总延迟改善。普通成绩采用上方关闭profile的规范回归。保留根页减少了确定的重复读取和恢复，仍未解决跨请求推进的主要等待，当前平均活跃CU约0.63。

## Index Join接入并行哈希聚合

移除计划转换中排除Index Join的旧限制。它已有可接续列批次，现在与Hash Join一样，可以将分组排序移到聚合结果上方，直接进入现有并行Hash Aggregate sink。不新增设备算子、局部表或接口，不改变PG选定的索引连接和排序语义。

[规范Q4/Q13冷启动回归](../../../../results/tpch.1kdWx8/timings.csv)均正确、整条卸载且快于同轮host，8 CU / 256MiB workmem / 64MiB FDM不变。Q4 host 2822.046ms、CSD 689.592ms；Q13 host 1204.663ms、CSD 448.802ms。本阶段只修改PG计划转换并重新编译安装扩展，未重复完整TPC-H。

Q4单独冷启动profile：[原排序聚合](../../../../results/group-vector-local/q4-before.log)、[Index Join → Hash Aggregate → Sort分组结果](../../../../results/group-vector-local/q4-hash.log)。均无spill。

| 指标 | 原路径 | 哈希聚合路径 |
| --- | ---: | ---: |
| 排序累计CPU（sink＋source） | 84.217ms | 0.054ms |
| 查询workmem峰值 | 4649040字节 | 2561104字节 |
| 控制写入次数 | 553 | 1 |
| 控制写入累计墙钟 | 372.462ms | 0.054ms |
| worker累计CPU | 600.665ms | 617.476ms |
| pipeline墙钟 | 1432.989ms | 583.051ms |

共享聚合消化Index匹配结果后，worker不再为每批明细返回协调端，既有callback可以连续推进，因此不仅消除了明细排序，也减少了协调结果的控制写入。profile查询树1545.556→639.268ms，存在运行波动，不作为固定加速比例。新的聚合sink累计CPU20.511ms，平均活跃CU约1.06；索引读取与任务推进仍有等待，本轮不据此引入新的每CU局部表，也不宣称8 CU持续满载。

## 并行聚合归一化保留结果排序

参照DuckDB先更新分组哈希状态、再向下游交付分组结果的路径，修正PG计划归一化：原生Finalize GroupAggregate → Gather Merge → Sort → Partial HashAggregate被合并后，使用Hash Aggregate → Sort分组结果，保留原排序操作符、collation和NULL位置。此前丢失Gather Merge顺序，使Q15主路径为满足有序下游而退回明细排序。本阶段只修改PG计划转换，复用设备已有哈希聚合和排序，不改变SNIA、FEMU或资源容量。

[规范Q15/Q3/Q18冷启动回归](../../../../results/tpch.dPhaTL/timings.csv)结果一致、整条卸载且快于同轮host；没有重复完整TPC-H。

| 查询 | host ms | CSD ms |
| --- | ---: | ---: |
| Q15 | 2615.810 | 878.550 |
| Q3 | 2581.968 | 1496.805 |
| Q18 | 6971.519 | 1972.070 |

Q15单独冷启动profile：[修改前](../../../../results/aggregate-order/q15-before.log)、[修改后](../../../../results/aggregate-order/q15-after.log)。两次均8 CU、256MiB workmem、64MiB FDM、无spill。

| 指标 | 修改前 | 修改后 |
| --- | ---: | ---: |
| Sort Sink累计CPU | 58.170ms | 0.619ms |
| Sort Source累计CPU | 63.792ms | 2.662ms |
| 查询workmem峰值 | 25250832字节 | 4943968字节 |
| 排序run数 | 21 | 2 |

profile树墙钟2397.166→1084.822ms，但前序扫描/续交也有波动，不能把总差值都归于排序。普通延迟采用上方关闭profile的成绩；排序CPU和内存下降是本阶段直接证据，仍未证明持续8CU满载。

Guest构建及布局CTest通过。临时分组SQL额外检查ASC/DESC、NULLS FIRST与host逐行一致，两个计划均完整卸载（[计划](../../../../results/aggregate-order/order-plan.log)）；这些仅检查排序语义，不替代规范TPC-H验收，临时SQL已删除。

## 页请求自动续交与加载完成标志

PG扩展复用已有SNIA callback：独立worker完成后，回调读取页请求元数据、按固定extent映射构建LOAD→计算batch；有输出、EOF或共享表维护需求时交给协调端。回调只调用libc/SNIA，不调用PG API。主线程复用回调已读的完成记录，取消时停止续交并等待在途回调结束。SNIA仍只执行标准请求，FEMU没有SQL语义。

计算节点通过既有cparam1的QP_PAGES_INPUT位表示本批LOAD已完成，在设备SQL程序入口设置ready；cparam2和MR顺序不变，spill重入不重复设置。PG与pg-sina-plan.so须一起部署，固件无需重编或重启。这消除独立ready控制写入，但页请求读取和主机映射仍存在。

本阶段没有重复完整TPC-H。[规范Q2/Q8/Q17/Q18/Q21冷启动回归](../../../../results/tpch.yOFTVR/timings.csv)均结果一致、整条卸载且快于同轮host，8 CU / 256MiB workmem / 64MiB FDM不变；测量期间没有并行构建或单测。

| 查询 | host ms | CSD ms |
| --- | ---: | ---: |
| Q2 | 889.215 | 500.954 |
| Q8 | 2940.162 | 1013.789 |
| Q17 | 4119.599 | 1232.811 |
| Q18 | 7142.096 | 2189.292 |
| Q21 | 2284.412 | 1142.473 |

Q21诊断对照：[回调续交、仍独立写ready](../../../../results/page-callback.T4yAsn/q21-before-ready-flag.profile.log)、[加入加载完成位](../../../../results/page-callback.T4yAsn/q21-ready-flag.profile.log)。

| 指标 | 独立写ready | cparam1加载完成位 |
| --- | ---: | ---: |
| 控制写入 | 4137次 | 2次 |
| 控制写入累计墙钟 | 1042.135ms | 0.090ms |
| PG线程提交 | 21次 | 21次 |
| 回调提交 | 4133次 | 4141次 |
| 回调到下次queue调用前平均时间 | 619.294μs | 275.497μs |
| profile查询树墙钟 | 2000.271ms | 1683.594ms |

表中2次控制写入用于协调结果，并非ready。实际计算提交是PG线程与回调提交之和；请求改由回调发起不等于消失，不能只用PG提交21次宣称减少了计算请求。准备LOAD和完成记录读取的计数包含并发回调，时间不能直接相加为查询延迟。profile有扰动，普通延迟采用上方冷启动成绩；目前未证明持续8CU满载。

NativePlan及8/16/64 CU共4项CTest和Guest布局CTest通过。Index测试通过参数通知输入完成，普通Scan仍覆盖旧ready字段协议；新增4MiB Index排序用例确认发生spill，并在重入中保留原cparam1。[取消恢复检查](../../../../results/page-callback.T4yAsn/cancel-recovery.log)在Q21执行中触发300ms超时，随后同一连接、未重启PG/Guest，完整Q21的100行与host一致；该恢复查询不计入冷启动性能成绩，临时SQL已删除。

## 完整验收基线（a98575e1c）

- SF1冻结Heap，8 CU、256 MiB workmem、64 MiB FDM；未扩容FDM或改变CPU亲和性。
- 仅使用 `tests/guest/queries/` 的22条原SQL和对应explain，保留标准LIMIT和参数。
- 每次host/CSD测量前重启PG并清除Guest page cache，普通成绩关闭profile；诊断另行冷启动。
- 上一阶段共享数据布局和内存归属改完后运行一次完整验收，期间没有并行编译或单测。

记录：[timings.csv](../../../../results/tpch.9MVM0c/timings.csv)。22条结果一致、实际卸载且均快于同轮host，脚本退出0。逐条检查SELECT顶层是单个SniaCsd Custom Scan，设备查询均为8 CU和268435456字节workmem；Q15保留原CREATE/DROP VIEW，不将其DDL计时行当作查询执行节点。Q1/Q6使用专用聚合路径，其余使用PG选定计划的通用树。

| 查询 | host ms | CSD ms | host/CSD |
| --- | ---: | ---: | ---: |
| Q1 | 4713.222 | 811.277 | 5.81× |
| Q2 | 784.073 | 530.743 | 1.48× |
| Q3 | 2611.759 | 1735.771 | 1.50× |
| Q4 | 1897.290 | 966.744 | 1.96× |
| Q5 | 1958.889 | 820.130 | 2.39× |
| Q6 | 1392.928 | 430.406 | 3.24× |
| Q7 | 2284.016 | 1388.060 | 1.65× |
| Q8 | 2434.799 | 1188.488 | 2.05× |
| Q9 | 2962.355 | 1031.121 | 2.87× |
| Q10 | 2413.635 | 1456.660 | 1.66× |
| Q11 | 425.339 | 236.858 | 1.80× |
| Q12 | 2163.736 | 720.589 | 3.00× |
| Q13 | 1196.084 | 429.509 | 2.78× |
| Q14 | 1518.545 | 404.008 | 3.76× |
| Q15 | 2264.723 | 874.491 | 2.59× |
| Q16 | 700.822 | 481.125 | 1.46× |
| Q17 | 4280.112 | 1154.183 | 3.71× |
| Q18 | 7773.746 | 2472.319 | 3.14× |
| Q19 | 1739.686 | 620.024 | 2.81× |
| Q20 | 3539.305 | 1341.091 | 2.64× |
| Q21 | 2197.327 | 1248.988 | 1.76× |
| Q22 | 620.050 | 354.631 | 1.75× |

合计 **51.872s → 20.697s，2.51×**。这是一轮结果，不能保证固定加速倍数；没有测量DuckDB延迟。

## 当前字典的内存归属

通用树不再为运行时字典保留大块AFDM。PG根据物理Heap、所需字符串列和常量提供既有容量上限；设备SQL程序在根查询workmem中按实际插入增长。AFDM只保留一页配置，不传进程指针；分配器函数仅在设备内部建立。FEMU和SNIA仍只处理通用执行、I/O、CU及spill完成值，不解释字典或SQL。

多CU共享稳定的字符串ID。已有值查询不持插入锁；增长时在原插入锁内建立新存储并发布，旧存储保留到查询结束，使旧字节视图仍有效。旧、新存储都计入查询总预算，不能用FDM绕过workmem。预编码输入导入后保留原ID，Join、分组、排序及spill行里的ID不变；分配失败不破坏已有数据。

`CSD Dictionary Entries/Bytes`是实际条目/文本字节，`CSD Dictionary AFDM`是配置页；`CSD Workmem Peak`包含字典增长和保留的旧存储，也包含查询其他算子的实际分配。

## 字典迁移阶段：解除Q21的AFDM槽位限制

[a98575e1c阶段profile](../../../../results/workmem-dictionary.Yk17wP/q21.profile.log)与固定字典布局对照：

| 指标 | 原AFDM字典 | 当前workmem字典 |
| --- | ---: | ---: |
| 字典AFDM | 约32MiB | 4096字节配置 |
| 实际字典 | 未报告 | 377项 / 6724字节文本 |
| 工作槽 | 6 | 8 |
| 读取块 | 1MiB | 4MiB |
| 总AFDM | 64799288字节 | 66768152字节 |
| Index/Heap页读取 | 65577 | 48412 |
| 主机提交 | 5746 | 4140 |
| 平均活跃CU | 1.40 | 2.60 |

原布局profile为 `results/index-columns.T4R3iT/q21-sized-dictionary.profile.log`，设备资源相同。释放字典预留后，空间被8个工作槽和更大的输入块使用，总AFDM仍在64MiB以内。当前workmem峰值7355872字节、无spill、CU同时活跃峰值8；平均2.60仍不是持续8CU满载。当前独立profile查询树1226.854ms；普通成绩用完整轮的1248.988ms，不混用计时。

## Q18：字符串构建等待改善

[78d26eb8e阶段profile](../../../../results/dictionary-wait.kOicWe/q18.profile.log)与[字典迁移阶段profile](../../../../results/workmem-dictionary.Yk17wP/q18.profile.log)使用相同规范SQL、单独冷启动，保持8工作槽、4MiB读取块。

| 指标 | 原等待方式 | 当前等待方式 |
| --- | ---: | ---: |
| customer构建流水线墙钟 | 846.351ms | 338.897ms |
| customer累计CPU | 2935.097ms | 1186.894ms |
| Scan解码累计CPU | 3882.703ms | 2096.852ms |
| 总CSF累计CPU | 9297.234ms | 7551.785ms |
| 查询树profile墙钟 | 2471.975ms | 1968.859ms |

两者均为150000个字符串/2700000字节文本、查询workmem峰值205415424字节（约196MiB）、无spill；所以改善不是内存扩容或减少读取数据。此前忙等不断交换同一锁缓存行，即使持有者仍在增长/插入，等待CU也反复写入；共享读取等待减少了这些竞争。上一阶段8线程插入150000个18字节字符串的本地微测，中位累计CPU从3881.735ms降至1947.642ms，见[记录](../../../../results/dictionary-wait.kOicWe/dictionary-bench.csv)。该微测与查询阶段CPU同时下降，支持其为实际瓶颈之一，不证明全部等待已消失。

该阶段普通成绩2025.607ms记录在results/tpch.6kQo9K；完整基线2472.319ms来自a98575e1c，不能把不同样本的差值当作固定加速比例。共享字典仍串行发布新值，平均CU使用率也不能仅凭墙钟缩短就断言更高。

## 已实现的执行边界

PG保留原选定Join树，做有语义条件的局部rewrite；SQL程序执行Heap解码、列向量表达式、匹配及聚合，固件没有SQL节点。Heap解码窗口2048行、表达式批次256行，与4/2/1MiB读取块分开。Scan/Hash/Index/Project及支持的聚合可以接续所需列，跨I/O保存输入与匹配位置；Index的查找批次边界不能凭远端Scan耗尽来推断。

无过滤Scan的非键字符串可延迟到Hash匹配后物化。中间列保留int128、每值scale/dscale和NULL；统一scale、可安全窄化时沿用SIMD。后续行错误延迟到消费时报告，LIMIT不提前观察未来结果；聚合探测消费最终结果。父分组不在阻塞子节点完成前按估计预占大表。

## 验证与剩余工作

字典迁移阶段21项相关设备CTest及Guest布局CTest通过：并发字典增长/重复插入/旧字节视图/失败后读取与回收，预编码ID导入，4MiB总预算spill，NativePlan和8/16/64 CU，表达式、Filter、Scan、聚合、Sort/Hash/Index Join。PG及共享布局涉及的全部设备SO已重新构建部署。

Index右侧投影与Join条件、Merge Join、部分多聚合分组及排序边界仍有逐行适配；页请求虽可由回调续交，仍需读取元数据和主机extent映射；部分阻塞推进仍依赖协调端。共享字典保留原容量上限，自身尚无spill；真正耗尽预算或上限会返回错误，不能宣称任意大小字符串查询及低内存形态全部覆盖。共享字典串行写入、全链路列化和整体CU利用率仍有优化空间，完整基线22条加速不代表这些基础改造已全部完成。当前证据不要求扩容FDM。
