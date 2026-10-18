# v1.1 接口覆盖与能力范围

参照 SNIA《Computational Storage API v1.1》（2025-05-31）第 6 章实现 Guest API。全部 33 个函数有声明和可链接入口，不表示所有选项已实现，也不是规范一致性认证声明。

## 当前支持

| 接口 | 范围 |
| --- | --- |
| `csOpenCSx`、`csCloseCSx` | 显式配置允许的 namespace/controller；资源引用保护 |
| `csQueryCSxList`、`csGetCSxFromPath` | 只接受配置中存在、且能打开的 NVMe namespace；路径解析 |
| `csCSFDownload`、`csConfig` | 固定 CSEEId/EEIId=1，显式程序索引 1..65535；native 文件名+入口；原生命令加载、卸载、激活 |
| `csQueryCSFList`、`csGetCSFId` | 仅发现本进程经本库加载/激活的 CSF；同一路径的多个 device handle 共享目录 |
| `csAllocMem`、`csFreeMem`、`csInitMem` | FDMId=1、4096 字节粒度；清零/32 位填充由 Guest F5 写入；不提供主机映射 |
| `csQueueCopyMemRequest` | Host↔AFDM；AFDM→AFDM 为经过 Guest 的软件 fallback，最多 1 MiB 缓冲，支持重叠 |
| `csQueueStorageRequest` | block LOAD、多 LBA range、直接 D0 进入 AFDM；不支持 STORE |
| `csQueueComputeRequest`、`csHelperSetComputeArg` | 最多 64 参数：AFDM+offset、8/16/32/64/128 位整数；CSF 入口采用可选解码器；每次计算额外使用 4 KiB AFDM 输入 |
| `csAllocBatchRequest`、`csFreeBatchRequest` | 最多 64 节点的主机软件 batch |
| `csConfigureBatchEntry`、`csHelperResizeBatchRequest` | ADD/DELETE/RECONFIG/JOIN/SPLIT；环检测、配置回滚、描述深拷贝、引用保护 |
| `csQueueBatchRequest` | SERIAL/PARALLEL/HYBRID；单 batch 一个 CSx；并发提交上限来自显式实验配置 |
| `csCreateEvent`、`csDeleteEvent`、`csPollEvent` | Guest 进程内完成队列，一个 event 可保存多个完成 |
| callback | Guest 工作线程调用应用函数；保留 Context、状态、CompValue |
| `csAbortRequest` | 仅支持尚未开始的异步请求；运行中的 native CSF 返回不支持 |
| `csQueryDeviceProperties` | CSx、固定 native CSEE、本进程已知 CSF、配置中的 FDM 容量；硬件 batch flag=0 |
| `csQueryLibrarySupport` | 文件系统支持列表为空，当前仅 block I/O |

原生 FEMU 未提供设备能力/全局程序目录/统计查询。`SINA_CS_FEMU_DEVICES` 是管理员确认的实验配置，不是设备探测结果；内存分配是否成功仍由 FEMU 的真实 FDM 限额决定。应用需要预留程序索引，本库无法防止其他进程使用同一索引。设备重启后重新启动应用并初始化。

## 有入口但返回不支持

- `csQueryDeviceProperties(..., CS_CSE_TYPE, ...)`：无法查询真实可用 CU，不填充猜测值。
- `csQueryDeviceStatistics`：不把本进程的局部记录伪装成设备全局统计。
- `csRegisterNotify`、`csDeregisterNotify`、`csCSEEDownload`、`csReset`、`csRegisterPlugin`、`csDeregisterPlugin`。
- uBPF/FPGA/container profile、文件 I/O、block STORE、跨设备复制/batch、descriptor 参数、FDM 主机映射、CR affinity、设备整图提交和运行中抢占。

旧 passthrough 和原有设备功能保持原样。部分功能有主机软件实现，不等于硬件具有对应能力；AFDM 内复制 fallback 尤其不能用于测量内部带宽。

## 生命周期及 ABI 约定

- opaque handle 不等于 NVMe ID 或进程指针。配置图和在途请求保留资源引用，仍被引用的 AFDM 不能释放。
- 异步请求深拷贝描述，不复制 HostVAddress 指向的数据；应用需保留数据至完成。
- Callback 与 EventHandle 不能同时指定。异步 ReqHandle 在完成后失效，无额外 free API。
- 共享 AFDM 的依赖由应用明确建边，库不推断 CSF 的读写语义。
- CSF 私有栈/heap 与 AFDM 分开；没有私有工作内存配额或 native 沙箱。
- 本 profile 将 completion 的 0..INT32_MAX 解释为成功，负 32 位值解释为 CSF 错误；结果数据应写 AFDM。

## 规范正文中的不一致

- 状态枚举缺少正文使用的 `CS_LENGTH_RETURNED`，提供 `CS_INVALID_LENGTH` 别名。
- `CSFIdinfo`/`CSFIdInfo` 和 `CS_COPY_AFDM`/`CS_COPY_DEV_MEM` 提供兼容别名。
- 参数枚举包含 128 位值但 union 缺少对应字段，本实现补充 `u128 Value128`；descriptor 尚不支持。
- 可变参数 helper 的 AFDM offset 应传 `unsigned long`，32/64/128 位值应传对应的 `u32/u64/u128`；8/16 位值遵循 C 整数提升。
