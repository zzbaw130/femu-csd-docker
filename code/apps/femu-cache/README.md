# FEMU 文件页缓存回填

本目录是运行在 **Guest 内核** 的页缓存接收模块和运行在 **Guest 用户态** 的小型 SNIA 验证程序。设备实现位于 `code/FEMU/hw/femu/csd/csd.c`，测试 Filter 算子位于 `code/csd-programs/examples/cache-filter.c`，在 FEMU CU 线程执行。没有用户态代理、临时辅助进程或轮询守护进程。

## 数据流与生命周期

1. 应用对只读、位置固定的文件执行 FIEMAP，将文件描述符、文件偏移及对应的 namespace 物理字节范围注册到 `/dev/femu-cache`。模块按打开的设备文件描述符管理注册并持有文件引用；CLEAR 只清理该调用者，关闭描述符（包括进程异常退出）时也会自动释放，不清理其他调用者的注册。
2. 设置 `SNIA_FEMU_CACHE_REFILL=1` 后，FEMU 插件给 EXEC_GRAPH 添加本项目的回填标志。它不改变公开 SNIA API，默认关闭。
3. 设备将图中的普通 LOAD 拆为最多 64 KiB 的子块。每块内部读取完成后，生成包含 token、LBA、FDM 范围的记录，发出 vendor AER；计算节点按原依赖继续执行，无需等待回填。
4. Guest 内核通过 NVMe AER tracepoint 收到通知，唤醒有序 workqueue 获取元数据，将各块投递到 I/O 工作队列，全局限制最多 4 个块同时准备页和执行 I/O。一次取最多 32 条元数据记录；元数据不包含文件数据。标准 vendor Get Log Page 为 C8；模块使用等价的 E8/FETCH I/O 命令读取，避免依赖 NVMe 驱动的私有控制器结构。
5. I/O worker 只在取得文件引用时持有映射锁，随后独立申请并锁定缺失的文件页，把这些页直接作为 F3 请求的 BIO。文件引用保留到 DMA 和页发布结束，调用者关闭注册不会使在途映射失效。NVMe 驱动映射目标页，FEMU 从原 FDM 范围直接 DMA 到目标页；不重读 NVM，也不经用户态缓冲复制。已有有效缓存页不覆盖，锁冲突、脏页等情况跳过。
6. DMA 完成后标记 folio 有效并解锁，异步发送 ACK；不再等待 ACK 完成才处理下一块。ACK 请求持有命令和模块引用，完成回调记录状态并释放资源；最多 8 个异步 ACK，达到上限时使用同步确认。设备同时等到引用该范围的 CU 消费结束和回填完成，才释放回填占用，允许整条图完成。失联 5 秒后放弃该块回填，不永久占用 FDM；日志里不将失败标成成功。

**当前约定：开启回填的 LOAD 目标范围，在 batch 返回前必须保持只读，不得由另一个 LOAD、WRITE 或 CSF 输出覆盖。** 应用可以并行提交使用独立 FDM 的 batch。当前只覆盖普通图 LOAD，不覆盖 CSF 重入发起的内部读取。并发文件写入、一致性失效和 PG 文件注册尚未接入；本阶段针对冻结只读文件验证设备与内核闭环。

设备端最多保留 128 条回填记录，后台队列每次最多处理一个 64 KiB DMA；FTL 前台请求队列排空后才服务后台 DMA。Guest 将元数据获取和 I/O 处理分开，全局限制最多 4 个 I/O 任务同时执行，至多 4 条 DMA 请求在途、约 256 KiB 文件页锁定用于回填，统计与每块有效字节数分别管理。这是软件后台优先级，**不是硬件独立低优先级 SQ/WRR，也不能抢占已经开始的 DMA**。FEMU 此处使用 QEMU 的实际内存 DMA 路径，没有新增 PCIe 传输带宽模型，因此这些测量不能解释为真实硬件 PCIe 吞吐。

## Guest 编译与安装

需要当前运行内核对应的 headers、C 编译器、make、CMake，且已安装 SNIA。内核模块必须由 Linux Kbuild 编译：

```sh
mkdir -p /tmp/femu-cache-build
cp /home/femu/apps/femu-cache/femu_cache.c /home/femu/apps/femu-cache/csd-cache.h \
   /home/femu/apps/femu-cache/Makefile /tmp/femu-cache-build/
make -C /tmp/femu-cache-build -j2
sudo insmod /tmp/femu-cache-build/femu_cache.ko
cmake -S /home/femu/apps/femu-cache -B /tmp/femu-cache-user
cmake --build /tmp/femu-cache-user -j2
```

模块目前针对 Linux 6.8、4 KiB 页和 `/dev/nvme0n1`。注册接口限 CAP_SYS_ADMIN；这里的权限不通过辅助进程转发。

### Linux 6.8 的 AER 启动修正

该版本驱动在 OAES 没有可选事件位时不提交 AER；FEMU CSD 的 vendor 事件也需要常驻 AER。`kernel/linux-6.8-aer.patch` 只修正这一启动条件，不虚报 OAES。必须用与运行内核**版本完全对应**的 Ubuntu 源码重新编译 nvme-core，不能用其他版本替换：

```sh
sudo apt-get install linux-source-6.8.0 linux-headers-$(uname -r)
mkdir -p /tmp/femu-nvme-kernel
tar -xjf /usr/src/linux-source-6.8.0.tar.bz2 -C /tmp/femu-nvme-kernel \
    --strip-components=1 linux-source-6.8.0/drivers/nvme/host
cd /tmp/femu-nvme-kernel/drivers/nvme/host
patch -p1 < /home/femu/apps/femu-cache/kernel/linux-6.8-aer.patch
make -C /lib/modules/$(uname -r)/build M="$PWD" -j4 nvme-core.ko
sudo mkdir -p /lib/modules/$(uname -r)/updates
sudo cp nvme-core.ko /lib/modules/$(uname -r)/updates/
sudo depmod -a
sudo update-initramfs -u
```

安装后需重新加载 NVMe 驱动或重启 Guest。重新加载前先停止 PG、卸载本模块和 `/data` 文件系统；先卸载模块才能释放文件注册。FEMU 的 RAM 后端在容器重启时丢失，必须先备份 namespace、重启后恢复，不能直接重启丢弃实验数据。

## 针对性测量与绘图

程序生成 16 MiB 顺序整数文件，8 条独立流各自提交 LOAD→只读 Filter，每块 64 KiB。Filter 的重复次数用于分别测量轻、重计算，执行真实过滤与求和，不插入 sleep。两种模式都先驱逐文件缓存，计算结束后检查页驻留数量、计算结果，并在计时区间外逐字节检查文件内容：

```sh
sudo /tmp/femu-cache-user/test-refill /data/cache-refill.bin 1 0
sudo /tmp/femu-cache-user/test-refill /data/cache-refill.bin 1 1
sudo /tmp/femu-cache-user/test-refill /data/cache-refill.bin 1000 0
sudo /tmp/femu-cache-user/test-refill /data/cache-refill.bin 1000 1
```

保存 FEMU 日志中的本次 `CACHE_BLOCK` 行后，在宿主执行：

```sh
python3 scripts/plot-cache-refill.py device.log results/cache-refill-plot
```

绘图依赖 matplotlib，输出逐块 CSV、统计 JSON 和 PNG/PDF。所有时间戳来自同一个 FEMU 单调时钟：`load` 是块可用，`cu` 是最后一个消费者的计算完成，`dma_start`/`dma` 是回填 DMA 开始/结束，`cache_ack` 是设备收到缓存有效确认，`release` 是解除回填占用。CU 时间不包含 FTL 处理完成事件的调度滞后。

必须区分 **LOAD→DMA 结束的总响应时间**和 **DMA 自身复制时间**：前者还包含 AER、Guest 调度和后台排队。缓存有效确认包含 ACK 命令的传输延迟，是缓存发布时刻的上界。`max(cache_ack-cu, 0)` 表示 CU 完成后等待回填确认的额外占用，不能将其全部归因于 DMA 复制。

### 拆分回填等待

模块参数 `profile` 默认关闭。开启后，将每块的 worker 入口、FETCH 起止、页分配、BIO 准备、DMA 提交/返回、页发布及 ACK 提交/完成回调时间存入内存；测量过程中不逐块打印日志。最多记录 4096 块，重新加载模块清空记录。测试程序的 `FEMU_CACHE_PROFILE=1` 则记录 8 条流中每轮 batch、结果读取和资源分配/释放的时间，在计时结束后打印。

```sh
sudo sh -c 'echo 1 > /sys/module/femu_cache/parameters/profile'
sudo env FEMU_CACHE_PROFILE=1 /tmp/femu-cache-user/test-refill \
    /data/cache-refill.bin 1 1 > measured.app
sudo cat /sys/kernel/debug/femu-cache/profile > measured.kernel
sudo sh -c 'echo 0 > /sys/module/femu_cache/parameters/profile'
```

应在没有正在执行的回填测试时开关计时，并同时保存此次 FEMU 的 `CACHE_BLOCK` 记录。kernel 记录的 token 与设备记录对应，LBA 与应用记录对应；多个块可以合用一次 AER、一次 FETCH，不能按块重复累加通知或 FETCH 时间。

`scripts/analyze-cache-refill.py` 处理 `roundN-pP-cacheC.app/.device/.kernel` 命名的一组实验记录，P 表示是否开启计时，C 表示是否回填。每轮各测 P=0/1、C=0/1，既拆分耗时，也比较计时开关自身的影响。输出 `breakdown.json`、`critical-waits.csv`，附加 `--plot` 生成图：

```sh
python3 scripts/analyze-cache-refill.py results/cache-refill-breakdown --plot
```

Guest `ktime` 与 FEMU 时钟有偏移，不能直接相减。分析利用“设备 DMA 和 ACK 处理一定发生在对应 Guest 请求提交与返回之间”得到每轮时钟偏移区间；若所有请求的区间没有交集，直接报错，不输出混合时钟的归因。报告同时保留区间宽度和关键等待各项在下界、中点、上界对齐时的变化。按最晚结束的一条应用流统计关键路径，避免将 8 条流的重叠等待直接相加。旧版串行记录可以统计单 worker 的服务构成；带 `parallel=1` 的记录中多个 worker 会重叠，分析工具只对当前块的服务进行关键等待归因，其余标为本块服务前等待，不重复计入其他 worker。并行模式图中的请求耗时之和不是整段时间，不能将其当作串行服务构成或新增总延迟的分摊。
