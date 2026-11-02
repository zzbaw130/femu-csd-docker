# Host Page Cache Population（HPCP，主机页缓存填充）

本功能用于只读、extent 固定的实验数据。默认关闭，应用设置 `SNIA_FEMU_HPCP=1` 后启用，不改变公开 SNIA API。文件注册仍由 PG 扩展或测试应用执行；普通 SNIA 应用若未注册文件，只接收并丢弃无法关联到文件的数据。

## 运行位置与流程

- **Guest 用户态**：`libsnia` 的 FEMU 插件管理接收缓冲和一个库内消费线程，不启动辅助进程或代理。
- **Guest 内核态**：本目录的 `femu_cache.ko` 固定 DMA 接收页、提供完成记录映射，并把收到的数据复制到干净的文件页缓存。
- **设备端**：FEMU `hw/femu/csd/csd.c` 的 LOAD 与 CSF 调度；两个设备级HPCP线程执行主动软件 DMA；正常 I/O 由两个 NVMe poller 处理。设备不理解 inode、PG 表或文件偏移。

1. 上层将文件、namespace LBA 和文件偏移注册到 `/dev/femu-cache`。文件保持只读，不改变 extent；开启HPCP时，LOAD 输入在图内也保持只读，算子使用独立输出缓冲，不原地修改这份基表输入。
2. 模块加载时预分配 **8 个 4MiB 大槽和32个64KiB小槽**（合计34MiB Guest 内存）和一页完成记录，完成固定页及 DMA 映射。显式大 LOAD 保持原粒度，内部不超过64KiB的读取使用独立小槽，不占大槽。这是设备环境初始化成本，应单独报告。SNIA 首次提交开启HPCP的设备图时借用这组缓冲，通过一次私有 NVMe 注册命令绑定当前会话；查询结束后归还，下次直接复用。模块保存本次会话的文件映射引用。并发会话需要独立缓冲，预分配组已借出时按需分配额外接收区；不会串行化 SQL 执行。
   PG注册extent时，还提供查询开始时的实际驻留位图：只有该4KiB页面满足全部已启用填充目标时才标记可跳过，同时启用HPCP/DCP时要求文件页和对应PG页均驻留，或已满足既有DCP bulk ring目标。模块将这些位图转换为namespace页位图，设备据此在登记和DMA前跳过已满足的范围；接收端真正发布成功后才补充标记。位图仅是本次可选填充的提示，每次绑定重建，不替代数据读取，也不把“已发DMA”当成“已驻留”。
3. 设备图显式 LOAD 和 CSF 通过可重入 namespace read 请求的内部基表读取都接入HPCP，最多按4MiB分块；隐藏 spill 的换入不参与 HPCP。计算按原依赖继续推进；两个HPCP线程从保留块中领取任务，将同一份输入 DMA 到空闲接收槽位。DMA 复用共享 PCIe 带宽模型，没有二次 NAND/FTL 读取延迟。数据复制不占用 FTL 或正常 I/O poller 线程，也不持有 CSD 全局锁。HPCP线程空闲时等待条件变量；仅在等待尚未到期的 PCIe 模拟完成时间时进行短等待。
4. 软件复制和模拟链路传输均完成后，设备写入来源 LBA、长度等元数据，最后发布槽位的 `ready`。**没有 AER、逐块日志读取、F3 Read 或 ACK 命令。** 没有空闲接收槽位时跳过HPCP，不等待页缓存处理。
5. SNIA 库内线程检查完成记录，调用模块消费该槽位；启用DCP时由PG后端接手，先安装shared_buffers，再在释放槽时发布HPCP。模块把接收数据复制到缺失的文件缓存页，已有有效页不覆盖，脏页、冲突等情况跳过；不产生文件写入或 NVM 写回。处理完成后更新共享 `ready`，归还槽位，不发送 NVMe 请求。
   Guest发布时，同一输入范围内复用已匹配的extent；会话已持有文件引用，无需每4KiB重新扫描全部映射和增减文件引用。
6. CU 和 DMA 都结束后，设备输入可以复用。原 FDM 输入同时供 CU 和 DMA 读取，保留到二者结束；不再建立设备页别名或重复映射，仍不等待主机填入文件缓存。接收槽位是 Guest 内存，不计入设备 FDM。
   已完成或已放弃DMA的设备填充记录即可回收，不再等CU结束才退还这128项元数据。CU/图节点自身持有AFDM引用，因此回收填充记录不会释放正在计算的输入；它只提前归还可选填充队列容量。
   数据复制及模拟链路传输结束后，即允许重叠区域被下一次读取覆盖，不再为完成记录的发布继续占用该输入。HPCP线程使用专用缓存元数据锁，完全不获取 CU/FTL 的执行调度锁；接收槽探测另用环锁，访问 Guest 内存时也不持有缓存元数据锁。FTL 只在状态改变后回收输入、推进等待节点，锁顺序固定为执行调度锁→缓存元数据锁。逐块计时日志在全局锁外输出，关闭细分计时时不输出 CACHE_BLOCK。
   CSF 重入时也必须保持内部读取的基表输入只读，使用独立输出和控制区域。下一次内部读取若要覆盖仍在 DMA 的重叠区域，设备只暂停该节点，DMA 完成后通过就绪队列唤醒；尚未开始 DMA 的可选HPCP可以直接放弃。细分计时的 `CACHE_REUSE_WAIT` 记录此等待，`CACHE_BLOCK` 的 `internal=1` 标识内部读取。
7. 关闭 SNIA 设备时，先停止并等待实际 DMA 写入退出，再排空消费线程，最后解除 DMA 映射。模块也在文件描述符异常关闭时执行停止，避免设备继续写入释放后的页。关闭阶段的排空耗时需单独考虑，不能宣称后台工作没有任何资源开销。

控制器暂停时会等待在途软件 DMA 退出；控制器复位会注销接收环，恢复后须重新注册，避免向旧 Guest 地址写入。设备关闭时唤醒并回收两个HPCP线程。8 个 I/O 队列按每 poller 4 个队列配置；正常 I/O 与HPCP仍共用同一个设备到主机 PCIe 带宽预算，并未增加链路容量或实现抢占优先级。

`0xeb` 是本项目的设备私有接收注册/停止命令，不是 SNIA 标准命令。头文件 `csd-cache.h` 定义初始化地址表和共享完成记录，按小端编码。接收页由模块持有到停止完成，不保存普通已完成 NVMe 请求的临时 PRP 映射。现实现针对 Linux 6.8、4KiB 页、PCIe FEMU 和 `/dev/nvme0n1`，注册和发布接口要求实验 Guest 已授权的 CAP_SYS_ADMIN。

## 编译与安装

Guest 已安装 SNIA、当前内核 headers、make 和 C 编译器：

```sh
mkdir -p /tmp/femu-cache-build
cp /home/femu/apps/femu-cache/femu_cache.c /home/femu/apps/femu-cache/csd-cache.h \
   /home/femu/apps/femu-cache/Makefile /tmp/femu-cache-build/
make -C /tmp/femu-cache-build -j4
sudo mkdir -p /home/femu/.local/lib/femu-cache
sudo cp /tmp/femu-cache-build/femu_cache.ko /home/femu/.local/lib/femu-cache/
sudo insmod /home/femu/.local/lib/femu-cache/femu_cache.ko
sudo chown "$(id -un):$(id -gn)" /dev/femu-cache
```

需要匹配的 FEMU 固件和 SNIA 插件，使用发行版原生 NVMe 驱动，无需修改内核 NVMe 模块。

## 数据库缓存消费者（DCP）

PG扩展可选择把设备返回的原始页填入自身的shared_buffers，称为Database Cache Population（DCP）。HPCP和DCP共用上述34MiB接收环。模块的接收数据使用可映射的零初始化vmalloc内存；除原有完成记录外，允许在`FEMU_CACHE_DATA_OFFSET`偏移以只读方式映射整个数据区。槽位布局和驻留提示属于私有设备协议，FEMU、模块、SNIA及PG扩展需要使用匹配的`csd-cache.h`重新编译。

调用者先SETUP、映射接收环，再通过SNIA `csConfig(CS_VENDOR_SPECIFIC)`传入`struct femu_cache_config { receive_fd; }`。这是本项目FEMU插件的厂商配置，不是SNIA定义的DCP API。适配器复制这个fd，绑定设备图会话；此模式不启动库内HPCP消费线程，接收槽由调用者独占消费。未配置外部消费者时，原HPCP后台线程行为保持不变。

每个ready槽依次执行`FEMU_CACHE_RX_ACQUIRE(slot)`、从只读映射使用数据、`FEMU_CACHE_RX_RELEASE(slot)`。ACQUIRE/RELEASE只做Guest端DMA同步和内存所有权交接，不提交逐块NVMe请求；RELEASE携带`FEMU_CACHE_RELEASE_HPCP`时，还会把同一份接收数据发布到文件页缓存，然后归还槽位。DCP完整处理该槽后附带`FEMU_CACHE_RELEASE_DCP`，模块仅在文件页也发布成功（或未启用HPCP）后标记设备可跳过的页。因此DCP-only不写文件页缓存，双开也只传输一份数据。调用者结束时先停止设备DMA，再排空已接收槽位并解除映射/关闭fd。

PG缓冲管理接口只能由PG backend调用，不能放进SNIA pthread；PG的具体导入逻辑在`pg_sina_csd/src/executor/cache.c`。当前使用PG12 `ReadBufferExtended(RBM_ZERO_AND_LOCK)`避免重新读盘，保留已有有效/脏页，仅安装完整且校验通过的8KiB页，不拼装跨接收块或extent的残缺页。只读镜像保证磁盘页与数据库版本一致。

### 数据大于缓存时的接纳限制

DCP开始时统计所有已占用的shared buffer（包括索引、系统目录等），只把达到`NBuffers`的90%之前的剩余页数作为本次新增填充预算。每安装一个缺失页扣减一次，预算用完就跳过后续DCP页，不再调用PG缓冲分配；下次设备执行开始时重新观测。保留10%给HOST工作，不按已提交字节数虚增缓存驻留。该策略不预测热点，也不改变PG自己的正常缓存替换策略；同查询内HOST执行仍可能改变缓冲内容，预算是保守的新增接纳上限，不是给DCP独占一部分shared_buffers。

HPCP每个接收块检查Guest实际空闲页，低于总RAM的1/8时只查找已有文件页，不再创建缺失页。新页使用不触发直接回收的`GFP_NOWAIT`分配，避免为了可选填充而同步回收旧缓存。不能以`MemAvailable`作为这一阈值，因为它包含可回收文件缓存，会允许不断替换已经填充的页。内核`CACHE_ADMISSION`记录因容量跳过的字节及预留内存；PG的DCP计时行记录初始/剩余页预算和因容量跳过的字节。

新填入的文件页在置为有效后调用一次`folio_mark_accessed`，对应这次设备读取的访问；已有有效页被跳过时不重复提升其回收优先级。这沿用Linux正常缓冲读取的页面访问处理，不锁定页面、不禁止回收，也不保证规划时驻留的页面在执行期间始终驻留。

两层独立决定是否继续接收。PG在首次提交设备图前读取已有页面位图：目标已驻留，或两个目标均无接纳空间时，通过已有SNIA厂商配置暂停本轮填充，不借用/注册接收槽。`FEMU_CACHE_RX_ADMISSION`只读查询Guest文件缓存的空闲页准入，不分配缓冲或发送设备命令；发布时若达到空闲页预留线或可选页分配返回ENOMEM，也报告停止接纳。DCP预算耗尽但HPCP仍需要且能接收时继续传输，反之亦然。

执行中两层都不再接收时，复用已有`RX_STOP/UNBIND`停止设备发送：等待正在软件复制的DMA离开，取消尚未发布的可选块，后续LOAD/CSF内部读取不再登记DMA、保留输入或探测接收槽，PG也停止轮询该环。已在途复制与STOP本身仍有一次性收尾开销，不能声称它们从未发生。正常LOAD、计算、spill及结果返回继续执行。模块初始化预分配的34MiB池仍保留供下次使用；停止发送不等于卸载模块或释放这个环境资源。下一轮设备执行/reset后按实际驻留和容量重新决定是否启用。

以上准入结果及`receive_fd=-1`暂停语义是本项目FEMU插件/接收模块约定，不是新增SNIA状态或设备固件命令。公开应用仍只使用`cs.h`和`-lsnia`。

SNIA 按其 [README](../snia/README.md) 编译安装。应用仍只包含 `cs.h`、链接 `-lsnia`；文件映射注册接口由本目录头文件提供，PG 已使用此接口。

## 验证与计时

```sh
cmake -S /home/femu/apps/femu-cache -B /tmp/femu-cache-user
cmake --build /tmp/femu-cache-user -j2
sudo /tmp/femu-cache-user/test-hpcp /data/hpcp.bin 1 0
sudo /tmp/femu-cache-user/test-hpcp /data/hpcp.bin 1 1
sudo cat /sys/kernel/debug/femu-cache/profile
```

测试使用 1GiB 文件、4MiB 输入、8 条流，重复数 1 为轻计算，可改为 100 测重计算。两种模式都从冷文件缓存开始，校验计算及文件字节；HPCP是尽力而为，不要求全部驻留。`wall_ms` 为任务执行时间，`cache_drain_ms` 在两种模式下都记录 CSF 卸载、关闭设备及排空后台消费，二者分别报告。每次查询测试仍须使用冷启动脚本，TPC-H 只使用正式 SQL。

设备 `CACHE_BLOCK` 记录 LOAD 完成、已知的CU完成、DMA 开始、软件复制结束、链路结束、完成记录发布及填充记录回收时间；其中 `dma` 是发布完成记录后的设备时间。记录回收时CU尚未结束则`cu=0`，不能据此声称CU耗时为0，完整链结束看`CACHE_GRAPH`；不能把并行图的等待相加当作查询延迟。Guest `CACHE_STATS` 记录块数、实际填充、已有缓存、跳过字节和错误数；读取统计不负责驱动或排空后台线程，应在设备关闭后读取。

`CACHE_DEVICE`在UNBIND后累计本次设备计数：`candidate_bytes`是填充候选读取量，`resident_bytes`是在DMA前按驻留提示跳过的量，`dma_bytes`是实际数据传输量；`queue/credit/overwrite/error/stop_dropped_bytes`分别是登记队列满、接收槽满、FDM复用前放弃、传输/对齐错误和停止时取消的字节。均为字节，重复读取按实际发生次数计量，不是唯一页面数。正常完成且错误为0时，候选量等于跳过、实际传输和各类丢弃之和。PG开启profile时也输出每条执行流的同组计数，不需开启设备逐事件日志。

分析瓶颈时给应用设置 `SNIA_FEMU_PROFILE=1`，模块加载时设置 `profile=1`（已加载时可写 `/sys/module/femu_cache/parameters/profile`）。设备增加 `CSD_IO_SPAN`、`CSD_CU_SPAN`、`CSD_GRAPH_TIME`，分别记录内部 I/O、CU 排队/执行、执行图从取到命令至返回完成队列的时间；软件 I/O 和 CU 同时记录线程 CPU 时间，以区分实际工作与被调度出去的时间。内部 I/O 还分别统计 FTL 模型处理、后端范围锁等待、NVM/FDM 软件复制和返回时的 CSD 锁等待。`CACHE_BLOCK` 包含 DMA 锁等待、更新状态锁等待、完成记录发布、SG 段数及复制线程 CPU 时间。

环内 `ready` 的更新只是一次状态发布，不代表整个缓存操作只有一次赋值。设备还要探测 Guest 接收槽、转换 DMA 地址、复制数据并等待模拟链路；Guest 还要获取文件缓存页并复制数据。锁等待、线程唤醒与日志输出也不能归为标志位写入本身。接收页分配及 DMA 映射在模块初始化时单独计量；查询中的缓冲借用、会话绑定等真实开销仍计入查询，不从结果中扣除。

SNIA 的 `PROFILE GRAPH_IO` 可通过 session/cookie 与设备执行图对应。两侧分别计算时长后相减，得到取 SQE 前与完成返回后的合计开销；Guest 与 FEMU 时钟不能直接相减。`PROFILE CACHE_RX` 记录接收缓冲初始化、注册、停止、线程排空，以及消费线程的 CPU 时间、发布 ioctl 和空闲睡眠。模块的 `CACHE_HOST` 把发布耗时拆为 DMA 同步、文件映射查找、缓存页获取/分配、复制和剩余处理。分项为累计工作时间，可能互相并行；只有同一条串行任务流内互斥的区间才可相加并计算总耗时占比。细分采样也有开销，性能验收应关闭这些开关。

真实设备 DMA 对应的 FEMU 软件复制，以及新增的 Guest 内存复制都会消耗 CPU 和内存带宽，必须实测是否拖慢计算。当前 Compose 将设备线程及内存固定在 node2，Guest 线程及 RAM 固定在 node3；具体参数见项目根 README。

### 缓存输入路径对比

同一 CMake 项目还提供 `test-cache-input`，用于比较同一份 1GiB 文件的内部 LOAD、已驻留文件页的只读 `mmap` 输入，以及 `pread` 到用户缓冲后再输入。它只测数据搬运，不执行 SQL 或计算算子，不开启HPCP；缓存预热、FDM 分配和末尾数据校验单独进行。

```sh
cmake --build /tmp/femu-cache-user --target test-cache-input -j2
sudo /tmp/femu-cache-user/test-cache-input /data/cache-input.bin 4096 4 3
```

参数为文件路径、数据块 KiB、并发流数、轮数；空文件自动写入 1GiB 测试数据，请使用独立测试文件。`mmap` 地址可直接传入 `csQueueCopyMemRequest`，不经过插件的用户数据中转缓冲。测试根据 Guest 的 NVMe `max_segments` 限制分段输入：本机 128 段、4KiB 页，对应最多 512KiB 一次；现有插件固定 1MiB 分段不能保证容纳分散文件页。该限制尚未改入生产适配层。

缓存输入仍占主机到设备的 PCIe 带宽，内部 LOAD 不占这条链路，因此缓存命中不保证更快。后续混合输入需要保留内部 LOAD 路径，并正确区分缓存命中和缺失，不能让缺页隐式读取文件后再上传。
