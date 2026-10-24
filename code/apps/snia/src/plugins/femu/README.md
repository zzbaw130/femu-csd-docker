# FEMU 设备适配插件

本目录编译为 libcs-femu.so，由 Guest 应用进程中的 SNIA 公共库加载；它将 API 请求映射成 NVMe 命令。设备算子位于 csd-programs，执行在 FEMU CU 线程。

| 文件 | 具体职责 |
| --- | --- |
| femu_discovery.c | queryCSxList、getCSxFromPath、queryCSFList |
| femu_access.c | openCSx、closeCSx |
| femu_afdm_management.c | allocMem、freeMem、initMem、queueCopyMemRequest |
| femu_storage_ios.c | queueStorageRequest，内部 D0 LOAD / D1 STORE |
| femu_csf_scheduling.c | getCSFId、queueComputeRequest，原生 MR 和两个标量的映射 |
| femu_batch_scheduling.c | 编码 EXEC_GRAPH、设备会话、取消及失败资源回收 |
| femu_management.c | CSFDownload、config、queryDeviceProperties |
| internal/femu_plugin_entry.c | C 工厂入口和设备方法表 |
| internal/femu_internal.h | 插件内部对象和共享函数声明 |
| internal/femu_csf_catalog.c | 记录本进程加载的设备函数，供发现、下载和验证共用 |
| internal/femu_device_resources.c | 读取 GET_INFO 的 FDM 容量、CU 数和功能位 |
| internal/femu_nvme_transport.c | Linux fd/ioctl、Identify、传输计时 |
| internal/femu_request_validation.c | 检查该设备的 MR、标量、namespace 与容量限制 |
| internal/femu_request_execution.c | 分派单次计算、复制和存储请求 |
| include/femu-csd-kernel.h | 设备算子的原生参数定义 |
| include/cs_csf.h | 设备算子的重入判断及 NEED_* 完成值辅助函数 |

根目录的七个实现文件按 SNIA 功能分类；internal/ 保存工厂入口、传输和其他内部设备处理，include/ 保存设备算子头文件。文件名不表示额外 SNIA API。此设备不实现的标准能力，由公共层返回不支持。

仅含 LOAD/STORE/compute 的 batch 经 queueBatchRequest 编码为一条 E9，设备读取依赖图并自主推进。设备在内部解释 NEED_STORE/LOAD/FREE，使用隐藏临时空间，I/O 完成后重入同一 CSF；Guest 不逐节点处理完成值。含 Host↔AFDM 等复制的 batch 在任何节点执行前选择公共软件调度，其中每个 compute 仍用单节点设备图处理内部重入。finishBatchExecution 只负责混合批次失败时回收此前节点新建的 token。

include/csd-graph.h 是设备私有线格式，与 FEMU 同名头保持一致，由针对性测试检查；它不安装为应用公开头文件。新插件要求支持 E9/EA 的新固件，公共插件操作表使用 ABI 3，须配套重新编译。

## 性能诊断

应用以SNIA_FEMU_PROFILE=1运行时，internal/femu_nvme_transport.c在设备关闭时向stderr打印E1、D0、D1、TRIM、F2、EXEC_GRAPH的请求次数及累计ioctl等待秒数。默认关闭；计数用原子操作，不改变并行调度或公开API。统计覆盖句柄生命周期，不是按batch自动清零。E1包含设备CSF执行时间，不能再与算子内部计时相加；D0包括原始数据和spill的LOAD。HYBRID并发时累计等待可重叠，不能直接相加视为总延迟。此功能只用于定位当前请求开销。

同一诊断开关还输出 `LOAD_to_E1`：同一 SNIA 线程最近一次成功 D0 返回到下一次 E1 ioctl 提交之间的累计间隔及次数。多范围 LOAD 取最后一次 D0 返回；该间隔使用 Guest 同一时钟，不包含 E1 提交后在设备队列中的等待，也不表示实际 CU 已开始计算。

EXEC_GRAPH 计数是整图 ioctl 次数与等待时间；图内的 LOAD、CSF、spill 不计入 Guest 的 D0/E1/D1 次数。LOAD_to_E1 仅诊断分开提交的旧路径，不能据此判断图内 I/O 与计算的间隔。图整体时间包含内部 I/O 和计算，不应与算子计时相加。
