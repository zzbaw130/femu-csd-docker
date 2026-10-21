# pg_sina_csd 设备算子

这里的 SO 在容器内编译，由 FEMU CU 执行，逻辑上属于 CSD CPU。Guest PG 扩展和 SNIA 编排代码在 `code/apps/pg_sina_csd`；FEMU 不解释 SQL，没有 SQL 专用 runtime，也没有查询辅助进程。

## 流水线与参数

Guest PG 从表达式树生成计划，直接调用 SNIA。当前支持以下流程：

```text
LOAD → Scan → Filter（可选）→ Aggregate.sink
Aggregate.source（小哈希合并，或外部部分状态归并；按需排序结果及LIMIT）

LOAD → Scan → Filter（可选）→ Sort.sink
Sort.source（多路归并、OFFSET/LIMIT，分批输出）

build表 LOAD → Scan → Filter（可选）→ HashJoin.build
每个build批次：HashJoin.load；probe表 LOAD → Scan → Filter → HashJoin.probe → Filter(join，可选) → Aggregate.sink
Aggregate.source → 分组归并/排序（按需）或最终数值表达式
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
| join.c / pg-sina-join.so | pg_sina_join_build：build列批次、完整计划、共享build控制记录、spill I/O AFDM |
| join.c / pg-sina-join.so | pg_sina_join_load：共享build控制记录、spill I/O AFDM；current由PG指定 |
| join.c / pg-sina-join.so | pg_sina_join_probe：probe列批次、完整计划、分支聚合任务、共享build控制记录、连接输出列批次；分组时追加spill I/O AFDM |
| join.c / pg-sina-join.so | pg_sina_join_close：共享build控制记录；只释放workmem，临时token由本查询deviceClose回收 |
| heap_count.c / pg-sina-heap-count.so | 诊断：原始页、计数；cparam1为物理列数，cparam2为有效字节 |

`apps/pg_sina_csd/include/query.h` 是本应用的数据布局，不是新的设备协议。参数按表中顺序构造，没有参数解码框架。结果块最多256行，用finished表示source结束；行排序总行数不受这个块容量限制，分组聚合也支持跨块输出，256仅是局部哈希表容量，满表会flush部分状态。

Aggregate.source 的单MR调用释放私有状态。Sort.source 的单MR调用也仅释放私有内存，随后PG关闭本查询独占的SNIA设备，回收剩余临时token；这允许失败batch已经回滚部分token。Sort.source 的双MR调用（句柄组、I/O）可通过NEED_FREE重入主动清理全部token和内存。正常source读到末尾会先释放token，再销毁状态，最后一个结果块仍保留在AFDM。

## 列批次、向量和精度

Scan解析PG12/x86-64/8KiB、全visible/frozen的heap，仅提取所需列。列批次包含int64值、NULL位图、numeric显示精度和selection vector；仍须遍历变长属性边界，不减少heap页读取量。

numeric输入必须有限、至多两位小数，可精确表示为int64百分数；支持整数、date、bool、ASCII bpchar(1)。较长字符串支持ASCII常量等于/不等于、IN和前缀LIKE；低基数、去尾空格后不超过15字节的分组/输出字符串通过运行期字典AFDM传ID，Aggregate.source恢复字节。只有顶层等于/IN证明值属于计划字典时，才支持该列使用静态字典进行投影/分组/排序。规划时已知不支持的能力回退PG；运行时数据超出设备表示范围时报错，不伪装成透明回退。

PG估计过滤后剩余不足一半行时，所需numeric可以延迟解码，包括WHERE使用的列。Scan将原始AFDM中的`{offset, bytes}`暂存在原8字节值槽。简单AND先用SIMD处理已解码列，再按存活行逐个判断其余列，同列相邻上下界共用一次解码；复杂AND/OR只把仍需判断的行传给后续表达式。最终存活行的所有延迟列会在Filter中转换为正常列值，因此下游无需理解原始位置。原始AFDM一直保留到当前槽sink完成。

`query_vector.h` 每批最多256行。列直接引用原列和selection，常量只保存一份，计算结果才写临时列；公共表达式按行缓存，保留NULL三值逻辑、短路和溢出语义。无NULL/错误批次可用AVX2执行比较、加减和int32×int32→int64乘法；加减检查溢出，其余情况回退精确int128。没有浮点近似或JIT。

Aggregate批量计算表达式和分组位置，再按输入列更新；同表达式SUM/AVG/COUNT共享sum/count，无分组COUNT(*)按批增加。AVG在设备上按PG numeric精度和舍入规则完成，Guest只转换最终Datum。小聚合结果Sort只交换行号，最终统一复制；Limit只调整结果范围。

搜索CASE用selection拆分真/假分支，只计算实际选中的行；NULL条件走ELSE，公共表达式仍按行缓存。标量聚合后的numeric加减乘除使用int128，除法遵循PG的显示精度选择和远离零的半舍规则；数值表示范围仍有限，溢出报错。

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

## 有界整数 Hash Join

`join.c`实现两个输入的INNER JOIN，完整保留重复键组合，NULL键不匹配。PG选build表并生成两个物理扫描计划；完整计划定义连接后列的位置。Scan只提取各侧需要的列，build记录只保存投影键/载荷及NULL和显示精度。Q14的text前缀已变成布尔列，原始字符串不进入哈希表。哈希桶、记录和批次描述都在workmem；AFDM保存输入输出、计划和控制记录。控制记录只含设备状态句柄、批次序号与统计，不含哈希表本体。

build共用一半查询预算，probe/聚合槽分摊其余预算；设备实际池容量继续限制总分配。build缓冲满时将当前记录按64KiB块NEED_STORE，保存token后复用缓冲。最后一次build后由load完成剩余记录的spill；若全表可驻留则直接建桶，无内部STORE/LOAD。对每个spill批次，load通过NEED_LOAD恢复记录并重建桶，PG随后并行扫描完整probe表；所有probe完成才切换批次，桶在probe期间只读。

probe生成256行连接列批次，调用同一SO内的`aggregate.c`私有sink，支持标量及可spill分组。各槽聚合状态跨build批次保留，最后由普通aggregate source合并、排序或计算最终表达式。该调用不消耗另一个CU，也不向主机返回中间行。profile的`HashJoin.probe+Aggregate`包含两者时间；分组probe和build/load支持I/O重入，计数AFDM按约定放在最后I/O缓冲之前。

分组spill时，build的workmem状态为各执行槽保留probe选择行位置、重复键链的下一项和待提交状态；qs_task只输入PG分配的slot编号并输出句柄/统计，连接输出AFDM保持不变。重入先将相同的ACK和I/O缓冲传给聚合sink，完成后再继续probe。生成新批次时清除上一次ACK，避免误确认下一次spill。原始heap在Scan/Filter后不再使用，可作为聚合I/O缓冲。所有probe结束后PG显式调用join.close，释放build工作区；分组source的最终结果排序可以使用原build预算，局部部分状态排序则使用各槽固定状态之外的余量。该复用只发生于已明确结束的build生命周期，仍受同一个池和查询上限限制。

参考DuckDB的build/probe阻塞边界、键/载荷投影和分批列输出；本阶段没有照搬其radix分区。内存不足时重扫probe，可能明显增加内部I/O；极多build批次也可能耗尽驻内存的token元数据，尚不保证任意规模输入。`tests/join.c`以独立按键统计核对重复键、负键、NULL、倾斜、空输入、分批spill和异常清理，分别验证4/64MiB池及64MiB池内4MiB查询预算。

## SQL常量字符串字典

Guest将字符串字面量放在`qs_plan.strings`，每项记录物理属性号、长度和字节；当前最多32项、每项127字节。字典在生成物理侧扫描计划之前按属性号及ASCII字节序排列，随后仅重映射属性号，保留编号。Scan对匹配值输出索引加一，其他非NULL值输出0，NULL仍用独立位图。bpchar去尾空格，text/varchar保留尾空格；空串也有自己的正编号。

等于/不等于与IN使用这些整数编号进行向量比较。0仅表示“不等于本计划中的任何常量”，不能用于任意字符串间比较或不受约束的分组。PG仅在顶层等于/IN过滤能保证每个存活值有正编号时编译字符串原值的投影/分组/排序；OR或CASE内的比较不能单独作此保证。不支持的表达式回退PG，不合并未知字符串。最终设备结果仍是编号，Guest按同一计划恢复Datum；不会把设备指针或未序列化字符串写入spill。

这使用了字典编码/选择向量思路，没有复制DuckDB的完整DictionaryVector。`tests/scan.c`覆盖短/长varlena、NULL、空串、未匹配编号及bpchar尾空格；真实Guest的join回归检查text/varchar/bpchar、NULL数组项、顺序和拒绝不安全表达式。完整Q12示例在Guest扩展的`tests/q12.sql`。

## 运行期字符串字典

Q5/Q10 的 `n_name`、客户姓名/地址/电话/备注没有 SQL 常量约束，使用独立的运行期字典：Scan 收到一个字典 AFDM，把去尾空格后的字符串插入并输出 ID；Join/分组/排序只传 ID，Aggregate.source 在最终结果块离开设备前恢复字节。字典有大小两档：低基数 Q5 用 512 项/64KB；Q10 用 1,048,576 项/20MB。字典头、开放寻址 bucket 数组、entry 数组和字符串字节区在同一 AFDM 内，bucket 由 Guest 用 64KiB 拷贝清零，避免大块 FDM fill 命令。

为不改变 `qs_cell`、Hash Join 记录和 Sort run 的固定布局，Q5 的短字符串仍内联在 16 字节 cell；Q10 超过 15 字节的字符串通过 `qs_result` 的 64KiB arena 回传，cell 保存 arena offset 和长度。PG 从 `groupClause` 去掉的 FD 输出列（如 Q10 的 `c_name/c_acctbal/c_address/c_phone/c_comment`）由规划端重新加入分组键；FD 保证不会拆分分组，设备端因此仍只哈希整数 ID，不需要变长 group record 或 FD payload 记录。

当前字典按“列所在关系行数 >4096”选择大档，Q10 会插入 customer 全部 150k 行的字符串（约 19.6MB，20MB 区内）；超过容量时报设备错误，不回退。字符串 `ORDER BY` 由 Aggregate.source 在排序前把短字符串解析为 inline 字节序，长字符串仍回退；Q10 按 revenue 排序不受影响。

Q9 增加第二组整数等值键（`build_key2/probe_key2`），build/probe 哈希和相等判断同时使用两个键；Q9 的 `partsupp` 因此按 `(ps_partkey,ps_suppkey)` 连接。`Q_YEAR` 在设备内把 PG date 转年份，`Q_PATTERN` 在 Scan 内处理 `%` 中间通配。Q9 目前仍受 build 组合重扫限制，详见开发记录。

Q7 验证同一基表的两个 RangeTblEntry（nation n1/n2）：规划按 RTI 分别绑定列区间，连接链保留两个实例；OR 字符串等值使用静态字典，未知字符串在结果输出时报错，不做静默合并。Q7 的短字符串分组/排序由运行期/静态字典 ID 完成。

Q8 进一步支持 8 表/7 join，并在有分组键时把 `sum(CASE...)/sum(...)` 编译成分组后的 `Q_DIV(Q_AGGREF,Q_AGGREF)`；CASE 条件使用静态字典，结果按分组键排序后返回。

Q4 增加 EXISTS 半连接：orders 为 probe，lineitem_pkey 为 tail index，index probe 在 heap tuple 上先执行 `l_commitdate<l_receiptdate`，每个 probe 行最多输出一次。Aggregate.source 的小结果快速路径会在字符串排序前解析字典，避免按 ID 排序。Q10 后续若要支持更长/更大基数，需要 FD payload 或可 spill 字符串。


连接后过滤复用`filter.c`，与`aggregate.c`、`sort.c`一起编译进`pg-sina-join.so`。PG计划的`join.residual`保存剩余跨表条件的表达式索引加一（0表示没有）；这不是SNIA参数或FEMU接口。probe输出列批次后先应用此条件，再把选择向量交给聚合。过滤结束即释放临时workmem，聚合状态继续存活，因此预算仍需预留后续批次过滤空间。NEED_STORE/LOAD重入只恢复已经过滤的批次，不重复过滤或计数；统计和计时包含在probe入口内。


Hash Join的行数估计只决定首次分配。build满表后尝试倍增记录和桶数组，仍受原build预算及共享workmem池限制；扩容失败或到达最大容量才生成spill批次。分配器复制期间同时计入新旧数组，因此不会靠瞬时超额分配扩容。桶数组增长失败会缩回新增记录空间并沿用原容量；已有spill批次的行数和token独立记录，不受后续容量变化影响。


多表probe仍用原来的MR顺序：列批次、完整计划、槽任务、build控制数组、joined列批次区、可选spill I/O。每个Join在joined区占一个对齐的256行列批次；前一级直接调用下一级，最后一级调用聚合。`query.h`的njoins和extra_joins只描述本应用的连接链，不是新增SNIA参数。游标在各自build workmem，NEED_STORE/LOAD沿调用链向外返回，重入先恢复子级再继续本级。build/load/close仍每次接收一个独立控制AFDM。全体probe结束后才替换任何build批次，source之前释放所有build状态。

## PG12 B-tree 索引连接

`index_scan.c` 与 `join.c`、既有扫描/过滤/聚合代码一起编译进 `pg-sina-join.so`。`index_scan.h` 只是这些实现之间的私有函数声明，没有新增设备 runtime 或 SNIA API。

Guest 准备索引文件的 extent、根页和前导整数键宽度；设备对最多64个外侧连接键批量执行 B-tree 下探，处理跨叶页的重复键，再根据 TID 仅解码对应 Heap Tuple。与全表扫描共用 `qs_scan_tuple`，保留 frozen/visible 校验，支持已被 VACUUM 整理的 HOT 根重定向。索引右侧过滤在产生的列批次上执行，再交给原有连接后过滤和聚合。

缺少的页只以地址记录写入任务 AFDM：`qs_task.index_reads` 给出索引/Heap 页号和输入槽，`index_pending` 表示本次 Join 尚未完成。Guest 用原有 SNIA 内部 LOAD 填充输入槽后，再调用同一 Join 入口。私有返回值 `QS_INDEX_WAIT` 只在 SO 内部传播，外部 CSF 正常完成值仍为0，PG根据 AFDM 任务记录继续；它不冒充标准 CS_STATUS。聚合发生 spill 时仍由 SNIA 自动处理 NEED_STORE/LOAD，父级连接游标保持存活。

页数据占用 I/O AFDM 的64KiB spill区之后最多128个8KiB输入槽；它不是算法工作内存。cursor/tag/key状态由共享 workmem 分配器限额，当前每个索引状态预留128KiB，查询结束统一 close。页槽在下一次物理 Scan 前失效，不跨查询缓存。当前形状只允许链尾一个 Index Join；不支持部分/表达式/降序索引、其他操作符类或 PG13+ posting-list 格式。
