# SQLerate 节点级路径规划

Guest PG 负责 SQL 解析、去关联和连接顺序。`src/planner/cost.c` 在 PG 选中物理树后，对节点逐层估计 HOST 和 CSD 成本，不按 TPC-H 查询编号选择路径。设备仍只接收通用 SNIA 请求/设备图，不参与 SQL 成本规划。

## 两种候选状态

对节点 v，H(v) 表示该节点在 HOST 执行、其子树可按成本选址；D(v) 表示以 v 为根的完整子树在 CSD 内执行，结果尚未回到 HOST。边界成本 B(v) 是把 CSD 结果返回 PG、转换为 Datum 的成本。

```text
H(v) = HOST 本节点成本 + Σ min(H(child), D(child) + B(child))
D(v) = 可编译的设备子树的内部 I/O + CU 计算 + spill + 未被覆盖的 HPCP 工作
根节点选择 min(H(root), D(root) + B(root))
```

每个位置必须先通过现有算子编译器的语义与映射检查。HOST 结果行重新送入设备解释器尚未实现，故 HOST→CSD 中间行边界为不可行；不能假定它已经能运行。CSD 候选从已准备的基表/索引开始，可有多个独立子树向 HOST 的 Join、Aggregate、Sort 等返回数据。返回位置遵守 PG 的目标列、顺序、NULL 与表达式语义，子树边界保留索引/排序顺序。HOST Hash Join 的 Hash 子节点必须保留 MultiExec 接口；Merge Join 需要标记/恢复的内侧节点也保留在 HOST，它们下方仍可接入 CSD 输入。

PG Gather/GatherMerge 的 HOST 区域保留原并行执行，内部 HOST 成本仍递归观察缓存；SQLerate 不单独替换其 worker 扫描，若整个区域可编译，则可整体卸载。`filter-only` 保留 PG 原生并行计划：parallel-aware SeqScan 的参与者通过 PG 共享内存中的原子计数器领取互不重叠的设备输入块；每个参与者再通过现有 SNIA batch 驱动 CU。并行索引扫描暂留原生 PG，避免破坏其分片/顺序语义。PG 上层的 Partial/Final Aggregate、Gather 和 Join 不被强制串行化。

`csd-without-hpcp` 和 `csd-with-hpcp` 不按成本拒绝已支持的完整子树；完整编译失败时继续寻找可卸载的子树。不支持的语义继续由 PG 执行。`host-only` 不加设备节点或 SQLerate 外层节点。

## 成本来源

所有 SQLerate 成本以毫秒表示，和 EXPLAIN 原有 PG cost 单位区分。

- **HOST**：以 PG 节点增量成本为基础，减去子树避免重复计费。Nested Loop 对重复内侧执行估计重复次数。扫描按文件缓存驻留比例减少存储读取成本、加入缓存扫描成本。
- **缓存**：规划时按关系文件段只读建立 PROT_NONE 映射并调用 mincore，不读取 Heap。观察结果只在本次规划内复用，不保存在跨查询全局缓存。它是 Guest OS 文件页缓存的快照，不是精确的 PG shared_buffers 命中率，也不保证执行时页面仍驻留。
- **设备资源**：通过现有 SNIA 查询 CU/FDM，并调用已有 workmem 容量 CSF 读取计算内存。设备 CPU 成本按串行、可并行任务波次、调度和合并阶段分别估计；结合 4/2/1MiB 输入和列向量/控制/结果缓冲估计 FDM 可支撑的并行度。排序归并等串行阶段不按全部 CU 数等比例缩短。
- **内部 I/O**：按估计的 Heap/索引读取字节和内部带宽计费；不是再加一次主机 NVMe Read。索引估计目前是按行选择率、物理页面和索引大小推算，不是逐页执行模拟。
- **工作内存与 spill**：按设备压缩行、哈希桶、分组槽位、排序 run、字典和物化状态推算工作集，考虑生命周期及仍存活的子状态。超出设备 workmem 的部分估计写出、换入及额外归并轮次；不因固定低水位就提前判定需要 spill。该值是估计，实际执行仍由统一 workmem 分配器和既有 spill 协议控制。
- **CSD→HOST**：按有效投影列的紧凑行、批次头和字符串估计返回字节，按同一批次容量估计交付次数；再加入共享 PCIe 和 Datum 转换成本。大量返回行的扫描卸载可能因此比完整卸载或原生 PG 更昂贵。具体布局和测量见 [filter-only 执行](filter-only.md)。
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
