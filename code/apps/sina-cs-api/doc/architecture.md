# 架构、执行位置和内存

## 调用路径

```text
Guest 应用（cs.h / cs.sh）
  → Guest API 库：参数检查、句柄、资源引用、软件 batch、callback/event
  → Linux 原有 NVMe passthrough 驱动
  → 原生 FEMU CSD：B0/C0/D0/E1/F2/F5，原有加载/激活命令
  → FEMU CU 线程：调用原有 femu_csd_args ABI 的 native .so
  → NVMe 完成 → Guest 库推进依赖图，返回或发布完成通知
```

API 库位于 Guest 应用进程及其工作线程，消耗 Guest vCPU；没有常驻代理。FEMU 在容器内运行，`.so` 加载进 FEMU 进程，由其 CU 线程执行，逻辑上模拟 CSD CPU。

`code/FEMU/hw/femu/csd/` 保持初始 tag 的实现；没有 SNIA 专用查询命令、设备 DAG、类型化执行命令或追加的 `femu_csd_args` 字段。SNIA 应用接口通过 Guest 库映射到已有命令。

## 软件 batch 与完成通知

图及依赖状态保存在 Guest 私有内存。每个节点仍通过独立 NVMe 命令执行；前驱完成后，Guest 工作线程提交后继。SERIAL 单线程，PARALLEL 执行独立节点，HYBRID 等待每个节点的全部前驱。

- 最多 64 节点；ADD/DELETE/RECONFIG/JOIN/SPLIT，拒绝环并回滚配置。
- Before 表示后继，After 表示前驱；JOIN 加 After→Before，SPLIT 的两个索引切断 Before→After。
- 配置复制请求描述并保留资源引用，不复制 HostVAddress 指向的数据。
- 并发提交上限取实验配置中的 CU 数，并限制为最多 64 个工作线程。实际并行受原生 FEMU 限制：同一程序 ID 串行，不同实例可占用不同 CU。
- 失败后停止启动新节点，等待已运行节点退出后报告整批失败。
- 输入输出通过 AFDM 参数明确绑定，依赖图不自动推导 CSF 的数据格式或读写冲突。
- 连续 CSF 使用同一块 AFDM 时，中间结果留在设备中；Guest 只推进控制依赖。

硬件 batch 标志为 0。callback 由 Guest 工作线程在完成后调用；event 记录也在 Guest 内存，由应用轮询。FEMU 只返回原有命令的完成状态，不接收 callback 地址。

## 实验配置与主机目录

原生 FEMU 没有本库所需的设备能力/程序枚举查询，库不补造设备命令。`SINA_CS_FEMU_DEVICES` 显式指定允许使用的 namespace、FDM 容量和 CU 数。容量属性来自配置；内存是否分配成功仍由设备决定。CSE 实时可用 CU 属性、设备全局统计返回 `CS_UNSUPPORTED`。

程序名称、GlobalId 和激活状态由 Guest 进程内目录记录，同进程打开同一路径的多个 handle 共享目录；只包含该进程通过库加载的程序。不能查询其他进程/旧 passthrough 已加载的程序，也不持久化到 FEMU。应用需预留程序索引，避免与其他客户端重复使用；当前实验使用单一管理进程。设备重启后应结束原应用进程并重新初始化。

## AFDM 与 CSF 私有运行内存

AFDM 保存 CSF 输入、输出和共享中间数据。为适配完整类型化参数，Guest 库为每次计算分配一个 4 KiB AFDM 输入缓冲，写入参数描述，并作为原生 E1 的最后一个 MR。`.so` 可使用 `cs_kernel_decode` 在自己的栈上解析，FEMU 不理解该内容。完成后库释放输入缓冲，包括执行失败的路径。因此纯标量 CSF 也可执行，且每个在途计算需要额外 4 KiB FDM。

该输入缓冲不保存队列、线程、主机指针或 CSF 私有工作区。CSF 的解码数组和临时变量位于 CU 栈/私有内存；64 MiB FDM 不等同于设备总 RAM。目前没有 CSF 私有 heap 配额或 native 指令级沙箱。

## 内存操作的软件适配

- 清零/填充：Guest 生成最多 1 MiB 的模式缓冲，通过原生 F5 分块写入。
- AFDM→AFDM：Guest 使用最多 1 MiB 临时缓冲，通过 F2/F5 分块复制；重叠范围按 memmove 方向处理。该路径有数据往返，不能视作设备内部带宽。
- block LOAD：原生 D0 直接从 namespace 进入 AFDM，不经过 Guest 数据缓冲。

## 资源生命周期

句柄为登记表中的 opaque token。配置图和执行请求持有 AFDM/CSF/device 引用，仍被引用的资源不能释放。完成时先释放请求引用，再通知应用；callback 不在登记表锁内执行。已开始的 native CSF 不能抢占，abort 返回不支持。

关闭设备只会失效空闲 CSF handle；有内存、配置图或请求时拒绝关闭。主机目录和设备状态之间不具备跨进程事务性；真实命令失败仍会报告给应用。
