# pg_sina_csd 设备算子

这里的 SO 在容器内编译，由 FEMU 的 CU 线程执行，逻辑上属于 CSD CPU。Guest 中的 PG 扩展和 SNIA 调度代码位于 `code/apps/pg_sina_csd`；FEMU 不解释 SQL，也未增加 SQL 专用 runtime。

## 执行流程

PG 从表达式树生成列投影、过滤、聚合、排序和 LIMIT 描述，Guest 的 PG backend 直接调用 SNIA 提交 batch：

```text
LOAD → Scan → Filter（可选）→ Aggregate.sink     # 每个执行槽一个SERIAL batch，多个batch异步并发
                   ...重复读取后续 chunk...
Aggregate.source → Sort（可选）→ Limit（可选）  # 合并局部状态，SERIAL batch
```

每个执行槽通常处理4MiB原始页，拥有独立输入缓冲、列批次和聚合状态，不固定绑定CU。Guest PG为每槽异步提交一个SERIAL batch，通过共享完成事件为先完成的槽分配下一块；SERIAL仅约束槽内顺序，各槽可并发。SNIA负责batch内执行，CSD SO仍只处理各自输入输出。原输入保留到本槽sink完成才复用，不增加双缓冲或预取；聚合source仍等待全部输入完成。

| 源文件 / 构建产物 | 工作及 MR 参数顺序 |
| --- | --- |
| scan.c / pg-sina-scan.so | MR[0]原始页、MR[1]计划、MR[2]投影列批次、MR[3]分支状态与有效输入字节数 |
| filter.c / pg-sina-filter.so | MR[0]列批次、MR[1]计划、MR[2]原始页；生成 selection vector，再解码延迟列，不复制被选中的行 |
| aggregate.c / pg-sina-aggregate.so | sink：MR[0]列批次、MR[1]计划、MR[2]分支状态；source：MR[0]所有局部状态句柄、MR[1]最终结果 |
| sort.c / pg-sina-sort.so | MR[0]最终结果、MR[1]计划；按多列方向及 NULL 顺序对行号堆排序，最后统一重排结果 |
| limit.c / pg-sina-limit.so | MR[0]最终结果、MR[1]计划；设置结果范围，支持 OFFSET |
| heap_count.c / pg-sina-heap-count.so | 阶段0诊断：MR[0]原始页、MR[1]计数；cparam1物理列数、cparam2有效输入字节数 |

`query.h`（位于 apps/pg_sina_csd/include）定义本扩展的应用级输入输出布局，`query_vector.h` 提供 Filter/Aggregate 共用的批量表达式计算，`query_eval.h` 共享精确整数运算、比较和AVG规则，并保留逐行求值函数供差分测试。参数均按上述顺序构造，不新增设备参数解码机制。source 还接受仅有 MR[0] 的清理调用，用于错误或取消时释放尚未完成的局部状态。

## 列批次与内存

Scan 只解码查询需要的列；跳过其他列的内容，仍须遍历 heap 的变长属性边界。输出为列式 int64 值、NULL 位图、小数显示精度和 selection vector。对于 PG 估计过滤后剩余不足一半行的查询，规划时将不在 WHERE 中出现、仅用于分组或聚合的 numeric 列标为 deferred。Scan 不转换这些值，而是把 `qs_heap_ref {offset, bytes}` 存在原有8字节列值槽中，位置相对于当前块的原始 AFDM。Filter 的 MR[2] 引用该原始输入，筛选后仅为 selection vector 中的非NULL行解码，并将原位置记录替换为正常列值和显示精度；Aggregate.sink 看到的仍是正常列批次。该输入在整个分支完成前不会被下一块覆盖，无须额外读取、缓冲或设备命令。无延迟列时，Filter 也接受只传前两个 MR 的调用。

`heap_numeric.h` 仅共享 Scan/Filter 的 PG numeric 解码函数，避免复制实现；不包含调度逻辑。`tests/scan.c` 比较提前/延迟解码，覆盖跨页、短/普通 numeric、NULL、显示精度、SIMD和复杂条件回退、批次复用及被过滤值不进入解码。

输入 numeric 使用百分之一为单位，表达式及 SUM/AVG 使用带溢出检查的整数运算；内部 `__int128` 仅用于精确计算，不是 NVMe 标量参数封装。AVG 在设备上完成除法及符合 PG numeric 的精度选择与舍入，Guest 只转换最终结果为 PG Datum。

AFDM 保存输入、输出、计划和不透明状态句柄；真正的哈希表和排序工作区通过 `libcs-workmem` 分配。sink 首次运行创建私有上下文，source 合并后销毁；取消路径也调用 source 清理。所有 SO 通过根目录 `add_csd_program` 统一链接同一份 workmem 库。64MiB AFDM 与64MiB workmem 是两项独立限制。

当前针对 Q1/Q6：PG12、x86-64小端、8KiB页、完全 visible/frozen heap；numeric 输入必须有限、至多两位小数且可精确表示为 int64 百分数。支持整数、date、bool、ASCII bpchar(1)；分组最多256组，最多4个 CU。这里的 Sort 只排序聚合后的少量结果，尚非可 spill 的通用行排序。超出已知规划能力回退 PG；数据值超出上述设备表示范围时会报错，不能将这种情况视为已实现透明回退。

## 编译与测试

```sh
cmake -S code/csd-programs -B code/csd-programs/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build code/csd-programs/build -j2
ctest --test-dir code/csd-programs/build -R pgSina --output-on-failure
```

真实 PG heap、冷启动结果对照和 EXPLAIN 检查由 `tests/pg-sina-csd-test.sh query` 执行。设备算子不调用 NVMe，不持有 Guest 文件描述符，不查询 SQL 元数据。

## 可选入口计时

PG 的 `pg_sina_csd.profile` 默认关闭。`profile.c` 是查询算子共用的计时入口：CMake 仅在编译时将算法入口命名为 `_impl`，对外导出的原入口先判断是否计时，再调用算法。Scan/Filter/Aggregate/Sort/Limit 功能源码不加入时钟代码。

本扩展的调试参数约定：关闭时 cparam1=0，MR 顺序与上表完全相同；开启时 cparam1=1，在原 MR 之后追加一个 `qs_profile` 输出 AFDM。包装入口记录调用期间的 CLOCK_MONOTONIC 与 CLOCK_THREAD_CPUTIME_ID，移除附加 MR、清零 cparam1 后调用原实现，保留返回值及调用者参数。每个并行分支独占一份计数输出，分支内顺序运行，无须原子计数；收集后由 PG 合并。第二条流水线继续使用第一个分支的计数输出。

这是 PG 查询程序自己的可选输入输出约定，不是新 SNIA API，不修改 femu_csd_args。阶段0的 heap_count 参数约定不变，不接入这个开关。CPU/墙钟时间均为 FEMU 宿主线程测量，开启计时本身有时钟和小量输出传输开销；正常测量或发布时关闭即可。Filter 的计时包含筛选后的延迟解码；比较解码收益时需合计 Scan/Filter CPU，不能单看 Filter 是否变慢。

## 列向量过滤

Filter 参考 DuckDB 的类型化比较和 selection vector 组织：每块输入先将单条件或复合 AND 条件中的列/常量比较归一化，常量精确转换到列的定点单位，再批量比较列值。支持 AVX2 的宿主一次处理4个 int64，合并 NULL 位图后输出行索引；尾部和无AVX2机器使用类型化标量循环。过滤比较路径继续使用现有4个CU分支；随后按上述约定补解码延迟列。

比较支持 =、<>、<、<=、>、>= 及反向常量比较；单谓词也使用该路径。OR/NOT、NULL常量、计算表达式、不能无损归一化的阈值等由下面的批量表达式计算处理。`tests/filter.c` 将结果与原逐行精确求值器对照，覆盖符号边界、NULL、定点精度和批次尾部。

## 过滤、表达式与聚合的批量计算

`query_vector.h` 的处理单位是256行。一个表达式对应一组数值、NULL、显示精度及错误标记数组；数组在 workmem 中，而非 AFDM。PG 已按子表达式在前、父表达式在后的顺序构建表达式表，设备只标记当前算子用到的节点，按该顺序一次处理一列。公共子表达式在同一批内仅算一次，没有JIT、额外执行协议或配置项。每个表达式的临时列占4864字节，128个表达式上限为608KiB；Aggregate在其已有私有上下文中分配并跨块复用，Filter的复杂表达式在本次调用结束释放。

列循环使用精确整数和溢出检查；运算类型判断从逐行递归移到表达式外层。AVX2用于列/常量过滤；int128加乘仍是标量机器指令，不宣称所有向量循环都使用SIMD。AND/OR以每行错误标记保留短路：未被该行使用的右分支即使溢出，也不会使该行报错；需要使用的错误会传到当前表达式输出。

Aggregate先为本批计算分组键、聚合输入和分组位置，然后按输入列累加；同一表达式的SUM/AVG/COUNT共用sum/count，不再重复更新，source分别完成各输出的精度和AVG除法。COUNT(*)单独处理，无分组时直接加本批行数。分组哈希查找仍需逐行定位，未引入新哈希算法。Sort只交换16位行号，结束统一复制每个结果行；Limit仍仅设置范围。

Scan仍按PG heap变长属性边界解析元组；常见零至三个base-10000数字、weight为-1/0/1的numeric直接转为int64百分数，其他合法表示保留通用解码路径。未减少必须读取的heap页，也未增加缓存。

实现借鉴本地DuckDB的ExpressionExecutor/SelectionVector和类型化向量循环，保留本项目的PG数据格式及精确numeric表示。`tests/vector.c`对照原逐行求值器验证算术、比较、NULL、短路、溢出、稀疏选择和批次尾部，并验证多CU/多块合并时共享聚合状态不重复累加。当前由Guest按独立异步执行槽推进各块，槽内算子顺序保持不变；详见apps/pg_sina_csd的执行流程。
