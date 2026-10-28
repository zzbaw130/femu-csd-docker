# FEMU 文件页缓存回填

本目录是运行在 **Guest 内核** 的页缓存接收模块和运行在 **Guest 用户态** 的小型 SNIA 验证程序。设备实现位于 `code/FEMU/hw/femu/csd/csd.c`，测试 Filter 算子位于 `code/csd-programs/examples/cache-filter.c`，在 FEMU CU 线程执行。没有用户态代理、临时辅助进程或轮询守护进程。

## 数据流与生命周期

1. 应用对只读、位置固定的文件执行 FIEMAP，将文件描述符、文件偏移及对应的 namespace 物理字节范围注册到 `/dev/femu-cache`。模块按打开的设备文件描述符管理注册并持有文件引用；CLEAR 只清理该调用者，关闭描述符（包括进程异常退出）时也会自动释放，不清理其他调用者的注册。
2. 设置 `SNIA_FEMU_CACHE_REFILL=1` 后，FEMU 插件给 EXEC_GRAPH 添加本项目的回填标志。它不改变公开 SNIA API，默认关闭。
3. 设备将图中的普通 LOAD 拆为最多 4 MiB 的子块。每块内部读取完成后，生成包含 token、LBA、FDM 范围的记录，发出 vendor AER；计算节点按原依赖继续执行，无需等待回填。
4. Guest 内核通过 NVMe AER tracepoint 收到通知，唤醒有序 workqueue 获取元数据，将各块投递到 I/O 工作队列，全局限制最多 4 个块同时准备页和执行 I/O。一次取最多 32 条元数据记录；元数据不包含文件数据。标准 vendor Get Log Page 为 C8；模块使用等价的 E8/FETCH I/O 命令读取，避免依赖 NVMe 驱动的私有控制器结构。
5. I/O worker 只在取得文件引用时持有映射锁，随后独立申请并锁定缺失的文件页，把这些页直接作为 F3 请求的 BIO。4 MiB 的页描述符在堆上分配；一个 NVMe 请求可链接多个 BIO。当前驱动每请求最多 128 个物理段；若目标缓存页过于分散，只有该范围会缩小后重试构建，不固定按 64 KiB 拆分。文件引用保留到 DMA 和页发布结束，调用者关闭注册不会使在途映射失效。NVMe 驱动映射目标页，FEMU 从原 FDM 范围直接 DMA 到目标页；不重读 NVM，也不经用户态缓冲复制。已有有效缓存页不覆盖，锁冲突、脏页等情况跳过。
6. 设备记录各段成功传输的字节数，整个块已传到主机且引用该范围的 CU 消费结束后，即释放回填占用，允许整条图完成；无需等待主机发布缓存页。Guest 的 Read 完成后标记 folio 有效并解锁，完整传输不再发送 ACK。若已有缓存、跳过页面或传输失败，Guest 仍在处理结束后异步发送 ACK，让设备释放未传输部分的占用；最多 8 个异步 ACK，达到上限时同步确认。旧模块在完整传输后发送的迟到 ACK 可以安全忽略。失联 5 秒后放弃该块回填，不永久占用 FDM；日志里不将失败标成成功。

**当前约定：开启回填的 LOAD 目标范围，在 batch 返回前必须保持只读，不得由另一个 LOAD、WRITE 或 CSF 输出覆盖。** 应用可以并行提交使用独立 FDM 的 batch。当前只覆盖普通图 LOAD，不覆盖 CSF 重入发起的内部读取。PG 扩展在读取已准备的表/索引 extent 时直接注册对应文件段，注册随查询内存上下文释放，ERROR 时也会关闭；不另起进程。并发文件写入和一致性失效尚未接入。CSF 内部请求的基表读取仍未回填，不能将部分显式 LOAD 的回填视为整条 SQL 全部读取已覆盖。

设备端最多保留 128 条回填记录。F3 回填 Read 与普通 Read 一样，由处理该 NVMe 提交队列的 poller 同步执行软件 DMA；复制前后短暂持有 CSD 锁校验 token、登记在途引用和更新计时，PRP/SGL 映射与数据复制期间不持有 CSD 锁。FTL 只处理图调度、介质时延及回填记录回收，不执行回填数据复制。在途 DMA 会阻止该记录因 ACK 或超时提前释放，保证输入 FDM 在复制结束前有效。

Guest 全局限制最多 4 个 I/O 任务同时执行，至多 4 条 DMA 请求在途、约 16 MiB 文件页锁定用于回填。不同 poller 可以并行处理；同一 poller 上仍会顺序处理请求。当前没有独立低优先级 SQ/WRR，复制仍消耗宿主 CPU 和内存带宽，也不能被同一 poller 的后续请求抢占。FEMU 此处使用 QEMU 的实际内存 DMA 路径，没有新增 PCIe 传输带宽模型，因此这些测量不能解释为真实硬件 PCIe 吞吐。

## Guest 编译与安装

需要当前运行内核对应的 headers、C 编译器、make、CMake，且已安装 SNIA。内核模块必须由 Linux Kbuild 编译：

```sh
mkdir -p /tmp/femu-cache-build
cp /home/femu/apps/femu-cache/femu_cache.c /home/femu/apps/femu-cache/csd-cache.h \
   /home/femu/apps/femu-cache/Makefile /tmp/femu-cache-build/
make -C /tmp/femu-cache-build -j2
sudo insmod /tmp/femu-cache-build/femu_cache.ko
sudo chown "$(id -un):$(id -gn)" /dev/femu-cache
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

程序生成 1 GiB 顺序整数文件，8 条独立流各自提交 LOAD→只读 Filter，每个扫描任务 4 MiB，共 256 个任务，与数据库默认扫描粒度一致。开启回填时，设备按最多 4 MiB 范围通知和回读；物理范围连续、所有页缺失且驱动散列表未超限时，每个任务只发送一次 Read FDM，共 256 次，extent 边界和跳过页面可能增加次数。Filter 的重复次数用于分别测量轻、重计算，执行真实过滤与求和，不插入 sleep。两种模式都先驱逐文件缓存，计算结束后检查页驻留数量、计算结果，并在计时区间外逐字节检查文件内容：

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

绘图依赖 matplotlib，输出逐块 CSV、统计 JSON 和 PNG/PDF。所有时间戳来自同一个 FEMU 单调时钟：`load` 是块可用，`cu` 是最后一个消费者的计算完成，`dma_start`/`dma` 是回填 DMA 开始/结束，`cache_ack` 是设备收到缓存有效确认，`release` 是解除回填占用，`dma_bytes` 是成功传输的字节数。完整传输时不再发送 ACK，`cache_ack=0` 不表示失败；缓存发布时刻由 Guest 的 `published` 记录确认。CU 时间不包含 FTL 处理完成事件的调度滞后。

必须区分 **LOAD→DMA 结束的总响应时间**和 **DMA 自身复制时间**：前者还包含 AER、Guest 调度和 NVMe 队列等待。缓存有效确认包含 ACK 命令的传输延迟，是缓存发布时刻的上界。`release-cu` 表示 CU 完成后的额外 FDM 占用，不能将其全部归因于 DMA 复制。

### 拆分回填等待

模块参数 `profile` 默认关闭。开启后，将每块的 worker 入口、FETCH 起止、页分配、BIO 准备、DMA 提交/返回、页发布及 ACK 提交/完成回调时间存入内存；测量过程中不逐块打印日志。最多记录 4096 个回填范围；1 GiB 测试通常产生 256 条记录，每轮测量前重新加载模块清空。测试程序的 `FEMU_CACHE_PROFILE=1` 则记录 8 条流中每轮 batch、结果读取和资源分配/释放的时间，在计时结束后打印。

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

Guest `ktime` 与 FEMU 时钟有偏移，不能直接相减。分析利用“设备 DMA（以及实际存在的 ACK）处理一定发生在对应 Guest 请求提交与返回之间”得到每轮时钟偏移区间；若所有请求的区间没有交集，直接报错，不输出混合时钟的归因。报告同时保留区间宽度和关键等待各项在下界、中点、上界对齐时的变化。按最晚结束的一条应用流统计关键路径，避免将 8 条流的重叠等待直接相加。同一 4 MiB batch 的多个回填子块按设备图归组，只取最后释放的子块作为该 batch 的释放依赖，不将重叠的子块占用重复相加。旧版串行记录可以统计单 worker 的服务构成；带 `parallel=1` 的记录中多个 worker 会重叠，分析工具只对当前块的服务进行关键等待归因，其余标为本块服务前等待，不重复计入其他 worker。并行模式图中的请求耗时之和不是整段时间，不能将其当作串行服务构成或新增总延迟的分摊。

单块发生驱动限制拆分时，`dma_calls` 只统计实际提交的 Read 数量；`dma_start` 到 `dma` 包含多次请求之间的间隙，不等于纯复制时间。分析中的 `single_read_mean_us` 仅用一条 Read 完成的范围拆分复制与完成通知延迟。测试程序在计算任务返回后，通过现有诊断读取等待缓存页发布结束，再校验页驻留和内容；这段 `cache_drain_ms` 在 `wall_ms` 之外，包含诊断读取本身，不能当作纯回填尾延迟。

### PG 冷缓存对照

回填标志必须放在 **PG 服务进程的环境** 中；仅给 psql 设置环境不会改变已启动的后端。以下复用冷启动脚本：

```sh
SNIA_FEMU_CACHE_REFILL=0 sh /home/femu/scripts/cold-query.sh csd /home/femu/tests/queries/6.sql
SNIA_FEMU_CACHE_REFILL=1 sh /home/femu/scripts/cold-query.sh csd /home/femu/tests/queries/6.sql
sudo cat /sys/kernel/debug/femu-cache/profile
```

每次都会重启 PG 并清理 Guest page cache。模块访问权限在加载时配置，postgres 二进制沿用实验 Guest 已授权的 CAP_SYS_ADMIN。`CACHE_STATS` 在计时关闭时也会输出块数、回填/缓存命中字节数、跳过、错误和设备累计丢记录数；它会先等待现有工作完成，不参与 SQL 执行。

`CACHE_BLOCK.cu` 是输入最后一个消费者的结束时刻；它可能早于后续算子链结束。设备图返回时的 `CACHE_GRAPH` 另外记录最后一个计算节点的 `compute`、固件确认所有节点完成的 `work`、最终返回的 `done`。`compute=0` 表示该图只有 I/O，没有同图计算可以覆盖回传；`done-work` 表示图完成后的回填等待。多个图会并行，不得将这些等待直接相加当作 SQL 延迟。`work-compute` 还可能包含固件完成事件处理滞后或其他图内 I/O，不能当作纯计算。
