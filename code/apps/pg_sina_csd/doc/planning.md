# SQLerate 节点级路径规划

Guest PG 负责 SQL 解析、去关联和合法连接枚举。SQLerate 的 `src/planner/paths.c` 通过 PG 的扫描、连接和上层 Path hook 加入设备候选；`cost.c` 对搜索阶段确定的设备物理属性计价。PG 在路径比较阶段就能看到设备输入成本，不再先固定一棵 HOST 树再做选址。设备仍只接收通用 SNIA 请求/设备图，不参与 SQL 成本规划。

参考 DuckDB 按关系集合枚举/比较子树的做法，复用 PG 已完成的连接合法性判断：每次 PG 提供一个合法的 outer/inner 关系组合，除其 Index/Merge/Hash 候选外，再组合两侧保留的最佳完整设备子树，生成设备 Hash Join 候选。设备编译再按行数×行宽调整 build/probe；RIGHT JOIN 复用 LEFT JOIN 的匹配与 NULL 补齐。没有另写一套关系合法性搜索，也不声称穷尽 PG 已剪枝、GEQO 未访问或当前编译器不支持的候选。

当前每个关系集合单独保留一份内部成本最低的设备子树；交付给 HOST 的候选仍由 PG 按成本、顺序等属性剪枝。这不是同时枚举所有内存峰值、启动延迟和有序性的多目标最优搜索。

SQLerate 保留三类完整候选：原始 Query 的原生 HOST 计划、将该 PG 优选树转换后的完整 CSD 计划（能编译时），以及安全改写 Query 后的 HOST/CSD 共享 Path 搜索结果。最后统一按毫秒比较。保留第二项是为了防止共享搜索提前剪枝时丢失原先完整卸载使用的物理树。设备候选先确定具体物理算法，再提取属性计价；完整设备执行记录仅在最终选择后生成。

去关联修改的是解析后的 Query 对象，不是 SQL 文本。改写 Query 的 HOST 路径与 CSD 路径参与同一次搜索，因此 HOST 同样可以享受改写；原始 HOST 候选则确保该改写不是强制替代。`host-only` 不调用扩展去关联，以保留原生 PG 对照。

## 两种候选状态

对同一个关系集合/上层结果，H 表示 PG 的 HOST Path（输入可来自设备）；D 表示完整设备子树，结果尚未回到 HOST。边界成本 B 是返回 PG 并转换为 Datum 的成本。以下仍是选址的简化递推，但现在 PG 同时枚举不同连接配对和 HOST 算法，不只比较一棵固定物理树的节点。

```text
H(v) = HOST 本节点成本 + Σ min(H(child), D(child) + B(child))
D(v) = 可编译的设备子树的内部 I/O + CU 计算 + spill + 未被覆盖的 HPCP 工作
根节点选择 min(H(root), D(root) + B(root))
```

每个位置必须先通过算子能力、类型/表达式和映射检查。HOST 结果行重新送入设备解释器尚未实现，故 HOST→CSD 中间行边界为不可行；不能假定它已经能运行。CSD 候选从已准备的基表/索引开始，可有多个独立子树向 HOST 的 Join、Aggregate、Sort 等返回数据。返回位置遵守 PG 的目标列、顺序、NULL 与表达式语义，子树边界保留索引/排序顺序。HOST Hash Join 的 Hash 子节点必须保留 MultiExec 接口；Merge Join 需要标记/恢复的内侧节点也保留在 HOST，它们下方仍可接入 CSD 输入。

设备 CustomPath 不标记为 parallel-safe，不放进 PG worker 私有/共享扫描中；原生 PG partial Path 和 Gather/GatherMerge 仍参与搜索，完整区域可编译时也可整体卸载。设备内部候选按不含返回边界的成本单独保留，不能仅因中间结果返回很贵就丢弃其与上层继续融合的机会。`filter-only` 保持既有共享设备扫描和原生并行边界，见 [filter-only 执行](filter-only.md)。

`csd-without-hpcp` 和 `csd-with-hpcp` 不按成本拒绝已支持的完整子树；完整编译失败时继续寻找可卸载的子树。不支持的语义继续由 PG 执行。`host-only` 不加设备节点或 SQLerate 外层节点。

## 成本来源

所有 SQLerate 成本以毫秒表示，和 EXPLAIN 原有 PG cost 单位区分。

- **HOST**：复用 PG 的 Path 成本计算；把设备输入的成本换算成 PG 单位后，PG 正常重算各 Join/上层候选。扫描在加入路径比较前按现有公式修正文件缓存驻留；原始 HOST 对照不修改其物理计划，仅在最终比较时做同样的缓存成本修正。Nested Loop 内侧重复、Gather、Hash 构建等继续由 PG 路径模型处理。
- **缓存**：规划时按关系文件段只读建立 PROT_NONE 映射并调用 mincore，不读取 Heap。观察结果只在本次规划内复用，不保存在跨查询全局缓存。它是 Guest OS 文件页缓存的快照，不是精确的 PG shared_buffers 命中率，也不保证执行时页面仍驻留。
- **设备资源**：通过现有 SNIA 查询 CU/FDM，并调用已有 workmem 容量 CSF 读取计算内存。设备 CPU 成本按串行、可并行任务波次、调度和合并阶段分别估计；结合 4/2/1MiB 输入和列向量/控制/结果缓冲估计 FDM 可支撑的并行度。排序归并等串行阶段不按全部 CU 数等比例缩短。
- **内部 I/O**：按估计的 Heap/索引读取字节和内部带宽计费；不是再加一次主机 NVMe Read。索引估计目前是按行选择率、物理页面和索引大小推算，不是逐页执行模拟。
- **工作内存与 spill**：按设备压缩行、哈希桶、分组槽位、排序 run、字典和物化状态推算工作集，考虑生命周期及仍存活的子状态。超出设备 workmem 的部分估计写出、换入及额外归并轮次；不因固定低水位就提前判定需要 spill。该值是估计，实际执行仍由统一 workmem 分配器和既有 spill 协议控制。
- **CSD→HOST**：按有效投影列的实际类型宽度、NULL 位图、批次头和 PG 变长正文估计返回字节，按同一批次容量估计交付次数；再加入共享 PCIe 和 Datum 转换成本。大量返回行的扫描卸载可能因此比完整卸载或原生 PG 更昂贵。具体布局和测量见 [filter-only 执行](filter-only.md)。
- **HPCP**：数据经共享 PCIe 填充主机页缓存，内部 I/O 和 CU 继续执行；模型计算不能被覆盖的传输，以及块登记工作。不会把所有后台累计耗时直接加到 SQL 墙钟，也不提前给尚未发生的后续查询缓存收益记负成本。

模型根据固定的规划时快照，选择**当前可执行候选及估计成本下的最小值**，不保证实测延迟的全局最优。内部/PCIe 带宽的初值参考当前测量和仿真配置；CPU 换算、缓存扫描带宽和结果批次交付开销仍是待校准的初始估计，不是 SNIA 标准返回的性能承诺。预备语句复用的通用计划不会因缓存页进出自动失效；切换模式、内存或成本参数会清理计划缓存，其他情况下需要重新规划以观察新的缓存状态。

详细公式、布局来源和未校准假设记录在 [阶段成本与状态估计](cost-model.md)，供论文方法与后续实测校准使用。

## 校准参数

```sql
LOAD 'pg_sina_csd';
SET pg_sina_csd.exe_path='SQLerate';
SET pg_sina_csd.max_memory=0;  -- 使用设备实际 workmem 总容量
```

| 参数（前缀 pg_sina_csd.） | 默认值 | 含义 |
| --- | ---: | --- |
| host_cost_ms | 0.02 | 每个 PG cost 单位的毫秒数 |
| csd_cpu_ratio | 0.35 | CU 数缩放前，设备向量 CPU 相对 HOST 的比例 |
| parallel_task_ms | 0.02 | 每个并行任务波次的调度成本，待校准的初值 |
| internal_mbps | 5000 | 聚合内部 I/O 带宽，十进制 MB/s |
| internal_write_mbps | 1000 | Spill 写带宽初值，读仍用 internal_mbps；需随设备校准 |
| pcie_mbps | 3500 | 每方向共享链路带宽，十进制 MB/s |
| cache_mbps | 10000 | HOST 缓存扫描带宽，十进制 MB/s |
| result_batch_ms | 1 | 每个设备结果批次的额外交付/物化毫秒数 |

CU/FDM/workmem 不在此重复配置。修改 FEMU 的容量/CU 后重新启动并准备关系，扩展读取新的实际容量；带宽等仿真参数改变时需相应校准模型参数。初期可以根据普通冷启动计时与 profile 调整这些量，不能用某一条查询的名字或固定胜负表代替成本。

## 查看计划与验证

使用 `tests/guest/queries/` 的原查询或 explain 脚本。SQLerate 外层 `SQLeratePlan` 仅转交子计划结果，显示选择和逐节点估计，不执行 Join/Filter 等 SQL 算子。CSD 节点显示模式、HPCP、输入来源、HOST/CSD/边界成本、缓存驻留、工作集、spill 和 CPU/I/O 估计；设备内部算子树仍由原 EXPLAIN 输出。成本估计与执行 profile 的实测时间分开显示。

```sh
bash tests/pg-sina-csd-test.sh paths
```

该针对性回归使用正式 Q6/Q13 检查五种模式及结果相等，用 Q1/Q17 检查快路径和标量子查询；另改变模型参数验证纯 HOST 和 HOST Join+CSD 子树两种选择，并覆盖并行扫描边界及同时存在的归并输入流。人为改变成本的用例只验证编排结构和语义，不作为性能验收。每次执行 SQL 前都重启 PG、清除 Guest page cache，不运行完整 22 条基准。

HPCP 名称统一到 `SNIA_FEMU_HPCP` 和 `test-hpcp`；内核模块 `femu_cache.ko`、设备 `/dev/femu-cache`、现有线格式和统计标签保留，避免为名称变化修改已部署的 DMA 协议。

## 设备内部索引连接与 RIGHT JOIN 验证

PG 的参数化 IndexScan 由其父 Nested Loop 编译成设备 Index Join。每个 CU 保留自己的 lookup 游标、页缓存和 AFDM，扫描过滤后将最多256个整数查找键组成一个向量；要求保持输入顺序时仍逐键处理。索引遍历、TID提取、所需Heap列解码及连接条件计算均在设备CSF中执行。缺页通过既有 `csNeedNvmRead` 请求交给设备图读取索引/Heap的映射范围，数据直接进入设备AFDM，不经过主机取回再传入。当前访问方式是PG12 B-tree前导整数等值查找，不宣称支持所有PG索引类型。

Index Join 的上游扫描现在与 Hash Probe 共用独立任务块调度：空闲CU继续取得下一块，索引读取等待保留当前键和左侧载荷，挂起的索引页读取完成后才更换输入。每批256键需要8KiB输入AFDM，因此PG执行器、设备算子和FDM成本估计共同按实际批次计算缓冲偏移；不修改SNIA API或设备图协议。匹配位图相应覆盖全部256键，保持LEFT/SEMI/ANTI及NULL语义。聚合改用Hash、移除分组Sort后，取消该Sort遗留的索引逐键顺序要求；不会跨Merge Join取消其输入顺序。

正式针对性冷启动结果在 `results/tpch.L53x7p/`，仅使用原始Q4/Q9/Q10/Q12/Q13。设备4CU、256MiB workmem、64MiB FDM，PG配置保持4 worker、2GB shared_buffers、16MB work_mem。所有结果与HOST逐行一致，均完整卸载，spill读写均为0：

| 查询 | HOST（ms） | csd-without-hpcp（ms） |
| --- | ---: | ---: |
| Q4 | 1781.171 | 916.445 |
| Q9 | 2558.822 | 1201.538 |
| Q10 | 2114.956 | 2372.699 |
| Q12 | 1577.865 | 1615.979 |
| Q13 | 2696.926 | 480.368 |

Q4/Q10/Q12的修正前CSD分别为1963.439/3092.929/4688.155ms（`results/tpch.4r6jA9/`）。Q13此前完整RIGHT JOIN编译失败，进入混合执行后结果错误；现可完整卸载。另在64MiB查询内存下，Q13为442.254ms、结果正确、无spill（`results/tpch.MZJzt9/`）。`results/tpch.ByuPy6/` 验证Q13的SQLerate也完整卸载且结果正确，原Q6快路径结果正确。

PG两项CTest与设备 `pgSinaIndexJoin64MiB`、`pgSinaPackedOutput64MiB` 通过。索引连接测试覆盖内部/任务块两种调度、四种连接类型、重复/NULL键、跨64键匹配及两侧空输入。此次未运行全量TPC-H；只对本节查询和Q6作针对性回归。

功能通过与性能通过分开：Q10仍比同轮HOST慢约12%，Q12也略慢，两者都没有spill，不能归因为物理工作内存不足，仍未达到性能验收要求。SQLerate对Index Join随机页访问的成本估计仍未补全。后续共享扫描修复已解决filter-only重复AFDM预留和协调CU旧确认位引起的丢行/超时，验证及剩余性能问题见[filter-only 执行](filter-only.md)。

## Q10/Q12 的索引连接与内部读取诊断

本节记录下面批量内部读取修正前的定位依据，修正与复测见下一节。

代码版本 `1e0b54b8a`，配置仍为 4 CU、256MiB workmem、64MiB FDM；PG 为 2GB shared_buffers、16MB work_mem、最多4个并行worker。本次只执行正式 Q10/Q12，每种组合重启 PG、清除 Guest page cache，没有修改查询、连接计划、参数或算子代码。

`results/tpch.l0gE2d/` 开启已有 `pg_sina_csd.profile`，用于诊断，不混入此前关闭 profiling 的正式基准：

| 查询 | HOST（ms） | filter-only（ms） | csd-without-hpcp（ms） | csd-with-hpcp（ms） | SQLerate（ms） |
| --- | ---: | ---: | ---: | ---: | ---: |
| Q10 | 2227.307 | 1782.023 | 2483.165 | 2360.429 | 2448.800 |
| Q12 | 1590.233 | 1117.749 | 1573.337 | 1584.088 | 1585.274 |

十次执行均与对应 HOST 输出一致。Q10 的三种完整 CSD 路径都慢于 HOST/filter-only；Q12 更准确地说是与 HOST 基本持平、明显慢于 filter-only，不能将百分之一左右的差别解释为稳定退化。三种完整卸载模式使用同一设备连接树；HPCP 与 SQLerate 不会自动换一种 Index Join 算法。

### 共同瓶颈：批量页读没有形成批内 I/O 并发

Q10 是过滤 orders 后用 `lineitem_pkey` 做 Index Join，再连接 customer/nation。Q12 只有两张表，是过滤 lineitem 后用 `orders_pkey` 做 Index Join。它们都实现了索引扫描，不是因缺失索引而全表读取连接内侧。

`index_scan.c` 一次保留256个查找键，最多请求64页。缺页通过 `namespace_wait → csNeedNvmRead → exec_graph` 在设备内部完成，页面不经过 HOST。但 `csd.c:csd_graph_io` 当前一次只处理一个物理连续范围，登记 `now + lat` 后进入 `GRAPH_IO_DONE`；`femu_csd_graph_poll` 等该事件到期才读取下一范围。因而同一 CSF 返回的多个独立范围逐段等待 NAND 模型延迟，没有批内并发。即使已有多个 CU/设备图，也不能把单个 CU 的64页请求当成64个同时在途的读取。

额外使用现有 `SNIA_FEMU_PROFILE=1` 对无 HPCP 的 Q10/Q12 诊断，记录为 `tpch.YAzugN`，截取固件日志至 `tpch.l0gE2d/firmware-profile.log`。同一计算节点、两次计算之间的连续内部读段，Q10 32188 对、Q12 6522 对，下一段开始时间全部不早于上一段完成时间，和代码路径一致。固件统计如下，均为跨图/CU的累计量，不能相加或直接当成查询墙钟占比：

| 固件累计指标 | Q10 | Q12 |
| --- | ---: | ---: |
| 内部 I/O 调用（含顺序输入） | 34037 | 7473 |
| 模型 I/O 延迟累计 | 1414.395ms | 432.734ms |
| I/O 软件处理墙钟累计 | 361.354ms | 293.633ms |
| 其中数据复制 | 199.023ms | 227.370ms |
| 其中 backend 锁等待 | 8.911ms | 6.927ms |
| 其中 CSD 锁等待 | 2.641ms | 0.635ms |
| 事件处理累计（包含 I/O 软件处理） | 715.152ms | 372.819ms |
| 事件逾期累计 | 175.090ms | 141.379ms |

内部 I/O 调用不是同等数量的 HOST NVMe 请求；相邻页可合并成一个范围。此轮仅包含 CSD 模式，测试脚本缺少同目录 HOST 基线而将 CSV 的 match 标为 no；已另与 `tpch.l0gE2d` 的正式 HOST 输出逐行比较，两条均一致，不将这个标记解释为计算错误。诊断结束后 PG 已恢复为不带 transport profiling 的进程。

### Q10：主要卡在索引连接阶段，尚非内存或 Hash Join 本身限制

无 HPCP 路径的执行树墙钟为2375.967ms；orders→lineitem 阶段为1955.275ms，协调 CPU 9.216ms、worker CPU 累计691.552ms，平均活跃计算约0.36个CU。后续 customer probe阶段320.327ms，聚合输出阶段80.118ms；Hash建表协调 CPU 11.844ms、最终Sort Sink CPU 57.026ms，结果只返回5651B、读取约0.064ms。不能将主要差距归于结果交付、最终排序或Hash建表。

实际索引候选228021条。设备载入76445个8KiB索引/Heap页；独立 PG 诊断中同样执行56934次 lineitem 索引探测，shared_buffers 命中168428次、缺失读取63044次。设备每个CU的索引/Heap页缓存固定128页、约1MiB，跨CU不共享；PG可以在查询内复用已经读入的索引和Heap页。计数证明设备存在额外载入，但PG的shared read不是物理NVMe请求数，不能直接等同磁盘读次数。

这里的63044次是HOST执行期间自身shared_buffers的缺失读取，不是CSD使用了PG缓存，也不是HPCP的收益。冷启动清除的是查询开始前的缓存；PG仍会在本次索引探测中逐渐填入并复用页面。HPCP写入的是Guest文件页缓存，不是shared_buffers；全CSD的Q10没有HOST基表读取消费者，本轮不能用它解释完整卸载性能。这个计数只用于对照两种索引实现的页访问行为。

workmem 峰值103780640B（约99MiB），FDM预留28359496B，spill为0。当前统计不支持“256MiB实际计算内存不够”的解释；固定缓存策略、索引批次的等待以及后续阶段的串行部分仍可优化，也不能仅凭上述累计计数精确预测修改后的收益。

### Q12：扫描过滤仍是主要CPU工作，索引等待降低其重叠程度

完整 CSD 的主阶段1485.421ms，累计 worker CPU 3739.068ms，平均活跃约2.52个CU；filter-only 的扫描阶段900.338ms，累计 CPU 3246.777ms，平均活跃约3.61个CU。filter-only 中CSD扫描过滤可以和HOST索引探测/聚合重叠；完整卸载后同一组4个CU既承担过滤又承担索引连接，缺页挂起期间虽然不占CU，却仍保留该任务块的输入，不能假定它已经切换到另一个独立块继续扫描。

完整 CSD 的 Scan Decode/Filter CPU 分别为1128.094/2444.455ms，filter-only 分别为1044.206/2165.420ms。复杂谓词仍是两边共有的热点，不能把整段Filter累计时间都算成完整卸载多出的开销。实际增量还包括orders索引读取、投影和较低的扫描/索引重叠程度。两条路径均处理31235次有效订单匹配，HOST索引读取21850页、设备载入24614页。

完整 CSD workmem峰值3052672B（约2.91MiB），FDM预留24140568B，spill为0；聚合Sink CPU约1.4ms、Sort Sink CPU约0.06ms、结果读取约0.058ms，不是主要瓶颈。HOST逐节点诊断及buffer计数在 `results/q10-q12-joins.QizqKg/`，其中开启ANALYZE/BUFFERS/TIMING的耗时仅用于定位，不作为正式成绩。

因此Q10/Q12有共同的内部随机读取问题，但主要CPU构成不同。应优先修正通用内部读的批内并发和完成归并，再核对索引缓存命中与任务块周转；Q12还可减少复杂谓词的CPU工作。未做实现前后A/B，不给出这几项优化的确定加速承诺。SQLerate也需把索引缺页及多轮读取计入候选成本，不能仅按总字节/带宽估价。本次只诊断，未修改固件或执行计划；结束后FDM_USED=0。

## 通用批量内部读取的并发与完成归并

FEMU的普通多范围LOAD和CSF的`csNeedNvmRead`现在共用有界批量提交：同一窗口最多64个物理读段以共同到达时间交给原FTL，NAND/LUN/通道状态仍由原模型更新。窗口完成时间取各段完成时间的最大值，只登记一个完成事件；窗口全部完成后才继续下一窗口，整批读取结束后仅恢复一次CSF。软件复制仍由原FTL线程执行，不新增线程，也不修改SNIA/NVMe接口。

同一物理LUN的竞争仍体现在模型完成时间中；FDM目标区域重叠时保留原有描述符顺序。HPCP为窗口中每个读段保留源AFDM、LBA、偏移和长度，完成归并后分别发布，不会只保留最后一段。读取覆盖活跃DMA区域时，先完成已提交窗口，再按现有周转规则等待或放弃可选填充，不能提前覆盖源数据。AFDM和程序的既有引用生命周期不变。

部署前干净停止PG，将同一份SF1归档至`results/exec-graph-batch.aRjWGW/tpch_s1.tar`；重启新固件后恢复原数据、加载已有HPCP模块并恢复`/dev/femu-cache`访问权限，重新prepare八张表。visible/frozen仍覆盖全部基表。设备保持4 CU、256MiB workmem、64MiB FDM，PG参数不变。

只使用正式Q10/Q12，各模式独立冷启动，关闭profiling，结果在`results/tpch.4wr0ie/`，十次全部与对应HOST输出一致：

| 查询 | HOST（ms） | filter-only（ms） | csd-without-hpcp（ms） | csd-with-hpcp（ms） | SQLerate（ms） |
| --- | ---: | ---: | ---: | ---: | ---: |
| Q10 | 2376.694 | 1748.387 | 1493.498 | 1497.387 | 1480.768 |
| Q12 | 1622.912 | 1129.901 | 1505.277 | 1482.596 | 1558.515 |

Q10三种完整卸载已快于同轮HOST/filter-only；对照此前关闭profiling的`tpch.gb3c93`，无HPCP由2357.234ms降至1493.498ms。这个跨轮比较不作为精确加速置信区间，HOST自身也有波动。Q12完整卸载仍明显慢于filter-only，不能将其判为最优编排，也不能据本次修改宣称所有剩余瓶颈已解决。

同等PG阶段计时的独立诊断，修正前为`tpch.l0gE2d`、修正后为`tpch.fzdWWU`，不混入正式延迟：

| 指标 | 修正前 | 修正后 |
| --- | ---: | ---: |
| Q10索引连接阶段墙钟 | 1955.275ms | 994.083ms |
| Q10该阶段worker CPU累计 | 691.552ms | 677.536ms |
| Q10索引/Heap载入页 | 76445 | 76384 |
| Q12扫描/索引连接阶段墙钟 | 1485.421ms | 1447.731ms |
| Q12该阶段worker CPU累计 | 3739.068ms | 3748.008ms |
| Q12 Filter CPU累计 | 2444.455ms | 2456.772ms |

Q10候选元组仍为228021，workmem峰值仍为103780640B，Q12峰值仍为3052672B，spill均为0；没有靠减少逻辑工作或增加缓存容量换取加速。Q10主要消除了读取串行等待；Q12的CPU过滤及与索引读取的重叠仍需优化，其阶段改善约2.5%，不能把原来相对filter-only的差距全部归于批内串行读。

额外固件tracing单独保存在`tpch.2ZOtgx`和`exec-graph-batch.aRjWGW/firmware-profile.log`：Q10完成事件数由36079降至3920，Q12由8451降至1939。逐段日志会影响墙钟，只用于验证完成归并。共同到达时间下，每段模型延迟包含同LUN的排队，累加的`io_model_ns`不再等于原串行服务时间和，不能用于推算查询墙钟收益。

针对性qtest通过：16路独立NAND读取、同LUN排队、重叠FDM有序覆盖、普通LOAD跨64段窗口（65段逐页校验）、原设备图、spill及4KiB LBA、取消，以及默认/双poller的HPCP DMA。多MR用例还验证描述符所在MR被读结果覆盖后快照仍有效、两个范围的HPCP内容和LBA均正确。查询结束后FDM_USED=0，PG已恢复为无transport tracing的正常进程。本轮没有运行全量TPC-H。

## Q12：让索引读取挂起时仍有其他任务可运行

完整卸载和filter-only共有lineitem扫描及复杂谓词，不能将整段过滤CPU作为完整卸载多出的成本。完整卸载额外在设备上执行orders索引连接和聚合；此前4个任务槽对应4个物理CU，每个槽持有自己的4MiB输入及索引游标。索引缺页挂起会释放CU，却不释放当前任务槽；如果4个槽同时等内部读取，即使还有未领取的扫描块，也没有独立上下文开始处理它们。filter-only则可以让PG处理上一批匹配行，同时设备继续扫描。

参考DuckDB的`PipelineExecutor`在输入阻塞时返回`INTERRUPTED`并保留任务状态的做法，本项目复用已有独立执行上下文、SNIA回调和设备就绪队列。含并行Index Join的pipeline使用每物理CU两个任务槽，最多64槽；物理CU数、线程数、NVMe接口和SQL算子均不变。扫描任务仍领取共享块序号，独立槽各自持有输入，不共享尚未完成的FDM内容。普通scan/filter不增加槽数。FDM布局继续按实际容量缩小输入块或减少槽数，不突破设备上限。

规划器与执行器使用同一槽数规则：FDM、表达式scratch、局部聚合状态按任务槽计费，CPU并行成本仍最多按物理CU数计算。EXPLAIN的`CSD Device CUs`为物理数量，新增`CSD Task Slots`明确表示独立上下文数量；已有`CSD Workers`保留原字段及其执行槽统计含义。

只用正式`tests/guest/queries/12.sql`，每个模式分别重启PG并清除Guest page cache。4 CU、256MiB workmem、64MiB FDM和PG参数不变。关闭profiling的最终记录为`results/tpch.I4mggb/`，所有结果与HOST一致：

| Q12执行路径 | 冷启动延迟（ms） |
| --- | ---: |
| host-only | 1599.295 |
| filter-only | 1156.080 |
| csd-without-hpcp | 1044.980 |
| csd-with-hpcp | 1062.048 |
| SQLerate | 1131.961 |

SQLerate选择完整CSD树：lineitem Scan/Filter → orders Index Join → Hash Aggregate → Sort，开启HPCP。无HPCP完整卸载比同轮filter-only低9.6%，比HOST低34.7%；相对此前关闭profiling的1505.277ms降低30.6%。跨轮单次数据不作为精确加速置信区间。

相同PG阶段计时的独立诊断为修改前`tpch.xnRU1U`、修改后`tpch.CjdzuH`，与正式延迟分开：

| 完整CSD指标 | 4任务槽 | 8任务槽 |
| --- | ---: | ---: |
| 物理CU | 4 | 4 |
| 主扫描/索引连接阶段墙钟 | 1439.493ms | 1012.379ms |
| 该阶段worker CPU累计 | 3752.527ms | 3394.866ms |
| 平均活跃CU / CPU利用率 | 2.61 / 65.2% | 3.35 / 83.8% |
| CSF调用次数 | 978 | 1017 |
| 索引/Heap载入页 | 24592 | 24673 |
| 匹配订单元组 | 31235 | 31235 |
| FDM预留 | 24140496B（23.02MiB） | 46919632B（44.75MiB） |
| workmem实际峰值 | 3052672B（2.91MiB） | 4745952B（4.53MiB） |
| spill读/写 | 0 / 0 | 0 / 0 |

索引读取页数及CSF调用没有减少，两个模式共有的过滤实现也未修改；收益来自用现有空闲FDM保留更多独立任务、提高计算与内部索引读取的重叠程度。复杂谓词的累计CPU仍很高，但不是本次针对的新增工作。阶段墙钟减去worker CPU/4约由501ms降至164ms，这个差额包含I/O、调度及负载不均衡等因素，不能称为精确的索引等待计时。

扩展的heap布局及规划成本测试通过；设备算子的4槽和新增8槽索引连接测试在64MiB workmem下通过，覆盖inner/left/semi/anti、空输入、直接及分块输入、结果和资源清理。查询结束后FDM_USED=0。本轮未运行其他TPC-H查询。

## Q10：增加任务槽后未观察到整体加速

复用上述已部署构建，仅运行正式`tests/guest/queries/10.sql`。三个模式分别重启PG并清除Guest page cache，关闭profiling；4 CU、256MiB workmem、64MiB FDM和PG参数不变。结果在`results/tpch.wDdubS/`，均与本轮HOST一致：

| Q10执行路径 | 冷启动延迟（ms） |
| --- | ---: |
| host-only | 2188.220 |
| filter-only | 1721.197 |
| csd-without-hpcp | 1493.777 |

完整卸载比同轮HOST低31.7%、比filter-only低13.2%，但与此前4任务槽记录`tpch.4wr0ie`的1493.498ms基本持平。本次确认实际启用8任务槽，不能把完整卸载已有的优势算作新增槽数的收益。两个记录是跨轮冷启动测量，不是同轮强制4/8槽的重复A/B。

为核对主阶段，单独对Q10完整卸载冷启动并开启阶段计时，保存在`results/q10-task-slots.oJ3XEh/`，输出已与本轮HOST比较一致。索引连接阶段1022.997ms、平均活跃0.68 CU；此前同类4槽诊断`tpch.fzdWWU`为994.083ms、0.69 CU，没有出现Q12那样的重叠改善。后续customer Hash Probe阶段319.696ms（此前320.050ms）、聚合输出阶段84.921ms（此前80.574ms），也无明显改善，不能解释成“索引阶段显著加速却被其他阶段抵消”。具体内部读取/就绪等待仍需另行定位，本轮不改代码或将其归因于共有过滤。

8槽实际workmem峰值173600384B（165.56MiB），此前为103780640B（98.97MiB）；FDM预留51007536B（48.64MiB）。spill读写均为0，不能归因于内存不足。阶段计时与正式延迟分开，结束后FDM_USED=0。本轮未运行其他TPC-H查询。

## 任务槽优化后的全量冷启动对照（旧SQL快照）

此节对应当时的代码及默认SQL快照。当前Q17/Q20按用户要求再次默认采用等价预聚合写法，原始相关版本另存为original文件。历史成绩仍与当时的代码、配置和SQL版本绑定，不当作最新复测成绩，也不能与原始相关版本混算；两类版本说明见本文末尾。

复用提交`98c0fee78`对应的现有部署，没有修改执行代码或重新编译。运行`PG_SINA_PROFILE=off sh /home/femu/tests/tpch-test.sh`，只使用正式22条SQL；每条、每种模式均单独重启PG并清除Guest page cache。设备4 CU、256MiB workmem、64MiB FDM，PG为shared_buffers=2GB、work_mem=16MB、max_parallel_workers_per_gather=4。完整记录为`results/tpch.BW5CB4/`，精确值及校验结果在`timings.csv`，配置和实际计划在同目录日志中。

110次执行全部成功，结果与对应HOST一致。两种强制完整卸载均在22条实际出现CSD节点；SQLerate中20条完整CSD，Q2/Q21由CSD完成Sort及其下方子树，最后的LIMIT由HOST执行。filter-only的Q11/Q22实际没有触发卸载，不能把这两条的波动算作设备扫描收益。

下表单位ms，四舍五入为整数；合计使用未取整原值。只统计SQL执行时间，不含PG重启和清缓存操作。

| 查询 | host-only | filter-only | CSD无HPCP | CSD有HPCP | SQLerate | SQLerate路径 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Q1 | 2913 | 2501 | 815 | 794 | 789 | 完整CSD |
| Q2 | 948 | 959 | 315 | 329 | 379 | CSD → HOST LIMIT |
| Q3 | 2303 | 1852 | 593 | 634 | 650 | 完整CSD |
| Q4 | 1840 | 1310 | 453 | 494 | 508 | 完整CSD |
| Q5 | 1751 | 1512 | 450 | 467 | 517 | 完整CSD |
| Q6 | 1218 | 511 | 531 | 484 | 473 | 完整CSD |
| Q7 | 2267 | 2025 | 570 | 623 | 681 | 完整CSD |
| Q8 | 2278 | 1800 | 573 | 620 | 681 | 完整CSD |
| Q9 | 2457 | 2251 | 1161 | 1131 | 1155 | 完整CSD |
| Q10 | 2110 | 1748 | 1496 | 1516 | 1548 | 完整CSD |
| Q11 | 440 | 444 | 242 | 267 | 269 | 完整CSD |
| Q12 | 1620 | 1094 | 1093 | 1054 | 1065 | 完整CSD |
| Q13 | 2587 | 1313 | 503 | 467 | 474 | 完整CSD |
| Q14 | 1291 | 743 | 571 | 523 | 528 | 完整CSD |
| Q15 | 1848 | 1939 | 782 | 762 | 739 | 完整CSD |
| Q16 | 601 | 849 | 443 | 406 | 438 | 完整CSD |
| Q17 | 3530 | 3641 | 1373 | 1356 | 1384 | 完整CSD |
| Q18 | 8578 | 8537 | 849 | 865 | 881 | 完整CSD |
| Q19 | 1373 | 1222 | 939 | 933 | 969 | 完整CSD |
| Q20 | 3253 | 2580 | 490 | 473 | 492 | 完整CSD |
| Q21 | 1912 | 2173 | 974 | 1003 | 1006 | CSD → HOST LIMIT |
| Q22 | 810 | 767 | 353 | 349 | 338 | 完整CSD |
| 合计 | 47928 | 41770 | 15568 | 15550 | 15964 | — |

三种完整卸载方案的22条全部快于同轮HOST。按SQL耗时合计，无HPCP、HPCP、SQLerate分别为HOST的3.079倍、3.082倍、3.002倍速度；逐查询加速比的几何均值分别为2.822、2.817、2.733倍。所有完整卸载的spill读写均为0，实际workmem峰值最大173600384B（Q10，165.56MiB），FDM预留最大66875920B（Q21，63.78MiB），结束后FDM_USED=0。

filter-only相对HOST仍有Q16（+41.2%）和Q21（+13.7%）的明显退化；Q15（+4.9%）、Q17（+3.1%）、Q2（+1.2%）、Q11（+0.8%）差距较小，其中Q11未卸载。完整卸载Q10约1496ms，与专项测试相符；Q12无HPCP约1093ms，与本轮filter-only约1094ms几乎持平，先前单轮9.6%的优势没有稳定复现，但仍低于修改前约1505ms。

SQLerate不是逐条实测最低：相对五路径逐查询最小值，Q2/Q7/Q8分别高20.3%/19.5%/19.0%。Q2/Q21有HOST LIMIT边界，其余选择完整CSD；单次冷启动且关闭细分profiling，不能把额外延迟直接认定为规划误选、HPCP或某个算子的问题。本轮记录性能差距，不额外修改或复测。

HPCP与无HPCP的22条SQL耗时合计相差约-0.12%，总体基本持平；单条最大增加约10.1%（Q11，约24ms）。部分查询开启后更快也不能据单轮测量推断填充必然加速；每次冷启动都清除此前页缓存，本轮不验证跨查询缓存利用收益。

## 当前InitPlan、SubPlan与去相关的支持边界

当前并不是执行任意PG SubPlan的通用设备解释器，而是先做受限的去相关，再编译PG选择的物理树。流程在PG扩展内完成，FEMU/SNIA不识别子查询语义：

1. `src/extension.c:hooked_planner`仅在完整CSD/HPCP/SQLerate模式先复制Query，调用`src/planner/rewrite.c`。host-only和filter-only保持原始Query，交给PG正常规划。
2. 本项目改写严格WHERE条件中的相关标量SUM/AVG/MIN/MAX：提取内外层整数等值键，将内层聚合改成按关联键分组，再与外层INNER JOIN，之后仍由PG选择物理Join顺序。只覆盖空集返回NULL的聚合及其严格表达式；COUNT、OR分支、SELECT输出位置、非等值相关、额外GROUP BY/HAVING/窗口/LIMIT等形态不做这项改写。它借鉴DuckDB的去相关思想，但不是DuckDB完整的Delim/Mark/Single Join实现。
3. PG12自己的`prepjointree.c:pull_up_sublinks`会在满足条件的WHERE/JOIN ON位置，将ANY/EXISTS/NOT EXISTS转成Semi/Anti Join；不能保持NULL、相关或外连接语义时会保留子计划。`subselect.c:build_subplan`通常将非相关标量结果转成InitPlan/PARAM_EXEC，将适合哈希的非相关ANY保留为hashed SubPlan。不能期待所有子查询都消失，非相关标量只计算一次本来就是合理方案。
4. 扩展的`src/planner/plan.c`编译上述物理节点；设备`code/csd-programs/pg_sina_csd/plan.c`执行注册的初始化子计划，再运行主流水线。

| PG规划后的形态 | 当前完整CSD支持 |
| --- | --- |
| 已展开为普通/Semi/Anti Join | 编译为现有Hash/Merge/Index Join；Q21的EXISTS/NOT EXISTS属于这个类别 |
| 非相关标量InitPlan | 要求EXPR_SUBLINK、无parParam/args、单setParam，且子树可编译；设备先求值一次并写入查询的params，主表达式用QP_PARAMETER引用，CU继承相同参数 |
| 非相关hashed ANY SubPlan | 要求useHashTable、无parParam/args、单参数的int2/int4/int8等值判断；编成QP_SET，主表达式用QP_INSET探测；外层NOT保留NOT IN的NULL语义 |
| 任意相关SubPlan | 不通用支持；未被改写/PG展开的parParam或args会使候选编译失败，保留HOST路径 |
| ARRAY/EXISTS/MULTIEXPR等InitPlan | 不按上述标量协议通用支持；CTE另由CteScan处理 |
| SubqueryScan/CTE | 支持的SubqueryScan变为投影；CteScan引用查询内共享的QP_MATERIALIZED状态，按现有内存/临时存储机制读回，并非一律消除物化 |

`pg_sina_compile_subtree`将支持的root InitPlan登记到`init_nodes/init_params`，设备在主数据流水线之前执行：标量0行赋NULL、1行保存原值及数值scale、多行报错；集合初始化构建整数集合，并记录空集/NULL，后续探测不逐tuple重跑子查询。初始化阶段自身可使用现有并行算子，但多个初始化项按登记顺序推进。这是当前设备执行约定，并没有完整复制PG所有按需InitPlan调度行为。

参数化Index Join也不是“支持任意相关SubPlan”：它只把已成为PG NestLoop的索引条件中的nestParams绑定回外层列，通过设备索引访问批量处理。不能将这个能力推广到任意SELECT标量子查询或相关表达式。

filter-only现在按扫描实际表达式的依赖判断：不使用执行参数的扫描可以独立下推；引用HOST InitPlan结果的扫描，将对应Param登记到`CustomScan.custom_exprs`，由PG保留依赖并按需求值，再将值、NULL、numeric计算scale和显示scale写入一次设备快照。快照使用现有`qs_cell`/查询params数组，附在计划AFDM之后、文件extent之前；`param_nodes`中的`QP_HOST_PARAMETER`区分HOST值与设备初始化节点，CU仍继承查询参数。没有新增FEMU/SNIA命令或HOST中间行传输。

HOST参数目前支持有限numeric、int2/int4/int8、boolean和date。numeric不经double/int64截断，以scaled int128传递；参数只在PG owner启动/重扫时读取，SNIA回调及设备线程不调用PG。外层NestLoop逐行参数、字符串/数组参数或扫描内嵌SubPlan不因此放开；Gather下需要私有重放的输入仍保留HOST。该修复针对filter-only，其他模式的整树设备初始化协议保持原样；HOST关系中间结果回传CSD仍未开放。

HOST InitPlan自身仍由PG执行聚合/连接，其内部的独立扫描也按filter-only规则下推。每条扫描的设备初始化数为0，避免将整个InitPlan顺带编入扫描。根节点本身就是扫描时也保留HOST InitPlan；标准PG依赖机制继续负责并行参数传递和重新执行时的求值。

Q11/Q22参数绑定修复时、恢复Q17/Q20之前的22条计划检查记录为`results/subplan-audit.ox4pU1/`，只运行当时22条explain脚本的host-only/filter-only/csd-without-hpcp，共66份计划，没有执行全量数据查询：

| 模式 | 仍保留的PG子计划 |
| --- | --- |
| host-only / filter-only | Q2相关标量SubPlan；Q16非相关hashed SubPlan；Q11/Q15/Q22的InitPlan |
| csd-without-hpcp | 无留在PG执行树中的相关SubPlan；22条均包含CSD执行节点，初始化/集合可在设备计划内部继续存在 |

Q2的原生SubPlan索引条件仍引用外层`part.p_partkey`，完整CSD模式先将它改写为聚合连接。Q16的尺寸常量IN是ScalarArrayOpExpr展开的OR；供应商NOT IN在完整卸载时编成NULL-aware整数集合，在filter-only中仍由HOST执行。Q11/Q22在完整卸载时保留设备标量初始化，在filter-only时使用HOST初始化及扫描参数绑定。

该次检查的Q17/Q20采用手写GROUP BY派生表，与当前默认版本的结构相同。原始相关SQL另存为original文件，其原生PG计划保留相关SubPlan；完整CSD模式的自动改写见下面的针对性验证。不能将默认SQL没有SubPlan的现象用于证明PG或本项目支持任意相关子查询。

## Q17/Q20原始相关版本的验证

提交`cf389b836`曾将默认Q17/Q20及各自explain脚本恢复为TPC-H相关结构，并对照仓库DuckDB dbgen的q17/q20模板核对。该原始版本现在逐字保留为`17.original.sql`、`20.original.sql`及对应`N.original.explain.sql`。Q17保留`Brand#54`、`JUMBO BAG`、7.0及0.2；Q20保留`goldenrod%`、1996-01-01起的一年日期区间、`JORDAN`及0.5。未添加LIMIT，也未更改索引或PG配置。

该次原始版本验证使用现有部署，当时运行`PG_SINA_PROFILE=off TPCH_MODES='host-only csd-without-hpcp' sh /home/femu/tests/tpch-test.sh 17 20`，文件当时仍名为`17.sql`、`20.sql`。每个组合独立重启PG并清除Guest page cache，沿用脚本120秒statement_timeout；设备4 CU、256MiB workmem、64MiB FDM，PG shared_buffers=2GB、work_mem=16MB、max_parallel_workers_per_gather=4。记录在`results/tpch.TfoGfB/`：

| 查询 | 原生HOST | 完整CSD无HPCP |
| --- | ---: | ---: |
| Q17 | 120秒超时，未完成 | 1378.759ms |
| Q20 | 120秒超时，未完成 | 509.227ms |

针对性计划在`results/q17-q20-correlated-plans.9jAqIE/`。原生PG两条均保留相关SubPlan，对lineitem重复顺序扫描；现有lineitem只有`(l_orderkey,l_linenumber)`主键索引，不能按相关的part/supplier键直接定位。超时不是完整查询的完成延迟，不将其填成120秒的正式成绩，也不重用旧派生表写法的HOST时间。

两条完整CSD均成功执行。记录中的原始Query Text保留相关AVG/SUM、没有手写GROUP BY；CSD计划已经包含Hash Aggregate和Join。由此确认现有`src/planner/rewrite.c`在PG规划前完成了去相关，无需在测试脚本或设备固件中另写规则。Q17实际workmem峰值86412864B，Q20为88968976B，spill读写均为0，结束后FDM_USED=0。

正确性与执行成功分开：本轮HOST均未完成，因此不能完成同轮HOST/CSD逐行结果验收；脚本`result_match=no`来自缺少HOST参考，不能解释成已观察到结果不一致。仅读取历史`tpch.BW5CB4`中等价旧SQL的既有HOST输出作辅助核对，两条CSD输出逐行一致，没有重新执行旧SQL替代验收。原始写法在当前120秒限制内的直接HOST结果比对仍未完成。

本轮未运行其他TPC-H查询，也没有为获取更快HOST成绩添加索引、调整规划开关或延长超时。

## 默认对照与original备份

按用户指定，`17.sql`/`20.sql`及对应explain再次采用等价预聚合连接写法，所有参数不变；SQL头部已说明原生PG相关子查询重复扫描导致120秒超时，以及采用该写法完成host/CSD对照的原因。默认测试脚本仍只读取22个编号文件，original备份不会被自动执行；Q19保持原样。

使用`N.sql`比较两侧对同一改写后计划的执行性能；验证扩展自身去相关能力时，显式用现有`cold-query.sh`运行`N.original.sql`，计划用`N.original.explain.sql`检查。不得将默认SQL中预先做好的去相关当成扩展收益。原始备份保持原始结构及相同参数，不回添非标准LIMIT。

核对全部22条与最初导入版本及本地dbgen模板，除Q17/Q20外没有发现额外的手工结构改写。其他差异是参数化日期写法和非标准LIMIT修正；Q15的创建/删除视图属于原TPC-H写法，DuckDB模板的CTE是其自身适配，不因此替换本项目Q15或新增重复备份。本次只做文件一致性及针对性EXPLAIN检查，不重跑性能基准。

## HOST/CSD 共享路径搜索验证

本阶段只修改 Guest 扩展的规划结构，不改变 SNIA、固件、设备算子或设备成本系数。候选预览使用隔离的 PG 规划状态；候选的临时内存在估价后回收，只保留 PG 物理候选与成本信息，最终选中后生成执行记录。物理选择和估价工作区的使用见下节。表映射在一次搜索内按关系/文件节点复用，搜索结束即失效，避免为重叠子树反复执行同一 SPI 查询。

已有 HeapLayout、PlannerCost 两个 CTest 通过。新增 `tests/guest/planner-path-test.sh` 只执行正式查询的 EXPLAIN：昂贵设备选 HOST、便宜设备可选 CSD；原始 Q17 的 host-only 保留 SubPlan，而昂贵设备条件下 SQLerate 选择去相关后的全 HOST；原始 Q17/Q20 的设备候选可生成。最终结构记录为 `results/planner-paths.AEuDWW/`。

原始相关语句的执行验证在 `results/planner-rewrite.fOnfKR/`：Q17/Q20 的 original SQL 经 SQLerate 改写后与同参数默认等价 SQL 的 HOST 输出一致；Q17 还提高设备 CPU 成本，实际选择无 CSD 节点的改写 HOST 计划，结果同样一致。这是改写正确性验证，不将不同结构 SQL 的计时作为原生相关查询的性能对照。

针对性冷启动结果如下，4 CU、256MiB workmem、64MiB FDM，PG 参数仍为4 worker上限/2GB shared_buffers/16MB work_mem；只使用编号 SQL，各模式结果均一致、实际 spill 均为0。Q2/Q9 来自 `results/tpch.92vGb7/`，其余三条来自同阶段 `results/tpch.urb67t/`；不声称是全量或多轮统计。

| 查询 | HOST ms | 强制 CSD+HPCP ms | SQLerate ms | SQLerate 执行器 ms | 选中路径 |
| --- | ---: | ---: | ---: | ---: | --- |
| Q2 | 915.930 | 335.417 | 615.749 | 258.681 | HOST Limit，其余 CSD，共享搜索 |
| Q9 | 2559.128 | 1150.805 | 1346.266 | 1096.984 | 原 PG 优选树的完整 CSD 转换 |
| Q11 | 447.557 | 293.442 | 262.445 | 183.293 | 原 PG 优选树的完整 CSD 转换，含初始化 |
| Q13 | 2651.835 | 471.410 | 485.838 | 407.977 | 原 PG 优选树的完整 CSD 转换 |
| Q18 | 8530.768 | 862.003 | 971.767 | 801.938 | 共享搜索的完整 CSD 树 |

执行器数值来自本轮关闭逐节点 TIMING 的 auto_explain。总耗时减执行器耗时还包含规划、初始化、协议和日志交付，不能直接命名为 Planning Time。Q9/Q2 的规划时映射查询已分别限制到12/5次，但复杂候选搜索仍有明显额外成本：两条总耗时与执行器之差约249/357ms，不能再假定 SQLerate 只增加固定20ms。结构与结果检查通过，不代表已达到最小实测延迟；设备估价和搜索开销后续仍需校准、精简。本阶段没有重跑完整 TPC-H。

## 物理选择、成本属性与最终编码

这里的“编译”是把 PG Plan 转成设备算子执行记录，不是编译 CSF 的 `.so`。本节改造范围是 SQLerate；强制 CSD 与 filter-only 沿用既有单计划路径。SQLerate 按三个步骤处理，代码均在 Guest PG 扩展内；生成的程序仍通过既有 SNIA 接口交给设备 CU 执行。

1. **Path 搜索确定物理方案。** 沿用 PG 的合法关系组合、连接顺序和物理候选。Hash Join 保持 Hash；Merge Join 保持 Merge，并计入其必要的输入排序；可用的参数化索引 Nested Loop 对应 Index Join。没有索引而具备可哈希等值键的设备方案，在搜索时明确为 Hash。PG 与设备候选统一比较，不在最终编码器里再次改选算法。Hash 的 build/probe 方向、聚合是否改为 Hash、分组排序是否能移除或移到聚合上方，也在这一阶段确定。
2. **候选只估必要属性。** 保存基数、分组数、输入输出类型/列来源、表达式数量、访问与顺序属性，以及并行、状态和 spill 估计需要的字段。复用现有 `qp_plan/qs_plan` 作为一次规划内的工作区；通用表达式只记录类型、列来源和操作数量，不生成可执行的标量指令和常量表，必要的字面量比较用于检查支持性和扫描解码类型。工作区不能作为程序发送设备，也不随候选保留。候选保存 PG 物理树、数字成本和简短选择信息。融合 CSF 沿用现有兼容性检查和短描述，不为候选封装执行 Const。既有内部/PCIe I/O、HPCP、缓存、阶段并行、状态和 spill 的成本公式继续使用。
3. **最终选择后编码。** 原始 HOST、原始 PG 树对应的设备方案与共享 Path 搜索结果完成比较后，才为选中的设备子树生成 Const。通用完整表达式和执行记录在此时填写，并生成逐节点 EXPLAIN 说明；全 HOST 选择不生成设备记录。保留 PG 修改列引用前的子计划上下文，避免延迟编码受到 `setrefs` 对 InitPlan/CTE 表达式的修改影响。

唯一集合过滤、LEFT JOIN COUNT 预聚合和 DISTINCT 分阶段等已有关系改写，同样在候选属性阶段确定，并随候选保存改写标志、节点算法和左右输入编号。编码器只执行已经选择的改写，并核对节点数量、算法/索引顺序及输入边；不满足则报错，不能悄悄采用另一个物理方案。新增的内部聚合节点以输入/键域基数估计状态，避免将分组数默认为1。

顺序是物理属性的一部分。设备子树融合进上层 Hash 后，可以去掉只为原 HOST 交付边界保留的排序，同时保留列投影；Merge 自行处理输入排序，Limit 保留必要顺序。如果 Hash/DISTINCT 等不能保证 PG 候选声称的顺序，就不再向 PG 宣告该 `pathkeys`；整条查询要求 ORDER BY 时补上明确的输出 Sort。已还原全量行数的 Gather 被移除或转为投影，防止父候选复用时再次乘以 PG worker 数。

候选阶段不再逐节点生成落选方案的 EXPLAIN 长文本。根部 Placement 仍报告原始 HOST、原始设备和共享搜索的比较成本；选中 CSD 子树保留数字估价与完整 `CSD Stage Estimates`。这既减少无用格式化，也避免把日志交付误计为设备执行时间。

`pg_sina_csd.profile` 的 `properties_ms` 表示候选物理属性分析，`compile_ms` 只表示最终编码，`estimate_ms` 表示数字成本计算。比较旧实现时应对照旧 `compile_ms` 与新 `properties_ms + compile_ms`；资源发现细分已包含在属性分析中，不能相加两次。`property_nodes/encoded_nodes` 区分属性访问和实际编码，`serialized_plans/fast_records` 区分最终通用与融合记录。

执行期仍按实际输入和内存处理分块与 spill；这不属于重新选择 Join 算法。

相同 RelWithDebInfo 构建，4 CU、256MiB workmem、64MiB FDM；PG shared_buffers=2GB、work_mem=16MB、max_parallel_workers_per_gather=4。修改前后交替顺序，各4次正式 Q2/Q9 冷启动 EXPLAIN，每次重启 PG 并清除 Guest page cache：

| 查询 | 总规划前→后 ms | 下降 | 最终编码 ms | 编码访问节点/执行 Const 数 |
| --- | ---: | ---: | ---: | ---: |
| Q2 | 122.602→95.452 | 22.14% | 0.925 | 21 / 1 |
| Q9 | 127.832→96.409 | 24.58% | 0.584 | 13 / 1 |

各列为自身4次中位数，不能要求分项中位数之和等于总中位数。扣除资源发现后，属性分析＋编码由Q2的33.677降至26.309ms、Q9的23.935降至16.520ms；数字估价由14.822/29.255降至2.635/16.922ms。后者也包含省去落选节点长文本的收益，不能全部归为编码加速。资源发现仍存在波动，已包含在总计中。

Q2候选/成功数保持323/248；Q9由308/306变为288/285，是物理成本参与PG正常剪枝的结果，没有设置候选数量上限。两阶段各4份计划均稳定；前后连接局部顺序可由PG重新比较，因此不声称两棵树逐节点相同。Q2为HOST Limit＋CSD子树，Q9完整CSD，均保留Hash Aggregate。原始日志、CSV、JSON及报告在 `results/planner-physical-final.cyUtRG/`。

正式Q2/Q9的host-only、csd-without-hpcp、SQLerate共6次冷启动执行全部成功且输出一致，记录在 `results/tpch.D2yG6s/`。本轮单次端到端延迟分别为Q2的973.238/319.552/391.404ms、Q9的2595.357/1163.489/1194.365ms；它们不是4次规划计时的中位数，不能把规划降幅直接当成执行器降幅。

HeapLayout/PlannerCost通过；路径回归 `results/planner-paths.RMft2n/` 验证全HOST零编码、Q9只生成一份执行记录、Q1/Q6融合记录、original Q17/Q20与CTE结构。`results/planner-encoding.zrMrZS/` 验证临时的分组聚合、含聚合的MATERIALIZED CTE、DISTINCT结果均与HOST一致，临时SQL已删除。融合CSF由正式Q1/Q6 EXPLAIN验证，不把通用分组执行称为融合执行。TPC-H验收仅使用正式Q2/Q9，没有运行全量。

## 剩余规划开销定位

本轮仅增加已有 `profile` 下的包含式细分及 backend 线程 CPU 计时，不改变物理选择、成本公式或执行器。正式 Q2/Q9 各四次交替顺序冷启动 EXPLAIN，资源与 PG 配置同上；算术平均如下，各行互斥，可相加。这里只测规划，不是 SQL 执行延迟。

| 阶段 | Q2 ms | Q9 ms |
| --- | ---: | ---: |
| 资源发现，含插件首次打开 | 35.079 | 37.030 |
| 属性分析，扣除资源发现 | 22.888 | 16.520 |
| PG 自身两轮搜索，扣除扩展工作 | 15.019 | 20.010 |
| 候选保留 | 8.595 | 7.119 |
| 数字估价，含部分缓存驻留观测 | 2.737 | 17.819 |
| 成本格式化 | 5.829 | 5.930 |
| PG 子树预览 | 4.447 | 3.150 |
| 最终设备编码 | 0.917 | 0.545 |
| 其余 | 3.141 | 1.337 |
| 合计 | 98.653 | 109.459 |

最大等待发生在首次 `csOpenCSx`：Q2/Q9分别33.554/35.433ms，整个资源发现的当前线程CPU仅3.038/3.028ms。每次打开记录到一次磁盘缺页；另一次独立perf抓到`pg_sina_read_resources → csOpenCSx → discoverPlugins → dlopen`的major-fault栈，主要指向插件冷加载及等待。实际探测CSF计算仅0.195/0.192ms，下载/激活/查ID合计约1.1ms，HPCP接收缓冲不在此路径初始化。资源与插件按backend缓存，相同backend后续规划不再重复资源发现；前移初始化必须单列冷启动成本，不能声称该等待消失。

Q9的页缓存驻留观测累计16.313ms，其中`mmap/mincore`13.840ms、驻留位统计1.930ms；CPU为16.306ms。它检查12个关系/索引、367442个4KiB页（约1435MiB文件范围），没有读取表数据。Q2相应为10个关系/索引、约192MiB、2.585ms。一次规划内相同OID已经复用观测；这些时间分布在HOST修正及设备估价等父阶段，不能再叠加到上表。主要成本是逐页缓存观测，不是Join/spill公式。

Q2/Q9仍检查323/288个候选，只最终编码21/13个节点及一份程序。属性分析仍有PG树复制、裁列、列来源/表达式描述；候选保留中的复制为8.252/6.888ms。每个候选还把15个浮点成本格式化为PG Float字符串，再随候选复制，形成约6ms开销。用户态CPU采样中`AllocSetAlloc/copyObjectImpl/equal/annotate_device`频繁出现，与墙钟细分相符。因此后续应优先减少候选树复制、复用共享属性、延迟成本字符串生成，以及降低缓存观测成本；最终编码已经不是大头。保留两轮PG搜索的语义，不能靠删候选或跳过缓存修正降低计时。

原始计时、缺页栈、CPU采样及CSV/JSON见`results/planner-bottleneck.zn6b1R/`。四次计划去掉客户端Time行完全一致。客户端EXPLAIN还比hook多约24/21ms，包含hook外工作，未逐函数归因，不将其称为设备或纯规划时间。HeapLayout/PlannerCost两项CTest及现有路径回归`results/planner-paths.CckPDv/`通过。只运行Q2/Q9冷启动规划，未重跑执行结果对照或全量TPC-H。

## 连接初始化与数值候选成本

设备连接与程序现在属于PG backend，在`_PG_init`中完成发现、打开、CU/FDM查询、14个CSF入口下载/激活和workmem容量探测；`on_proc_exit`统一停用、卸载并关闭。当前Guest的`tpch_s1`已配置`session_preload_libraries=pg_sina_csd`，无需查询前显式LOAD；不能在postmaster的shared_preload阶段建立SNIA设备连接。并行PG worker只注册扩展方法，主连接负责设备资源。普通LOAD同样立即初始化，不再由planner或执行器首次触发。

CSF代码与每查询生成的设备执行记录不是同一对象。前者连接内复用；后者仍在选中计划后编码，并随查询参数、AFDM、batch及算子私有状态管理。多个设备子树共享同一CSF，状态由独立AFDM隔离，不再按子树重复下载代码。

为保留原先异常结束时的spill兜底清理，最后一个活跃设备子树释放后调用`csReset(device, CS_VENDOR_SPECIFIC_TYPE, 0)`。这是FEMU厂商资源0的项目约定，只关闭空闲的临时设备图会话、回收遗留token及HPCP接收状态，不关闭NVMe描述符或卸载CSF；有在途设备图时拒绝reset。PG按仍活跃的子树/游标计数，不提前重置其他执行中的状态。既有固件命令足够，无需改变SQL算子或设备线协议。

候选成本改为二进制数值元数据，使用PG可复制的Const容器保存，只有全局选中后才转为PG Float字符串及详细EXPLAIN说明。`Path.startup_cost/total_cost`继续用PG成本单位；保留已有CPU/带宽/spill校准参数与公式，内部分项诊断仍可用ms。原始PG树设备候选的比较成本也直接保存数值，不再提前字符串化。

准备后的候选树在独立规划内存上下文中保存，拒绝时删除、接受时直接保留，消除原先临时估价结束前再次深复制整个树的步骤。为修改隔离、构造预览和PG setrefs保留的复制仍存在，本轮不重写整个Path属性系统。

相同RelWithDebInfo构建，4CU、256MiB workmem、64MiB FDM及前述PG参数；正式Q2/Q9各四次前后交替冷启动EXPLAIN，每次重启PG并清Guest页缓存。以下为算术平均，初始化单列，不将移动边界称为消除工作：

| 查询 | 规划前→后 ms | 规划线程CPU前→后 ms | 新连接初始化 ms | 格式化前→后 ms | 候选保留前→后 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Q2 | 101.266→59.020 | 63.712→52.343 | 41.517 | 5.741→1.553 | 8.762→1.873 |
| Q9 | 110.118→64.048 | 71.678→58.601 | 40.796 | 6.170→1.137 | 7.152→1.369 |

旧资源发现30.885/34.562ms已从规划移出，新初始化还加载全部CSF。初始化加规划约100.537/104.844ms，不是连接端到端计时；不能声称冷启动整体下降42%。可确认减少的是每查询规划CPU约18%、候选字符串格式化与深复制。Q2候选/接受仍为323/248，Q9仍为288/285；最终分别编码21/13个节点、各一份执行程序。前后各四份计划去掉客户端Time行完全相同，包括路径、连接顺序和估价。

缓存观测方法未改，仍是必要成本；Q9本轮约16.357→13.646ms的差异不归为缓存算法优化收益。剩余主要是属性分析、PG搜索和缓存观测；最终设备编码仍非主要瓶颈。

PG两项CTest、规划路径回归`results/planner-paths.yZHj17/`及SNIA的`csDiscoveryIdentify`通过，未运行低于64MiB的压力场景。新`tests/guest/connection-lifecycle-test.sh`验证一次连接初始化、两轮正式Q2/Q9、取消后重跑与HOST输出一致；最终记录为`results/connection-lifecycle.2Mp6Qh/`。单连接下载CSF计数为14，激活/退出停用合计28，无逐查询重复加载。额外开发排序验证使用64MiB预算，同连接连续两次spill读写各33MiB成功，临时SQL已删除，不作为TPC-H验收。

最终正式Q2/Q9在HOST、CSD无HPCP、SQLerate和filter-only共8次冷启动执行均正确，见`results/tpch.OaANdF/`；Q1/Q6融合路径另有三模式正确性对照和最终清理复核。未跑全量。原始前后计时、CSV/JSON及报告见`results/planner-connection.FgBQhq/`。

## 候选属性与缓存观测的进一步优化

本轮仅修改Guest规划层，不改SNIA、固件或设备SQL算子。参照本地DuckDB的列绑定解析与裁列实现，先快速排除不同NodeTag/Var绑定，再由PG equal检查完整类型、collation等字段。函数身份检查改为一次pg_proc syscache访问，不再反复分配函数名。关系锁保护的列布局和文件节点在一次规划内复用，候选自己的列槽/谓词限制仍独立；不跨查询缓存统计或布局。

成本候选不再提前翻译、复制交付PG的输出表达式；全局选中后才构造setrefs所需输出。仅供预览的包装在setrefs前移除，借用其私有子树已有targetlist。保留完整准备副本以隔离裁列/物理改写，不改变算法选择、参数处理和搜索空间。

缓存观测首选Linux cachestat，向内核查询完整文件范围的驻留页数，不采样、不读取Heap；不支持时保留mincore回退。观测仍每次规划重做，能反映HPCP和驱逐变化。内核汇总不意味着全热文件也是常数开销。接口参考：[Linux cachestat](https://www.man7.org/linux/man-pages/man2/cachestat.2.html)。

相同RelWithDebInfo构建、4CU/256MiB workmem/64MiB FDM及前述PG参数。最终版本与47c55c764交替测试正式Q2/Q9，每版本每查询四次独立冷启动EXPLAIN；以下为算术平均，均为规划阶段ms：

| 查询 | 规划前→后 | 规划CPU前→后 | 属性分析前→后 | 缓存观测前→后 |
| --- | ---: | ---: | ---: | ---: |
| Q2 | 61.932→55.863 | 52.735→49.532 | 29.743→25.642 | 2.319→0.275 |
| Q9 | 68.023→51.227 | 59.983→44.428 | 22.108→19.025 | 13.892→0.299 |

总规划下降约9.8%/24.7%，CPU下降约6.1%/25.9%。属性下降包含冷映射检查波动；扣除mapping_ms后，属性分析约24.046→22.517ms、18.419→16.615ms，不将全部墙钟下降归因于元数据复用。缓存计时包含在估价/HOST修正等父阶段中，不能再次相加。Q2只读取5次表列布局、复用1187次；Q9为6/1059。最终版本均走cachestat，没有mincore回退。

前后各4份冷计划完全一致，包括Join顺序、路径及成本；候选/接受仍为323/248、288/285，最终编码21/13节点、各一份程序。128MiB临时文件在冷、部分驻留、非整页范围、全热和驱逐后，两个内核API计数均一致，探针与数据已删除。正式Q9经HPCP填充后的单次准确性检查中，Q2/Q9驻留86.02%/94.25%，前后完整计划与成本仍相同；观测耗时约5.136→3.546ms、37.950→25.240ms。这些暖缓存检查不加入冷启动统计，也不作为稳定的多轮性能收益；高驻留观测仍是剩余热点。

PG两项CTest、路径回归`results/planner-paths.c9afD8/`和连接复用/取消恢复`results/connection-lifecycle.CekELo/`通过。正式Q2/Q9四种相关模式共8次冷启动结果一致，见`results/tpch.xoDncq/`。没有优化前端到端多轮基线，不宣称执行器加速；未跑全量TPC-H或低于64MiB压力场景。原始数据、CSV/JSON及报告见`results/planner-observation.GJojw4/`。

本轮最终版本另执行正式Q1/Q6的SQLerate路径，均与既有同快照HOST结果一致，记录在上述结果目录的`fused.*`，不将复用参考结果当成新的主机性能测量。

## 候选子树复制与裁列复用

预览父候选时只读借用已经准备的设备子树，仅复制可变的输出列标签。PG完成预览后，包装只保留标签，避免在准备副本中再次复制包装的表达式。准备阶段再深复制整个候选，移除包装并恢复标签、InitPlan及参数集合；之后才进行修改，保证HOST和其他候选不受影响。

准备副本记录其中已裁列的子树根。如果父节点保留该子树全部输出，直接复用其内部裁列结果；需要更少输出时仍重新收集依赖、裁列并更新排序/分组列号。排序要求和物理算法仍按新父节点重新判断。表达式属性描述仍按候选分析，不跨查询保存可变状态，也没有新增通用属性缓存框架。

与68ec72afe交替测试，正式Q2/Q9每版本各4次独立冷启动EXPLAIN；构建及4CU/256MiB workmem/64MiB FDM、PG参数与上轮相同。下表为算术平均、单位ms，初始化仍在连接阶段：

| 查询 | 预览+准备复制+裁列 前→后 | 裁列 前→后 | 规划CPU 前→后 | 总规划墙钟 前→后 |
| --- | ---: | ---: | ---: | ---: |
| Q2 | 13.614→11.709 | 3.345→2.132 | 48.414→46.796 | 52.844→53.660 |
| Q9 | 10.701→10.229 | 2.551→2.082 | 44.297→43.651 | 51.089→51.278 |

三个相关阶段合计减少约1.90/0.47ms，规划CPU减少约3.3%/1.5%；总墙钟没有稳定下降，不能宣称端到端加速。prepare_copy自身包含包装移除等新增工作，因此不能仅用preview的下降报告收益。属性描述仍约9.24/7.19ms；冷映射等待与PG搜索/调度波动仍在，后续若继续大幅优化需要改变更大范围的属性表示或搜索流程，不属本轮小改动。

Q2每次借用374次、裁列复用337次；Q9为447/340次。前后各4份计划完全相同，包括成本、连接顺序、HOST/CSD边界；候选/接受仍为323/248、288/285，最终编码21/13节点、各一份程序。原始计时、CSV/JSON和报告见`results/planner-subtrees.UomkVv/`。

最终两项CTest、规划路径回归`results/planner-paths.NwjUfK/`、连接复用/取消恢复`results/connection-lifecycle.nw9nyi/`通过；正式Q2/Q9在HOST、CSD无HPCP、SQLerate、filter-only共8次冷启动结果一致，见`results/tpch.iOwnoT/`。未运行全量TPC-H或低于64MiB压力场景。

## 强制CSD与SQLerate共享物理规则

通用树统一调用`pg_sina_prepare_candidate`，在同一准备流程中恢复全流行数、裁列并选择物理算法；描述候选成本和编码执行图都使用准备后的算法。原先仅在强制模式编码时执行的Hash构建侧交换、无序Merge转Hash、分组Sort移除等规则前移，删除重复的编码分支。全表非覆盖索引扫描若仅为分组提供顺序、没有自身过滤或上层顺序需求，两模式均可改为顺序批量扫描＋Hash Aggregate。PG已经证明的函数依赖负载列仍纳入物理分组键，必需的结果顺序在聚合后保留。

SQLerate仍可选择不同连接顺序，强制模式继续采用其PG选中树；共享规则不等于强制所有模式使用同一计划。filter-only保留扫描卸载边界，专用融合CSF及设备程序没有修改。DISTINCT等图形转换仍在候选属性描述时确定并冻结，选中后编码按已记录选择实现；索引连接是否有残余过滤也加入既有一致性检查，不新增设备ABI字段。

Index Join原先跳过右侧整表扫描，却没有单独计入探测和回表。现在用左侧探测行数、右侧PG每次查找行数、键NDV、表/索引页数和PG物理相关系数估价；没有NDV时使用PG查找行数。过滤掉的候选仍可能已经发生回表及解码，只有无需残余过滤的Semi/Anti探测才按首次接受即可结束计价。PG放在内侧索引扫描中的相关过滤会移到设备连接残余条件，属性描述与编码使用同一条件集合。

有界缓存估计采用页占用近似：访问V次、P页的均匀分布，预计不同页数为`P*(1-exp(-V/P))`。按现有256键批次分别计算整批和尾批，对128页游标缓存暂按索引/Heap各半分配，取全程不同页数与批次缺页合计的较大值；因此重复读取可以超过表大小，不会误当成无限缓存。Heap访问数在随机逐元组与聚簇按页读取之间按PG correlation平方插值。另计B-tree查找、页标签检查、候选解码、读请求波次，以及HPCP链路工作；使用已有带宽、CPU和调度校准参数，未新增GUC。`CSD Stage Estimates`记录连接类型及`probes/index_pages/heap_pages`，这些数值属于模型估计，不能当成实际I/O计数。

残余自反连接还会触发PG的一行基数下限，使后续Index Join近似免费。仅当Anti Join有残余条件、估计不超过1行，且等值键来自同一列或经已验证单列外键关联的键域时，采用本地DuckDB `CardinalityEstimator::DEFAULT_SEMI_ANTI_SELECTIVITY=5`的兜底，即保留左侧20%。这是统计不足时的启发式，不是测得的选择率；外键只用于确认键域关联，不推断所有父键都出现或将父表行数当作实际NDV。Inner Join在左右任一子树行数被修正后，都会传播相应比例，避免Hash构建侧交换后丢失右侧修正。没有按查询编号或SQL文本设置规则。

最终版本在原有RelWithDebInfo、4CU/256MiB workmem/64MiB FDM及2GB shared_buffers、16MB PG work_mem、4个PG worker上限下，仅测正式Q10/Q18/Q21。每个模式各一次独立冷启动，profile统一开启；本表为psql总时间ms，旧SQLerate来自修改前完整测试`results/tpch.i6nyie/`，不是交替多轮均值：

| 查询 | 旧SQLerate | 新HOST | 新CSD无HPCP | 新CSD有HPCP | 新SQLerate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Q10 | 2635.474 | 2158.782 | 1499.494 | 1523.303 | 1564.737 |
| Q18 | 3991.048 | 8694.642 | 817.322 | 855.366 | 848.360 |
| Q21 | 2714.634 | 1915.699 | 915.829 | 1025.257 | 1210.827 |

Q10/Q18的SQLerate与强制CSD最终设备图相同，执行主体分别约1461/795ms，对照强制HPCP约1482/811ms；Q18已不再走全量有序索引聚合。三条SQLerate均无实际spill。Q21仍选择先Hash连接orders、再进行两次lineitem索引探测，最终LIMIT留在HOST；强制CSD将orders索引探测放在后面。两者内部读取约2.262/1.552GB，执行主体1114/943ms，总时间仍相差约186ms。规则统一和严重回退修复已完成，但Q21候选排序仍非实测最优；20%兜底、均匀缓存及CPU校准仍有近似，不能宣称SQLerate总是最快，也不能把差距归因于物理内存不足。

原始计时、CSV及实际计划见`results/tpch.Y91s4R/`。两项CTest通过；`results/planner-paths.8mMbBe/`增加两模式Q18聚合选择和Index Join读页估价回归，原有融合路径、HOST选择、original Q17/Q20及CTE仍通过。`results/planner-cost.nUoyRT/`验证正式Q13在64MiB查询预算下结果及估价字段；旧测试中已失效的Node Cost Estimates断言更新为当前CSD Stage Estimates。未运行完整TPC-H或低于64MiB的场景。

## 三轮冷缓存与全热缓存对照

本轮固定设备实现为b25f64645，沿用4CU、256MiB workmem、64MiB FDM及上述PG配置。仅为测试脚本加入`PG_SINA_CACHE=warm`，不改规划器、算子或查询。每条件、每查询、每模式独立测量3次，共660次，查询采用既定22条编号SQL；条件与非HOST模式次序按轮交替。热测每次仍先重启PG并清缓存，再顺序预热表/索引文件，用官方pg_prewarm填充shared_buffers，分别以fincore及pg_buffercache核验100%驻留。约1.43GiB文件可同时放入8GiB Guest的文件缓存及2GiB PG缓冲；预热、核验和连接初始化不计入查询时间。

下表是22条查询各自总时间中位数之和，单位秒，不是连续执行一组查询的实测时间。profile统一开启，auto_explain不启用逐节点ANALYZE/TIMING。

| 模式 | 冷缓存 | 全热缓存 |
| --- | ---: | ---: |
| host-only | 48.034 | 25.023 |
| filter-only | 35.797 | 23.696 |
| csd-without-hpcp | 14.549 | 14.120 |
| csd-with-hpcp | 14.731 | 14.301 |
| SQLerate | 15.935 | 14.880 |

660份输出跨模式、轮次及冷热条件一致，330次热测缓存核验均通过，实际CSD spill读写全部为零。选中设备子树的文件驻留观测由冷测约0–0.14%升至热测100%；每个条件内三轮路径和物理计划形状均一致。冷测为19条全CSD，Q2/Q21的LIMIT留在HOST，Q20为混合；热测为17条全CSD，Q2/Q20仍混合，Q8/Q19/Q21改为全HOST。Q20两种条件下均由CSD处理lineitem/part/partsupp/supplier，HOST处理nation索引扫描、Nested Loop和最终Sort。

缓存感知有效，但最优路径选择未达标。SQLerate冷测22条均快于原生PG，热测仅13条更快。热测Q3/Q4/Q5/Q7/Q10仍选CSD，而原生HOST执行主体更快；Q10的HOST/SQ总时间为631.801/1101.494ms、执行主体为571.014/1020.596ms，模型却估HOST2931.132ms、共享CSD700.793ms。HOST修正目前基于文件驻留及估算扫描量，没有单独建模PG shared_buffers；不能仅因观测到100%就认为HOST成本准确。

Q8热测虽选全HOST，但共享搜索从lineitem全扫描及part Hash Join开始，原生PG则先过滤orders等关系后用lineitem_pkey探测。两者执行主体800.953/448.749ms，SQLerate总时间895.996ms，原生470.961ms；这是HOST物理候选排序问题，不只是81.681ms规划开销。Q19/Q21采用原生HOST树，Q21主体736.058ms接近原生712.978ms，但91.520ms规划使总时间增至846.387ms，对照749.093ms。Q20混合路径的热测主体831.846ms，完整HPCP卸载434.083ms，模型仍错误地偏好混合候选；差距不能归于内存不足。

SQLerate规划中位数逐查询合计由冷676.504ms升到热995.211ms，其中缓存观测由3.326ms升到460.445ms，后者包含在前者内。阶段中位数独立计算，不能要求相加等于总时间中位数。设备始终内部NVM→FDM，不输入主机缓存数据。原始六组数据、全部均值/中位数/范围、阶段时间、实际计划、复现脚本与图见`results/tpch-cache.lYsAOh/`；本轮没有修正成本模型，也不将正确性通过当作性能验收通过。
