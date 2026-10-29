# FEMU 主动 DMA 与文件缓存回填

本功能用于只读、extent 固定的实验数据。默认关闭，应用设置 `SNIA_FEMU_CACHE_REFILL=1` 后启用，不改变公开 SNIA API。文件注册仍由 PG 扩展或测试应用执行；普通 SNIA 应用若未注册文件，只接收并丢弃无法关联到文件的数据。

## 运行位置与流程

- **Guest 用户态**：`libsnia` 的 FEMU 插件管理接收缓冲和一个库内消费线程，不启动辅助进程或代理。
- **Guest 内核态**：本目录的 `femu_cache.ko` 固定 DMA 接收页、提供完成记录映射，并把收到的数据复制到干净的文件页缓存。
- **设备端**：FEMU `hw/femu/csd/csd.c` 的 LOAD 与 CSF 调度；两个设备级回填线程执行主动软件 DMA；正常 I/O 由两个 NVMe poller 处理。设备不理解 inode、PG 表或文件偏移。

1. 上层将文件、namespace LBA 和文件偏移注册到 `/dev/femu-cache`。文件保持只读，不改变 extent；开启回填时，LOAD 输入在图内也保持只读，算子使用独立输出缓冲，不原地修改这份基表输入。
2. 模块加载时预分配 **8 个 4MiB 接收槽位**（32MiB Guest 内存）和一页完成记录，完成固定页及 DMA 映射。这是设备环境初始化成本，应单独报告。SNIA 首次提交开启回填的设备图时借用这组缓冲，通过一次私有 NVMe 注册命令绑定当前会话；查询结束后归还，下次直接复用。模块保存本次会话的文件映射引用。并发会话需要独立缓冲，预分配组已借出时按需分配额外接收区；不会串行化 SQL 执行。
3. 设备图显式 LOAD 和 CSF 通过可重入 namespace read 请求的内部基表读取都接入回填，最多按4MiB分块；隐藏 spill 的换入不回填。计算按原依赖继续推进；两个回填线程从保留块中领取任务，将同一份输入 DMA 到空闲接收槽位。DMA 复用共享 PCIe 带宽模型，没有二次 NAND/FTL 读取延迟。数据复制不占用 FTL 或正常 I/O poller 线程，也不持有 CSD 全局锁。回填线程空闲时等待条件变量；仅在等待尚未到期的 PCIe 模拟完成时间时进行短等待。
4. 软件复制和模拟链路传输均完成后，设备写入来源 LBA、长度等元数据，最后发布槽位的 `ready`。**没有 AER、逐块日志读取、F3 Read 或 ACK 命令。** 没有空闲接收槽位时跳过回填，不等待页缓存处理。
5. SNIA 库内线程检查完成记录，调用模块消费该槽位。模块把接收数据复制到缺失的文件缓存页，已有有效页不覆盖，脏页、冲突等情况跳过；不产生文件写入或 NVM 写回。处理完成后更新共享 `ready`，归还槽位，不发送 NVMe 请求。
6. CU 和 DMA 都结束后，设备输入可以复用。原 FDM 输入同时供 CU 和 DMA 读取，保留到二者结束；不再建立设备页别名或重复映射，仍不等待主机填入文件缓存。接收槽位是 Guest 内存，不计入设备 FDM。
   数据复制及模拟链路传输结束后，即允许重叠区域被下一次读取覆盖，不再为完成记录的发布继续占用该输入。回填线程使用专用缓存元数据锁，完全不获取 CU/FTL 的执行调度锁；接收槽探测另用环锁，访问 Guest 内存时也不持有缓存元数据锁。FTL 只在状态改变后回收输入、推进等待节点，锁顺序固定为执行调度锁→缓存元数据锁。逐块计时日志在全局锁外输出，关闭细分计时时不输出 CACHE_BLOCK。
   CSF 重入时也必须保持内部读取的基表输入只读，使用独立输出和控制区域。下一次内部读取若要覆盖仍在 DMA 的重叠区域，设备只暂停该节点，DMA 完成后通过就绪队列唤醒；尚未开始 DMA 的可选回填可以直接放弃。细分计时的 `CACHE_REUSE_WAIT` 记录此等待，`CACHE_BLOCK` 的 `internal=1` 标识内部读取。
7. 关闭 SNIA 设备时，先停止并等待实际 DMA 写入退出，再排空消费线程，最后解除 DMA 映射。模块也在文件描述符异常关闭时执行停止，避免设备继续写入释放后的页。关闭阶段的排空耗时需单独考虑，不能宣称后台工作没有任何资源开销。

控制器暂停时会等待在途软件 DMA 退出；控制器复位会注销接收环，恢复后须重新注册，避免向旧 Guest 地址写入。设备关闭时唤醒并回收两个回填线程。8 个 I/O 队列按每 poller 4 个队列配置；正常 I/O 与回填仍共用同一个设备到主机 PCIe 带宽预算，并未增加链路容量或实现抢占优先级。

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

SNIA 按其 [README](../snia/README.md) 编译安装。应用仍只包含 `cs.h`、链接 `-lsnia`；文件映射注册接口由本目录头文件提供，PG 已使用此接口。

## 验证与计时

```sh
cmake -S /home/femu/apps/femu-cache -B /tmp/femu-cache-user
cmake --build /tmp/femu-cache-user -j2
sudo /tmp/femu-cache-user/test-refill /data/cache-refill.bin 1 0
sudo /tmp/femu-cache-user/test-refill /data/cache-refill.bin 1 1
sudo cat /sys/kernel/debug/femu-cache/profile
```

测试使用 1GiB 文件、4MiB 输入、8 条流，重复数 1 为轻计算，可改为 100 测重计算。两种模式都从冷文件缓存开始，校验计算及文件字节；回填是尽力而为，不要求全部驻留。`wall_ms` 为任务执行时间，`cache_drain_ms` 在两种模式下都记录 CSF 卸载、关闭设备及排空后台消费，二者分别报告。每次查询测试仍须使用冷启动脚本，TPC-H 只使用正式 SQL。

设备 `CACHE_BLOCK` 记录 LOAD 完成、CU 完成、DMA 开始、软件复制结束、链路结束、完成记录发布及输入释放时间；其中 `dma` 是发布完成记录后的设备时间。`CACHE_GRAPH` 记录计算图完成，不能把并行图的等待相加当作查询延迟。Guest `CACHE_STATS` 记录块数、实际填充、已有缓存、跳过字节和错误数；读取统计不负责驱动或排空后台线程，应在设备关闭后读取。

分析瓶颈时给应用设置 `SNIA_FEMU_PROFILE=1`，模块加载时设置 `profile=1`（已加载时可写 `/sys/module/femu_cache/parameters/profile`）。设备增加 `CSD_IO_SPAN`、`CSD_CU_SPAN`、`CSD_GRAPH_TIME`，分别记录内部 I/O、CU 排队/执行、执行图从取到命令至返回完成队列的时间；软件 I/O 和 CU 同时记录线程 CPU 时间，以区分实际工作与被调度出去的时间。内部 I/O 还分别统计 FTL 模型处理、后端范围锁等待、NVM/FDM 软件复制和返回时的 CSD 锁等待。`CACHE_BLOCK` 包含 DMA 锁等待、更新状态锁等待、完成记录发布、SG 段数及复制线程 CPU 时间。

环内 `ready` 的更新只是一次状态发布，不代表整个缓存操作只有一次赋值。设备还要探测 Guest 接收槽、转换 DMA 地址、复制数据并等待模拟链路；Guest 还要获取文件缓存页并复制数据。锁等待、线程唤醒与日志输出也不能归为标志位写入本身。接收页分配及 DMA 映射在模块初始化时单独计量；查询中的缓冲借用、会话绑定等真实开销仍计入查询，不从结果中扣除。

SNIA 的 `PROFILE GRAPH_IO` 可通过 session/cookie 与设备执行图对应。两侧分别计算时长后相减，得到取 SQE 前与完成返回后的合计开销；Guest 与 FEMU 时钟不能直接相减。`PROFILE CACHE_RX` 记录接收缓冲初始化、注册、停止、线程排空，以及消费线程的 CPU 时间、发布 ioctl 和空闲睡眠。模块的 `CACHE_HOST` 把发布耗时拆为 DMA 同步、文件映射查找、缓存页获取/分配、复制和剩余处理。分项为累计工作时间，可能互相并行；只有同一条串行任务流内互斥的区间才可相加并计算总耗时占比。细分采样也有开销，性能验收应关闭这些开关。

真实设备 DMA 对应的 FEMU 软件复制，以及新增的 Guest 内存复制都会消耗 CPU 和内存带宽，必须实测是否拖慢计算。当前 Compose 将设备线程及内存固定在 node2，Guest 线程及 RAM 固定在 node3；具体参数见项目根 README。

### 缓存输入路径对比

同一 CMake 项目还提供 `test-cache-input`，用于比较同一份 1GiB 文件的内部 LOAD、已驻留文件页的只读 `mmap` 输入，以及 `pread` 到用户缓冲后再输入。它只测数据搬运，不执行 SQL 或计算算子，不开启回填；缓存预热、FDM 分配和末尾数据校验单独进行。

```sh
cmake --build /tmp/femu-cache-user --target test-cache-input -j2
sudo /tmp/femu-cache-user/test-cache-input /data/cache-input.bin 4096 4 3
```

参数为文件路径、数据块 KiB、并发流数、轮数；空文件自动写入 1GiB 测试数据，请使用独立测试文件。`mmap` 地址可直接传入 `csQueueCopyMemRequest`，不经过插件的用户数据中转缓冲。测试根据 Guest 的 NVMe `max_segments` 限制分段输入：本机 128 段、4KiB 页，对应最多 512KiB 一次；现有插件固定 1MiB 分段不能保证容纳分散文件页。该限制尚未改入生产适配层。

缓存输入仍占主机到设备的 PCIe 带宽，内部 LOAD 不占这条链路，因此缓存命中不保证更快。后续混合输入需要保留内部 LOAD 路径，并正确区分缓存命中和缺失，不能让缺页隐式读取文件后再上传。
