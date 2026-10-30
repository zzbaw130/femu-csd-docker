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

workmem 峰值103780640B（约99MiB），FDM预留28359496B，spill为0。当前统计不支持“256MiB实际计算内存不够”的解释；固定缓存策略、索引批次的等待以及后续阶段的串行部分仍可优化，也不能仅凭上述累计计数精确预测修改后的收益。

### Q12：扫描过滤仍是主要CPU工作，索引等待降低其重叠程度

完整 CSD 的主阶段1485.421ms，累计 worker CPU 3739.068ms，平均活跃约2.52个CU；filter-only 的扫描阶段900.338ms，累计 CPU 3246.777ms，平均活跃约3.61个CU。filter-only 中CSD扫描过滤可以和HOST索引探测/聚合重叠；完整卸载后同一组4个CU既承担过滤又承担索引连接，缺页挂起期间虽然不占CU，却仍保留该任务块的输入，不能假定它已经切换到另一个独立块继续扫描。

完整 CSD 的 Scan Decode/Filter CPU 分别为1128.094/2444.455ms，filter-only 分别为1044.206/2165.420ms。复杂谓词仍是两边共有的热点，不能把整段Filter累计时间都算成完整卸载多出的开销。实际增量还包括orders索引读取、投影和较低的扫描/索引重叠程度。两条路径均处理31235次有效订单匹配，HOST索引读取21850页、设备载入24614页。

完整 CSD workmem峰值3052672B（约2.91MiB），FDM预留24140568B，spill为0；聚合Sink CPU约1.4ms、Sort Sink CPU约0.06ms、结果读取约0.058ms，不是主要瓶颈。HOST逐节点诊断及buffer计数在 `results/q10-q12-joins.QizqKg/`，其中开启ANALYZE/BUFFERS/TIMING的耗时仅用于定位，不作为正式成绩。

因此Q10/Q12有共同的内部随机读取问题，但主要CPU构成不同。应优先修正通用内部读的批内并发和完成归并，再核对索引缓存命中与任务块周转；Q12还可减少复杂谓词的CPU工作。未做实现前后A/B，不给出这几项优化的确定加速承诺。SQLerate也需把索引缺页及多轮读取计入候选成本，不能仅按总字节/带宽估价。本次只诊断，未修改固件或执行计划；结束后FDM_USED=0。
