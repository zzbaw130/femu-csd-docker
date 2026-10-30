# filter-only 执行与返回

该模式只卸载基表扫描、过滤和必要的列提取；Join、Sort、Aggregate 继续由 PG 执行，不按查询编号选择算法。扫描参考仓库 DuckDB `src/execution/operator/scan/physical_table_scan.cpp` 的全局扫描状态、线程局部状态和投影列处理，复用现有 CU 输入块任务池。

## 调用流程与职责

1. PG 规划器收集后续表达式、连接、分组和排序所需的列，删除无用的 Scan 投影，并更新直接消费者的位置引用；整行引用保留原输出。
2. 查询主进程配置 SNIA batch、申请一次设备流所需的 AFDM 并提交初始请求。实际分配路径仍为 `csAllocMem → SNIA FEMU 适配层 → NVMe 0xb0 → FEMU CSD`；PG 不分配设备物理内存，FTL 也不承担 AFDM 分配。
3. 已有 SNIA 请求线程调用完成回调，推进完成状态处理、后续 batch 和结果读取。CU 从共享输入任务池领取块。回调只使用不可变映射、libc 和 SNIA，不调用 PG API；复用 SNIA 已有请求线程池，扩展不自行创建线程、辅助进程、代理或服务。
4. 每个设备流有两个有界 HOST 结果批次。回调直接读入空批次，继续处理后续请求；缓冲满时在条件变量上休眠，HOST 取走一批后只通知空位，不提交 LOAD、不恢复 CSF。暂停的回调不占用 CU，已申请的 AFDM 仍属于该查询。该队列是有限反压，不承诺消除所有 HOST 消费瓶颈。
5. 独立扫描由查询后端直接消费。原生 parallel-aware SeqScan 的两个批次放在 PG DSM（并行执行共享内存）中，由查询主进程在启动 PG workers 前初始化。PG 原生 workers 只领取最多 256 行到各自本地结果缓冲，执行 HOST 上层算子；不打开设备、不申请 AFDM、不续交请求。没有选举“PG 提交者”，设备推进不依赖 leader 是否参与取行。PG 没有选择并行计划时也不额外创建 workers。
6. Gather 下需要每个 PG worker 完整重读的私有输入仍保留 HOST；并行索引扫描也保留原生分片/顺序语义。保留这些边界，避免把共享分片流误当成可完整重放的输入。原生 HOST 上层并行不能一概删除。
7. 多个同时存活的设备输入遇到真实 AFDM 分配失败时，释放未提交的分配，先缩小输入块，再降低活动 CU 槽数重试；不按 PG worker 数重复申请。

这些职责属于 PG 扩展和设备程序对标准 SNIA batch/callback 的使用约定，不是新增的 SNIA API。FEMU 仍只解释通用设备请求，不解释 SQL。

## 返回布局

CU 内部仍使用原有 `qs_cell`。CSD→HOST 边界复用已有 `qs_result` AFDM，按列保存实际类型：bool/int2/int4/date/int8/float8 分别使用 1/2/4/4/8/8 字节，每列附 NULL 位图；Numeric、text、varchar、bpchar 使用 4 字节偏移，正文放入既有 64KiB 变长空间。

变长正文使用 PG 原生 varlena 字节。无谓词、无计算且保持 Numeric 类型的顺序扫描投影直接保留 Heap Numeric 正文；其他 Numeric 结果在设备端编码。PG 可以引用本地结果缓冲中的 Numeric/字符串生成 TupleTableSlot，不再逐行重建和分配 Numeric。bpchar 保留 typmod 的补空格语义。设备指针不会传给 HOST，原始 Numeric 的临时设备引用在输入块复用前复制为返回字节。

128KiB 固定值空间按实际类型计算批次容量，变长空间满时提前发布；不足一批的输出可跨 CU 完成继续累积。NULL 位图随实际行数收紧。控制记录提供有效行数及变长长度，HOST 仅读取有效部分，单次传输上限 256KiB。完整卸载的通用执行器也使用该布局；已有单表聚合专用 CSF 保持原接口。

PG 并行消费者只复制领取范围内的变长值并重定位偏移，不复制整个字符串池。本地批次在这些行消费完之前不会复用，保证 TupleTableSlot 的 Datum 生命周期。规划器的 HOST 边界成本同步按类型宽度、变长估计和实际批次容量计费；不是把所有输出继续估成 32 字节 cell。

## 清理与观察

正常 EOF、提前结束、重扫和查询取消都先停止连续回调，再排空请求、关闭设备状态并释放 AFDM。PG 在 ERROR 时先解除 DSM 映射，随后才执行扩展的 ResourceOwner 回调，因此共享队列另注册 `on_dsm_detach`，在映射仍有效时完成清理；初始化尚未返回就被取消也适用。查询内存上下文删除时注销回调并回收私有队列。

EXPLAIN 的 `CSD Result Delivery` 标明回调驱动的双批次缓冲；不再有 `CSD Scan Producer PID`。共享设备流的返回字节、行数和请求数不能把 PG worker 的副本相加，`CSD Counter Scope` 会标明这一点。`CSD Workers` 是设备执行槽数，物理 CU 数由设备查询给出。

## 针对性验证

配置为 4 CU、256MiB workmem、64MiB FDM；HOST 为 4 worker 上限、2GiB shared_buffers、16MiB work_mem。只测试正式 `tests/guest/queries/` 中 Q6/Q10/Q13/Q17/Q18/Q21，每次每种模式均重启 PG、清除 Guest page cache，输出全部与同轮 HOST 一致，未运行完整 22 条基准。

| 查询 | HOST（ms） | filter-only（ms） |
| --- | ---: | ---: |
| Q6 | 1256.492 | 627.676 |
| Q10 | 2161.874 | 2402.109 |
| Q13 | 2766.909 | 1331.824 |
| Q17 | 3479.859 | 5246.678 |
| Q18 | 8388.209 | 9327.411 |
| Q21 | 1930.033 | 2333.643 |

记录在 `results/tpch.z2ozyJ/`（Q17/Q18/Q21）及 `results/tpch.H0gATq/`（Q6/Q10/Q13）。同轮完整 CSD 路径 Q6/Q10/Q13 分别为 536.133/2379.708/475.779ms，输出一致。

Q17 两遍 lineitem 的返回字节分别约 186.5/93.5MiB；Q21 返回 3793296 行、约 30.0MiB，两个 int4 不再以 64 字节/行返回。三条设备 spill 均为零。**Q10/Q17/Q18/Q21 仍慢于同轮 HOST，性能未达标**，不能归因为设备 workmem 不足。Q17 仍有两遍全量返回及 HOST 排序；Q18 保留 HOST 索引聚合的重复执行；Q21 的相关索引探测仍缺少原生扫描带来的缓存复用，详见 `results/filter-profile.DuNIro/summary.md` 的定位。回调推进解决其中的 HOST 取行反压耦合，不等于这些算法与缓存问题也已解决。

设备 `pgSinaPackedOutput64MiB` 覆盖类型打包、长字符串/NULL、并行排序，以及异步多 CU 的完整、不重复输出和旧确认位清理；`pgSinaIndexJoin64MiB` 验证索引连接。PG 的 `pgSinaPlannerCost` 验证类型容量、部分批次收紧、跨 NULL 位图的共享切片和变长偏移重定位，`pgSinaHeapLayout` 验证 Heap 布局；Numeric 模块的 14 个边界值用例通过。所有设备组件测试使用至少 64MiB workmem。

初始化阶段 100ms 和执行阶段 1s 的 Q17 取消检查已加入 `tests/guest/filter-only-test.sh`：原连接均能继续执行 SQL，另用只读 GET_INFO 核对 FDM_USED 为 0。关闭 leader 扫描参与后的 Q6 输出一致；临时功能 SQL 验证 Scan 实际重扫三次（25 行×3，总和 900）和 LIMIT 提前结束的 EXPLAIN，结束后 FDM_USED 同样为 0。这些是生命周期检查，不作为 TPC-H 性能验收结果。
