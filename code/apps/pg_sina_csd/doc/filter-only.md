# filter-only 执行与返回

该模式只卸载基表扫描、过滤和必要的列提取；Join、Sort、Aggregate 继续由 PG 执行。实现不根据查询编号选择算法。扫描参考仓库 DuckDB `src/execution/operator/scan/physical_table_scan.cpp` 的全局扫描状态、线程局部状态和投影列处理，复用现有 CSD 输入块调度。

## 调用流程

1. PG 规划器收集后续表达式、连接条件、分组键及排序键所需的基表列，删除 Scan targetlist 中无用的列，并更新直接消费者的位置引用；整行引用保留原输出。
2. 独立顺序 Scan 可以使用多 CU。PG 原生 parallel-aware SeqScan 保留 Gather、Partial Aggregate 等并行结构，但整个共享扫描只有一套 SNIA 会话、AFDM 和 CU 执行状态。首先执行扫描的现有 PG 参与者负责提交；其余参与者只消费返回行，不另开设备流，也不新增进程或代理。
3. 负责提交的参与者通过现有 SNIA batch 驱动 CU；CU 完成后继续领取同一输入块池中的任务。返回行发布到 PG 原生 DSM（并行执行共享内存）中的一个有界批次。PG 参与者每次领取最多 256 行，复制到各自已有的结果缓冲，再由 HOST 上层算子处理；批次暂时为空时用 PG 条件变量等待。
4. Gather 下非 parallel-aware 的私有输入留在原生 PG。例如每个 PG worker 独立构建的 Hash 输入需要完整重读，不能从共享扫描中各取一部分。并行索引扫描也保持 PG 原生分片/顺序语义。
5. 同时存活的多个设备输入遇到真实 AFDM 分配失败时，释放未提交的分配，缩小输入块/活动槽后重试；使用与其他设备流相同的布局降级规则，不再按 PG worker 数重复预留，也不强制最少两个 CU 槽。
6. CU 继续输出已有 `qs_cell`，协调端只紧密排列实际投影列。同一块 4096-cell 结果空间可容纳 `floor(4096 / 输出列数)` 行，长字符串同时受 64KiB 字符串空间限制。设备控制记录带回有效行数和字符串长度。PG 按有效长度读取，每次传输最多 256KiB；完整卸载也使用该返回方式。
7. PG 直接生成 Numeric 的 base-10000 表示，随后生成 TupleTableSlot。`qs_result` 是本项目结构，不是 PG 原生元组。

设备协调 CU 的“已消费批次”位图只确认当前提交消费的 CU 批次。新提交在初始化/恢复字典之前清零，即使字典忙而提前返回，也不会重复确认旧批次；同一次设备内部 I/O 重入则保留累计位图，直到主机收到该提交的完成。这属于 SQL 设备程序与 PG 调度器之间的私有协议，SNIA 和 FEMU 不解释 SQL 批次语义。

正常结束和重扫复用现有关闭流程；查询取消/错误时，PG 的 ResourceOwner 释放回调先排空 SNIA 请求、关闭设备算子并释放 AFDM。该回调不访问 DSM，避免 PG 已解除共享内存映射后的悬空访问；查询内存上下文删除时注销回调。不能只依靠内存上下文删除，因为报错退出的并行 worker 未必执行该删除。

EXPLAIN 的 `CSD Scan Producer PID` 标出共享扫描的唯一提交者；`CSD Counter Scope` 说明计数范围。共享扫描的返回字节、请求数和行数是该设备流的总数，不能把 PG worker 的 EXPLAIN 副本相加。`CSD Workers` 是设备执行槽数；物理 CU 数仍由设备查询给出。

## 针对性验证

Guest 执行 `sh /home/femu/tests/filter-only-test.sh 查询编号...`。每条每种模式先重启 PG、清除 Guest page cache，再执行 `tests/guest/queries/` 原查询并与 HOST 输出比较。

设备 `pgSinaPackedOutput64MiB` 覆盖紧凑返回、长字符串/NULL、并行排序，以及协调 CU 与扫描 CU 异步执行时的完整、不重复输出和旧确认位清理。旧代码在异步用例中会触发确认位未清理断言；修复后通过。`pgSinaIndexJoin64MiB` 验证索引连接不受影响。PG 的 `pgSinaPlannerCost` 和 `pgSinaHeapLayout` 继续验证规划与共享布局。

本轮仅运行 Q6/Q10/Q13/Q17/Q18/Q21，未运行完整 22 条基准。配置为 4 CU、256MiB workmem、64MiB FDM；每次测量均冷启动。记录在 `results/tpch.qanc3k/`（Q6）、`results/tpch.vTlbaB/`（Q10/Q18）、`results/tpch.Df4auw/`（Q13/Q17/Q21），全部输出与本轮 HOST 一致。

| 查询 | HOST（ms） | filter-only（ms） |
| --- | ---: | ---: |
| Q6 | 1277.209 | 642.791 |
| Q10 | 2160.220 | 2867.257 |
| Q13 | 2608.542 | 2278.607 |
| Q17 | 3477.851 | 11223.336 |
| Q18 | 8901.717 | 10083.833 |
| Q21 | 1953.015 | 4468.983 |

另外，Q6 完整 CSD 路径为 512.594ms，结果与 HOST 一致。关闭 PG leader 扫描参与后触发 100ms 查询取消，设备 GET_INFO 的 FDM_USED 在取消前后均为 0；取消后连接可继续查询，正常查询结束后也为 0。

Q17/Q21 不再出现 AFDM 分配失败，Q10/Q18 不再超时。Q10 的 customer 扫描完整返回 150000 行；Q18 的 orders/customer 分别返回 1500000/150000 行，补查了 TopN 结果可能掩盖的扫描丢行问题。

**功能通过与性能合格分别判断。** Q10/Q17/Q18/Q21 的 filter-only 仍慢于本轮 HOST，设备没有 spill，性能未达标；本阶段解决资源重复预留和批次确认错误，不能把剩余的明细返回、PG 元组转换及 HOST 消费链开销归因于 CSD workmem 不足。
