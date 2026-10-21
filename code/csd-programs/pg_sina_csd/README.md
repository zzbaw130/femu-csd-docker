# pg_sina_csd 设备算子

这里的 SO 在容器内编译，由 FEMU CU 执行，逻辑上属于 CSD CPU。Guest PG 扩展和 SNIA 编排代码在 `code/apps/pg_sina_csd`；FEMU 不解释 SQL，没有 SQL 专用 runtime，也没有查询辅助进程。

## 流水线与参数

Guest PG 从表达式树生成计划，直接调用 SNIA。当前支持以下流程：

```text
LOAD → Scan → Filter（可选）→ Aggregate.sink
Aggregate.source（小哈希合并，或外部部分状态归并；按需排序结果及LIMIT）

LOAD → Scan → Filter（可选）→ Sort.sink
Sort.source（多路归并、OFFSET/LIMIT，分批输出）
```

第一条流水线按最多4个独立执行槽并行，每槽一个 SERIAL batch；完成的槽立即领取下一块。第二条流水线等待全部输入完成。通常每块4MiB原始heap，同一槽没有下一块预取或双缓冲。

| 源文件 / 产物 | 入口与 MR 参数顺序 |
| --- | --- |
| scan.c / pg-sina-scan.so | scan：原始页、计划、列批次、分支任务（状态句柄、有效字节、实际执行槽数） |
| filter.c / pg-sina-filter.so | filter：列批次、计划、原始页；生成selection并物化最终存活行的延迟列 |
| aggregate.c / pg-sina-aggregate.so | 无分组sink：列批次、计划、分支任务；source：状态句柄组、结果；GROUP BY时sink/source均追加spill I/O AFDM |
| sort.c / pg-sina-sort.so | pg_sina_sort：小聚合结果、计划；仅排序聚合结果 |
| sort.c / pg-sina-sort.so | pg_sina_sort_sink：列批次、计划、分支任务、spill I/O AFDM |
| sort.c / pg-sina-sort.so | pg_sina_sort_source：局部状态句柄组、结果、spill I/O AFDM |
| limit.c / pg-sina-limit.so | limit：小聚合结果、计划；调整结果范围 |
| heap_count.c / pg-sina-heap-count.so | 诊断：原始页、计数；cparam1为物理列数，cparam2为有效字节 |

`apps/pg_sina_csd/include/query.h` 是本应用的数据布局，不是新的设备协议。参数按表中顺序构造，没有参数解码框架。结果块最多256行，用finished表示source结束；行排序总行数不受这个块容量限制，分组聚合也支持跨块输出，256仅是局部哈希表容量，满表会flush部分状态。

Aggregate.source 的单MR调用释放私有状态。Sort.source 的单MR调用也仅释放私有内存，随后PG关闭本查询独占的SNIA设备，回收剩余临时token；这允许失败batch已经回滚部分token。Sort.source 的双MR调用（句柄组、I/O）可通过NEED_FREE重入主动清理全部token和内存。正常source读到末尾会先释放token，再销毁状态，最后一个结果块仍保留在AFDM。

## 列批次、向量和精度

Scan解析PG12/x86-64/8KiB、全visible/frozen的heap，仅提取所需列。列批次包含int64值、NULL位图、numeric显示精度和selection vector；仍须遍历变长属性边界，不减少heap页读取量。

numeric输入必须有限、至多两位小数，可精确表示为int64百分数；支持整数、date、bool、ASCII bpchar(1)，尚无通用varchar/text。规划时已知不支持的能力回退PG；运行时数据超出设备表示范围时报错，不伪装成透明回退。

PG估计过滤后剩余不足一半行时，所需numeric可以延迟解码，包括WHERE使用的列。Scan将原始AFDM中的`{offset, bytes}`暂存在原8字节值槽。简单AND先用SIMD处理已解码列，再按存活行逐个判断其余列，同列相邻上下界共用一次解码；复杂AND/OR只把仍需判断的行传给后续表达式。最终存活行的所有延迟列会在Filter中转换为正常列值，因此下游无需理解原始位置。原始AFDM一直保留到当前槽sink完成。

`query_vector.h` 每批最多256行。列直接引用原列和selection，常量只保存一份，计算结果才写临时列；公共表达式按行缓存，保留NULL三值逻辑、短路和溢出语义。无NULL/错误批次可用AVX2执行比较、加减和int32×int32→int64乘法；加减检查溢出，其余情况回退精确int128。没有浮点近似或JIT。

Aggregate批量计算表达式和分组位置，再按输入列更新；同表达式SUM/AVG/COUNT共享sum/count，无分组COUNT(*)按批增加。AVG在设备上按PG numeric精度和舍入规则完成，Guest只转换最终Datum。小聚合结果Sort只交换行号，最终统一复制；Limit只调整结果范围。

## 外部行排序与 TopN

Sort.sink参考DuckDB的局部排序和TopN堆：OFFSET+LIMIT能放进一个有序段时，每个执行槽只保留最优的这些行；否则生成最多1MiB的有序段。按实际输出列数紧凑存储完整qs_cell，不为每行固定预留16列。段和表达式工作区全部来自共享`libcs-workmem`；每槽上下文按实际执行槽数限额，优先保留内存段，下一段将超出预算时请求spill；分配失败仍作为碎片或共享池压力下的补救。

spill使用64KiB I/O块，一个有序段包含多个块，不能把块数当作归并段数。sink将块复制到最后一个I/O AFDM并返回`csNeedStore(bytes)`；SNIA内部batch调度器走设备AFDM→NVM短路径，重入后将token保存在私有段描述中。段头始终缓存在workmem，source用小根堆归并，需补数据时返回`csNeedLoad(token)`；I/O重入仅确认前一次请求。读缓冲紧张时释放已有NVM副本的读缓冲，保留段头，之后按需重载，不扩大workmem。

输入heap AFDM在Scan/Filter结束后可复用为sink的I/O暂存；列批次在另一个AFDM，私有状态没有放入FDM。source输出和I/O也使用不同AFDM。设备算子不调用NVMe、不持有Guest文件描述符、不知道GPT或SQL元数据；token分配与内部LOAD/STORE由SNIA处理。

## 可选计时

`pg_sina_csd.profile`默认关闭。计时集中在`profile.c`入口包装，不在算法行循环中读时钟。开启时cparam1=1，增加一个qs_profile输出AFDM；普通入口将它附加在末尾，支持spill的Sort和分组Aggregate的sink/source将它放在最后一个I/O AFDM之前，以保持SNIA的“最后AFDM为I/O缓冲”约定。包装层移除计数MR、清零cparam1后调用算法，保留cparam2的重入信息。每个执行槽独占计数输出。

Sort.sink/source分别计时，每次I/O重入也算一次调用。累计CU CPU/墙钟时间不是查询延迟；CPU是FEMU宿主线程时间，不代表真实SSD处理器周期。Filter计时包含延迟解码，分析时应合计Scan/Filter成本。关闭时不申请计数AFDM、不读时钟。

## 编译与验证

```sh
cmake -S code/csd-programs -B code/csd-programs/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build code/csd-programs/build -j2
ctest --test-dir code/csd-programs/build -R pgSina --output-on-failure
```

`tests/sort.c`在4/8/16/64/128MiB工作内存及64MiB池内4MiB查询预算下模拟STORE/LOAD/FREE，覆盖四槽、超过内存的数据、归并读缓冲压力、分批结果、NULL/显示精度、TopN/OFFSET、提前结束及失败scope已释放token后的内存清理。其余测试分别检查真实heap解码、向量/标量差分、局部聚合合并和计时。

真实Guest使用`tests/pg-sina-csd-test.sh query`逐条冷启动并核对PG/CSD结果；`order-bench`对SF=1的TopN及外部行排序各测3轮，验证计划确实卸载，并另外采集profile JSON。细节和阶段数据见`apps/pg_sina_csd/doc/`。

查询预算由Guest PG执行时写入`qs_plan.work_mem_bytes`，单位字节，表示一个执行槽的份额；0供独立算子调用使用，表示按实际槽数分摊设备池。sink将份额与设备池容量/槽数取较小值，扣除Filter临时列的保留空间，再通过`cs_work_begin_limit`限制自己的全部私有分配。Filter的临时上下文同样限额，所有分配仍经过同一共享`libcs-workmem`，不绕过池硬上限。小结果Sort在聚合状态回收后运行，也遵守份额。Scan直接解析输入AFDM，无动态私有工作区。

外部排序为下一份run保留64KiB归并空间和4KiB分配开销，不按剩余容量百分比提前spill。预算较小时自动缩小run，较大时可保留更多run；每份run最多1MiB，与spill的64KiB传输块是不同概念。当前run描述和表达式工作区不可spill，极小预算或极多run仍可能报内存不足，多轮归并尚待后续扩展。


## 分组聚合的内存与部分状态

`aggregate.c`同时实现局部哈希预聚合和source状态合并。局部表满时，先将已有部分状态交给私有排序器，再重试当前向量批次；批次只有在确定能插入全部键后才更新累加器，I/O重入不会重复计数。每份部分状态记录先放分组键，后放每个聚合的32字节`{sum, count, dscale}`记录；这些载荷单元不参与排序比较。相同表达式的SUM/AVG/COUNT复用其实际累加状态，AVG在合并后统一计算。

部分状态排序直接调用编译进同一`pg-sina-aggregate.so`的`sort.c`私有算法函数，私有记录在workmem中传递；这是函数复用，不是将私有指针作为NVMe AFDM参数，也不是运行额外CSF或CU。对外MR仍遵循表中顺序。单MR source清理会同时回收部分状态排序与结果排序的内存；正常结束通过已有重入协议回收token。

source保留一个正在合并的组及一块部分状态输入。没有SQL排序时可分批输出；有ORDER BY时将完成的组传给结果排序器，最后由结果排序source输出。部分状态排序和结果排序各有预算，不能各用完整查询上限。小结果能在原哈希表合并时跳过外部排序；无分组聚合保留原路径。PG对分组数的估计不会决定是否具备spill能力。

归并读缓存起初保留完整64KiB块，适合连续读取。检测到反复提前丢弃尚未消费完的缓存后，按该上下文的剩余预算和spill run数缩小窗口；每次内部LOAD仍为64KiB，只将当前窗口复制到workmem。这样避免在顺序访问中无条件缩小缓存，也减少交错run在低预算下的反复加载。多轮归并尚未实现，不能保证任意数量run都能驻留元数据。

`tests/aggregate.c`用独立sum/count预期值核对超过32768组的全结果，覆盖多槽、NULL键、显示精度、加权AVG、按聚合结果排序、LIMIT/OFFSET、空输入、跨256行输出、提前关闭和失败scope回收。分别测试4/8/64MiB池与64MiB池内4MiB查询预算；`aggregate-bench`在真实Guest运行独立冷启动host/CSD对照。
