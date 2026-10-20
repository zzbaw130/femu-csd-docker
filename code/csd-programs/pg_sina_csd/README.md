# pg_sina_csd 设备算子

这里的 SO 在容器内编译，由 FEMU 的 CU 线程执行，逻辑上属于 CSD CPU。Guest 中的 PG 扩展和 SNIA 调度代码位于 `code/apps/pg_sina_csd`；FEMU 不解释 SQL，也未增加 SQL 专用 runtime。

## 执行流程

PG 从表达式树生成列投影、过滤、聚合、排序和 LIMIT 描述，Guest 的 PG backend 直接调用 SNIA 提交 batch：

```text
LOAD → Scan → Filter（可选）→ Aggregate.sink     # 每个 CU 一个分支，HYBRID batch
                   ...重复读取后续 chunk...
Aggregate.source → Sort（可选）→ Limit（可选）  # 合并局部状态，SERIAL batch
```

每个分支通常处理4MiB原始页。各分支拥有独立输入缓冲、列批次和聚合状态；SNIA 按依赖并行提交，底层不猜测并行策略。整个查询的数据块循环由 Guest PG backend 驱动，单个 batch 的依赖执行由 SNIA 调度。当前不包含设备自主预取或读算重叠。

| 源文件 / 构建产物 | 工作及 MR 参数顺序 |
| --- | --- |
| scan.c / pg-sina-scan.so | MR[0]原始页、MR[1]计划、MR[2]投影列批次、MR[3]分支状态与有效输入字节数 |
| filter.c / pg-sina-filter.so | MR[0]列批次、MR[1]计划；生成 selection vector，不复制被选中的行 |
| aggregate.c / pg-sina-aggregate.so | sink：MR[0]列批次、MR[1]计划、MR[2]分支状态；source：MR[0]所有局部状态句柄、MR[1]最终结果 |
| sort.c / pg-sina-sort.so | MR[0]最终结果、MR[1]计划；按多列方向及 NULL 顺序堆排序 |
| limit.c / pg-sina-limit.so | MR[0]最终结果、MR[1]计划；设置结果范围，支持 OFFSET |
| heap_count.c / pg-sina-heap-count.so | 阶段0诊断：MR[0]原始页、MR[1]计数；cparam1物理列数、cparam2有效输入字节数 |

`query.h`（位于 apps/pg_sina_csd/include）定义本扩展的应用级输入输出布局，`query_eval.h` 共享精确表达式计算。参数均按上述顺序构造，不新增设备参数解码机制。source 还接受仅有 MR[0] 的清理调用，用于错误或取消时释放尚未完成的局部状态。

## 列批次与内存

Scan 只解码查询需要的列；跳过其他列的内容，仍须遍历 heap 的变长属性边界。输出为列式 int64 值、NULL 位图、小数显示精度和 selection vector。输入 numeric 使用百分之一为单位，表达式及 SUM/AVG 使用带溢出检查的整数运算；内部 `__int128` 仅用于精确计算，不是 NVMe 标量参数封装。AVG 在设备上完成除法及符合 PG numeric 的精度选择与舍入，Guest 只转换最终结果为 PG Datum。

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

这是 PG 查询程序自己的可选输入输出约定，不是新 SNIA API，不修改 femu_csd_args。阶段0的 heap_count 参数约定不变，不接入这个开关。CPU/墙钟时间均为 FEMU 宿主线程测量，开启计时本身有时钟和小量输出传输开销；正常测量或发布时关闭即可。

## 列向量过滤

Filter 参考 DuckDB 的类型化比较和 selection vector 组织：每块输入先将复合 AND 条件中的列/常量比较归一化，常量精确转换到列的定点单位，再批量比较列值。支持 AVX2 的宿主一次处理4个 int64，合并 NULL 位图后输出行索引；尾部和无AVX2机器使用类型化标量循环。没有新增配置、FDM布局或算子参数，继续使用现有4个CU分支。

比较支持 =、<>、<、<=、>、>= 及反向常量比较。OR/NOT、NULL常量、计算表达式、不能无损归一化或超出int64范围的阈值继续由现有精确求值器处理。单谓词当前保留原求值路径：A/B验证发现单独加快Q1过滤会使总延迟回退，后续需要先细分执行等待。仅复合条件过滤使用显式SIMD；聚合表达式尚待向量化。`tests/filter.c` 将列式/SIMD结果与精确求值器逐行对照，覆盖符号边界、NULL、定点精度和非4倍数的批次尾部。
