# v1.1 接口覆盖与能力范围

参照 SNIA《Computational Storage API v1.1》（2025-05-31）第 6 章实现 Guest API。全部 33 个函数有声明和可链接入口，不表示所有选项已实现，也不是规范一致性认证声明。

## 公共层与插件

应用只包含 cs.h 并链接动态库。公共层负责句柄、异步、event、batch；设备操作由 handle 所属插件实现，设备枚举汇总已加载插件。csRegisterPlugin/csDeregisterPlugin 支持本项目 C 工厂契约，模块引用存在时不卸载。默认安装的真实适配器目前只有 FEMU。

## 当前 FEMU 插件支持

| 接口 | 范围 |
| --- | --- |
| `csOpenCSx`、`csCloseCSx` | Identify 校验 FEMU CSD；GET_INFO 查询真实 FDM/CU；资源引用保护 |
| `csQueryCSxList`、`csGetCSxFromPath` | 只接受通过 Identify 和资源查询的 FEMU NVMe namespace；路径解析 |
| `csCSFDownload`、`csConfig` | 固定 CSEEId/EEIId=1，显式程序索引 1..65535；native 文件名+入口；原生命令加载、卸载、激活 |
| `csQueryCSFList`、`csGetCSFId` | 仅发现本进程经本库加载/激活的 CSF；同一路径的多个 device handle 共享目录 |
| `csAllocMem`、`csFreeMem`、`csInitMem` | FDMId=1、4096 字节粒度；清零/32 位填充由 Guest F5 写入；不提供主机映射 |
| `csQueueCopyMemRequest` | Host↔AFDM；AFDM→AFDM 为经过 Guest 的软件 fallback，最多 1 MiB 缓冲，支持重叠 |
| `csQueueStorageRequest` | block LOAD/STORE、多 LBA range、内部 D0/D1，无 Guest 数据中转 |
| `csQueueComputeRequest`、`csHelperSetComputeArg` | AFDM 直接映射 MR；最多两个 64 位标量位置；两个 cparam 独立；计算 AFDM offset 仅支持 0；无额外参数 AFDM |
| `csAllocBatchRequest`、`csFreeBatchRequest` | 最多 64 节点的主机软件 batch |
| `csConfigureBatchEntry`、`csHelperResizeBatchRequest` | ADD/DELETE/RECONFIG/JOIN/SPLIT；环检测、配置回滚、描述深拷贝、引用保护 |
| `csQueueBatchRequest` | SERIAL/PARALLEL/HYBRID；单 batch 一个 CSx；设备 CU 决定工作线程数；可选恢复协议自动 STORE/LOAD/FREE |
| `csCreateEvent`、`csDeleteEvent`、`csPollEvent` | Guest 进程内完成队列，一个 event 可保存多个完成 |
| callback | Guest 工作线程调用应用函数；保留 Context、状态、CompValue |
| `csAbortRequest` | 仅支持尚未开始的异步请求；运行中的 native CSF 返回不支持 |
| `csQueryDeviceProperties` | CSx、单 CSE 的 CU/CR 列表、固定 native CSEE、本进程已知 CSF、设备 FDM 容量；硬件 batch flag=0 |
| `csQueryLibrarySupport` | 文件系统支持列表为空，当前仅 block I/O |

设备通过通用 GET_INFO 提供 FDM/CU/功能位；没有全局程序目录或设备统计查询。CU 映射为一个 CSE 的 CR，固定 CSEE 已分配全部 CR，NumAvailableCRs=0 不代表瞬时空闲线程数。无需手工配置 FDM/CU。应用仍需预留程序索引，设备重启后重新初始化。

## FEMU 有入口但返回不支持

- `csQueryDeviceStatistics`：不把本进程的局部记录伪装成设备全局统计。
- `csRegisterNotify`、`csDeregisterNotify`、`csCSEEDownload`、`csReset`。
- uBPF/FPGA/container profile、文件 I/O、跨设备复制/batch、128 位整数参数、descriptor 参数、FDM 主机映射、CR affinity、设备整图提交和运行中抢占。

设备增加通用内部 STORE、资源查询与 64 位完成值；原生算子参数结构不变。部分功能有主机软件实现，不等于硬件具有对应能力；AFDM 内复制 fallback 尤其不能用于测量内部带宽。

## 生命周期及 ABI 约定

- opaque handle 不等于 NVMe ID 或进程指针。配置图和在途请求保留资源引用，仍被引用的 AFDM 不能释放。
- 异步请求深拷贝描述，不复制 HostVAddress 指向的数据；应用需保留数据至完成。
- Callback 与 EventHandle 不能同时指定。异步 ReqHandle 在完成后失效，无额外 free API。
- 共享 AFDM 的依赖由应用明确建边，库不推断 CSF 的读写语义。
- CSF 私有栈/heap 与 AFDM 分开；通过设备 libcs-workmem.so 合作式管理私有工作内存；没有 native 沙箱。
- 本 profile 保留 64 位 completion，负 int64_t 值解释为 CSF 错误；batch 的高四位1/2/3范围保留给STORE/LOAD/FREE请求，其余非负值正常完成。

## 规范正文中的不一致

- 状态枚举缺少正文使用的 `CS_LENGTH_RETURNED`，提供 `CS_INVALID_LENGTH` 别名。
- `CSFIdinfo`/`CSFIdInfo` 和 `CS_COPY_AFDM`/`CS_COPY_DEV_MEM` 提供兼容别名。
- 保留标准的 128 位参数类型枚举，当前设备适配不实现该类型，也不添加对应 union 成员。
- 可变参数 helper 的 AFDM offset 应传 `unsigned long`，32/64 位值应传对应的 `u32/u64`；8/16 位值遵循 C 整数提升。

## 可选扩展

FEMU 专用 `src/plugins/femu/include/cs_csf.h` 提供重入判断和 STORE/LOAD/FREE 请求函数，不扩展标准 CS_STATUS。重入算子保留 cparam2，首次为0，I/O完成后由库填入 spill_id；最后一个AFDM是I/O缓冲。batch 自动推进，独立计算请求仍是一轮执行。临时区按 GPT PARTLABEL=femu-spill 识别；按 namespace LBA 分配，不使用隐藏 OP 或普通文件系统空闲块。成功批次的 token 可跨 batch 保留，失败批次回收本批新增区段。协议不是 SNIA v1.1 规定的算子 ABI，需双方采用，详见 README。
