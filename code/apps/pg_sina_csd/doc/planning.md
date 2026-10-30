# SQLerate 节点级路径规划

Guest PG 负责 SQL 解析、去关联和连接顺序。`src/planner/cost.c` 在 PG 选中物理树后，对节点逐层估计 HOST 和 CSD 成本，不按 TPC-H 查询编号选择路径。设备仍只接收通用 SNIA 请求/设备图，不参与 SQL 成本规划。

设备编译保留 PG 的关系配对与连接层次；INNER Hash Join 可参考 DuckDB 按估计行数×行宽交换 build/probe，避免把较宽的输入构建为哈希表。这里不重新关联多表连接，也不改变 HOST 原生计划。RIGHT JOIN 在设备计划中交换两侧，复用 LEFT JOIN 的匹配与 NULL 补齐，表达式按交换后的输入重新绑定，输出列仍遵循原计划。

## 两种候选状态

对节点 v，H(v) 表示该节点在 HOST 执行、其子树可按成本选址；D(v) 表示以 v 为根的完整子树在 CSD 内执行，结果尚未回到 HOST。边界成本 B(v) 是把 CSD 结果返回 PG、转换为 Datum 的成本。

```text
H(v) = HOST 本节点成本 + Σ min(H(child), D(child) + B(child))
D(v) = 可编译的设备子树的内部 I/O + CU 计算 + spill + 未被覆盖的 HPCP 工作
根节点选择 min(H(root), D(root) + B(root))
```

每个位置必须先通过现有算子编译器的语义与映射检查。HOST 结果行重新送入设备解释器尚未实现，故 HOST→CSD 中间行边界为不可行；不能假定它已经能运行。CSD 候选从已准备的基表/索引开始，可有多个独立子树向 HOST 的 Join、Aggregate、Sort 等返回数据。返回位置遵守 PG 的目标列、顺序、NULL 与表达式语义，子树边界保留索引/排序顺序。HOST Hash Join 的 Hash 子节点必须保留 MultiExec 接口；Merge Join 需要标记/恢复的内侧节点也保留在 HOST，它们下方仍可接入 CSD 输入。

PG Gather/GatherMerge 的 HOST 区域保留原并行执行，内部 HOST 成本仍递归观察缓存；SQLerate 不单独替换其 worker 扫描，若整个区域可编译，则可整体卸载。`filter-only` 的共享设备扫描在查询主进程初始化，随后由 SNIA 完成回调推进，直接填入两个有界 HOST 批次；PG 原生 workers 仅消费数据、执行 HOST 算子，不申请 AFDM 或续交设备请求，没有“PG 提交者”选举。Gather 下需要完整重读的私有 Scan 留在 HOST，并行索引扫描保留原生分片/顺序语义。PG 上层 Partial/Final Aggregate、Gather 和 Join 的原有并行保留，执行与验证见 [filter-only 执行](filter-only.md)。

`csd-without-hpcp` 和 `csd-with-hpcp` 不按成本拒绝已支持的完整子树；完整编译失败时继续寻找可卸载的子树。不支持的语义继续由 PG 执行。`host-only` 不加设备节点或 SQLerate 外层节点。

## 成本来源

所有 SQLerate 成本以毫秒表示，和 EXPLAIN 原有 PG cost 单位区分。

- **HOST**：以 PG 节点增量成本为基础，减去子树避免重复计费。Nested Loop 对重复内侧执行估计重复次数。扫描按文件缓存驻留比例减少存储读取成本、加入缓存扫描成本。
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

此节对应当时的SQL版本。Q17/Q20随后已恢复原始相关子查询，表内这两条的旧成绩及包含它们的合计/加速比，不再作为当前版本的基线；恢复后的测试见本文末尾。

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

该次检查的Q17/Q20仍是手写GROUP BY派生表版本。现已恢复原始相关SQL，原生PG计划新增这两条的相关SubPlan；完整CSD模式的自动改写见下面的针对性验证。不能将先前旧SQL没有SubPlan的现象用于证明PG或本项目支持任意相关子查询。

## Q17/Q20恢复原始相关子查询

`tests/guest/queries/17.sql`、`20.sql`及各自的explain脚本已恢复TPC-H相关子查询结构，对照仓库DuckDB dbgen的q17/q20模板核对。Q17保留`Brand#54`、`JUMBO BAG`、7.0及0.2；Q20保留`goldenrod%`、1996-01-01起的一年日期区间、`JORDAN`及0.5。未添加LIMIT，查询/explain正文一致，也未更改索引或PG配置。AGENTS.md明确要求验收SQL保留原始结构，去相关必须由扩展完成。

本轮只使用现有部署，运行`PG_SINA_PROFILE=off TPCH_MODES='host-only csd-without-hpcp' sh /home/femu/tests/tpch-test.sh 17 20`。每个组合独立重启PG并清除Guest page cache，沿用脚本120秒statement_timeout；设备4 CU、256MiB workmem、64MiB FDM，PG shared_buffers=2GB、work_mem=16MB、max_parallel_workers_per_gather=4。记录在`results/tpch.TfoGfB/`：

| 查询 | 原生HOST | 完整CSD无HPCP |
| --- | ---: | ---: |
| Q17 | 120秒超时，未完成 | 1378.759ms |
| Q20 | 120秒超时，未完成 | 509.227ms |

针对性计划在`results/q17-q20-correlated-plans.9jAqIE/`。原生PG两条均保留相关SubPlan，对lineitem重复顺序扫描；现有lineitem只有`(l_orderkey,l_linenumber)`主键索引，不能按相关的part/supplier键直接定位。超时不是完整查询的完成延迟，不将其填成120秒的正式成绩，也不重用旧派生表写法的HOST时间。

两条完整CSD均成功执行。记录中的原始Query Text保留相关AVG/SUM、没有手写GROUP BY；CSD计划已经包含Hash Aggregate和Join。由此确认现有`src/planner/rewrite.c`在PG规划前完成了去相关，无需在测试脚本或设备固件中另写规则。Q17实际workmem峰值86412864B，Q20为88968976B，spill读写均为0，结束后FDM_USED=0。

正确性与执行成功分开：本轮HOST均未完成，因此不能完成同轮HOST/CSD逐行结果验收；脚本`result_match=no`来自缺少HOST参考，不能解释成已观察到结果不一致。仅读取历史`tpch.BW5CB4`中等价旧SQL的既有HOST输出作辅助核对，两条CSD输出逐行一致，没有重新执行旧SQL替代验收。原始写法在当前120秒限制内的直接HOST结果比对仍未完成。

本轮未运行其他TPC-H查询，也没有为获取更快HOST成绩添加索引、调整规划开关或延长超时。
