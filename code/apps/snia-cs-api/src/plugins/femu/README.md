# FEMU 设备适配插件

本目录编译为 libcs-femu.so，由 Guest 应用进程中的 SNIA 公共库加载；它将 API 请求映射成 NVMe 命令。设备算子位于 csd-programs，执行在 FEMU CU 线程。

| 文件 | 具体职责 |
| --- | --- |
| femu_discovery.c | queryCSxList、getCSxFromPath、queryCSFList |
| femu_access.c | openCSx、closeCSx |
| femu_afdm_management.c | allocMem、freeMem、initMem、queueCopyMemRequest |
| femu_storage_ios.c | queueStorageRequest，内部 D0 LOAD / D1 STORE |
| femu_csf_scheduling.c | getCSFId、queueComputeRequest，原生 MR 和两个标量的映射 |
| femu_batch_scheduling.c | 解释 NEED_STORE/LOAD/FREE、维护临时区、完成 I/O 后重入 |
| femu_management.c | CSFDownload、config、queryDeviceProperties |
| femu_plugin_entry.c | C 工厂入口和设备方法表 |
| femu_common.h | 插件内部对象和共享函数声明 |
| femu_csf_catalog.c | 记录本进程加载的设备函数，供发现、下载和验证共用 |
| femu_device_resources.c | 读取 GET_INFO 的 FDM 容量、CU 数和功能位 |
| femu_nvme_transport.c | Linux fd/ioctl、Identify、spill 分区发现 |
| femu_request_validation.c | 检查该设备的 MR、标量、namespace 与容量限制 |
| femu_request_execution.c | 统一分派内部请求；重入时为计算入口设置完成 I/O 的标识 |
| include/femu-csd-kernel.h | 设备算子的原生参数定义 |
| include/cs_csf.h | 设备算子的重入判断及 NEED_* 完成值辅助函数 |

前七个实现文件按 SNIA 功能分类，其余是本适配器复用的设备处理；文件名不表示额外 SNIA API。此设备不实现的标准能力，由公共层返回不支持。

设备只执行单次 NVMe 请求；batch 图、依赖、异步通知由公共库处理。executeBatchEntry 在一个逻辑节点内循环执行 CSF 和内部 I/O，直到获得普通完成值，再交回公共调度器启动后继。finishBatchExecution 在所有线程结束后回收失败批次新分配的临时区。

## 性能诊断

应用以SNIA_FEMU_PROFILE=1运行时，femu_nvme_transport.c在设备关闭时向stderr打印E1、D0、D1、TRIM、F2的请求次数及累计ioctl等待秒数。默认关闭；计数用原子操作，不改变并行调度或公开API。统计覆盖句柄生命周期，不是按batch自动清零。E1包含设备CSF执行时间，不能再与算子内部计时相加；D0包括原始数据和spill的LOAD。HYBRID并发时累计等待可重叠，不能直接相加视为总延迟。此功能只用于定位当前请求开销。
