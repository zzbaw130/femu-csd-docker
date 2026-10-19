# 公共 API、设备插件与设备算子的分工

应用与 SNIA 库、设备适配插件都运行在 Guest 的应用进程中。设备算子 `.so` 在容器内的 FEMU CU 线程执行，逻辑上属于 CSD CPU。这两类动态库的用途、执行位置不同，没有常驻代理。

## 目录与职责

应用只包含 `include/cs.h`，其中定义全部公开类型和 33 个函数。`src/` 直接按 SNIA v1.1 分类组织，API 与所属实现放在同一文件：

| 文件 | API |
| --- | --- |
| cs_discovery.c | csQueryCSxList、csQueryCSFList、csGetCSxFromPath |
| cs_access.c | csOpenCSx、csCloseCSx、csRegisterNotify、csDeregisterNotify |
| cs_afdm_management.c | csAllocMem、csFreeMem、csInitMem、csQueueCopyMemRequest |
| cs_storage_ios.c | csQueueStorageRequest |
| cs_csf_scheduling.c | csGetCSFId、csAbortRequest、csQueueComputeRequest、csCSEEDownload、csHelperSetComputeArg |
| cs_batch_scheduling.c | csAllocBatchRequest、csFreeBatchRequest、csConfigureBatchEntry、csHelperResizeBatchRequest、csQueueBatchRequest |
| cs_event_management.c | csCreateEvent、csDeleteEvent、csPollEvent |
| cs_management.c | csQueryDeviceProperties、csQueryDeviceStatistics、csCSFDownload、csConfig、csReset |
| cs_library_management.c | csQueryLibrarySupport、csRegisterPlugin、csDeregisterPlugin |

除这九个分类文件，只单独保留两份跨 API 共享实现：

- `src/cs_handle_registry.c`：句柄创建、查找、类型检查、引用计数和退役，供设备、AFDM、CSF、event、batch 和异步请求共同使用。
- `src/cs_request_execution.c`：计算/复制/存储请求的描述快照、句柄绑定、资源引用，以及同步/异步执行和完成通知。batch 也使用这套请求执行机制。

batch 的依赖检查、调度和全部五个 API 直接在 `cs_batch_scheduling.c` 中实现；插件扫描、加载和全部三个库管理 API 直接在 `cs_library_management.c` 中实现。没有单独的 facade/core 目录，也没有为这两类 API 再留一组仅负责转发的同义函数。

`src/cs_internal.h` 只声明这些文件之间共享的对象和函数。公共设备对象保存插件引用、不透明私有对象、设备名称及并发数，不含 fd、namespace、NVMe 命令或 FEMU MR。插件不包含这个头。

## 后端方法如何对应标准 API

`src/cs_backend.h` 中的 `cs_backend_ops` 是 C 函数指针表，用于运行时多态；目前没有 C++ 虚函数类。每个设备插件提供自己的操作表，同一个 API 根据设备句柄调用对应插件。

标准设备操作使用原 API 去掉 cs 前缀的名称，首单词小写、CSF/CSEE 缩写保留。例如：

| 标准 API | 后端方法 | FEMU 实现 |
| --- | --- | --- |
| csQueryCSxList | queryCSxList | femu_queryCSxList |
| csOpenCSx | openCSx | femu_openCSx |
| csAllocMem | allocMem | femu_allocMem |
| csQueueCopyMemRequest | queueCopyMemRequest | femu_queueCopyMemRequest |
| csQueueStorageRequest | queueStorageRequest | femu_queueStorageRequest |
| csQueueComputeRequest | queueComputeRequest | femu_queueComputeRequest |
| csQueryDeviceProperties | queryDeviceProperties | femu_queryDeviceProperties |
| csCSFDownload | CSFDownload | femu_CSFDownload |

插件方法接收已绑定的私有设备/内存/CSF 标识。queue 方法在调用线程中完成一次操作；公开 API 的异步排队、callback/event 由共享请求执行器提供，因此不用每个设备重复实现。

还有四个内部钩子，**不对应额外的 SNIA API**，在头文件中与标准方法分组并逐项注释：

| 钩子 | 调用时机与用途 |
| --- | --- |
| getCSFResourceId | getCSFId 后，把私有 CSF 标识转换为设备资源编号，检查下载/配置是否影响仍在使用的函数 |
| validateRequest | 可选；配置 batch 或提交请求时，检查已绑定请求的设备特有限制，不执行请求 |
| executeBatchEntry | 可选；执行一个逻辑 batch 节点，FEMU 用于处理 spill 重入，直到节点真正完成 |
| finishBatchExecution | 可选；整批所有工作线程退出后清理本次资源，FEMU 用于回收失败批次新增的 spill 区段 |

其余能力由分类文件直接实现：event 没有绑定设备，batch 依赖由公共库调度，参数 helper 操作标准结构，库管理负责插件加载，设备枚举汇总已加载插件。

## 插件加载和一次计算的调用路径

1. 公共库扫描自身动态库旁的 `snia-cs-api/*.so`，调用每个模块的 `cs_backend_create` 获取操作表；也支持 API 显式注册路径。
2. csOpenCSx 依次让已加载插件识别设备，成功后创建公开设备句柄，绑定插件及其私有设备对象。
3. csAllocMem 创建公开 AFDM 句柄，内部保存插件返回的私有内存对象及容量。csGetCSFId 同样包装插件函数标识。
4. csQueueComputeRequest 进入公共请求层：复制描述，保留 CSF/AFDM 的引用，将请求副本中的句柄替换为对应插件私有标识。混用不同设备的资源在此被拒绝。
5. 可选的 validateRequest 检查设备限制，再调用所属插件的 queueComputeRequest 执行请求。同步调用直接返回；异步调用由共享工作线程执行并发布 callback/event。
6. FEMU 插件将私有 AFDM ID 转为 MR，标量依次填 cparam1/2，再提交 E1。其他插件可使用不同设备协议，不修改外观层。

插件使用自己的私有类型，不包含 cs_internal.h；公共层不包含 FEMU 头文件。此次后端操作表调整为 ABI 2，旧插件需重新编译。插件工厂导出的操作表有一个内部 ABI 版本，防止加载接口不兼容的动态库；它与算子参数、完成值无关。可选操作为空时公共 API 返回不支持。

公开设备、AFDM、CSF 和在途请求通过引用计数维持生命周期。发现时也临时引用插件。注销插件时有引用则返回 CS_HANDLE_IN_USE，不能卸载正在执行或仍被句柄使用的代码。重复注册同一路径不会重新初始化插件。插件 CSF 私有标识须有效到设备关闭；若内部需创建包装对象，由插件随设备管理。

## 通用 batch 与 FEMU 重入

batch 在 Guest 内存中保存最多64个节点、依赖和请求快照。SERIAL 单线程；PARALLEL 独立节点并行；HYBRID 等全部前驱完成。Before 为后继、After 为前驱；JOIN 添加 After→Before；配置阶段检查环。当前一个 batch 使用同一个公开设备句柄。

公共调度器执行准备就绪的节点。插件没有 executeBatchEntry 方法时，按请求类型调用 queueComputeRequest、queueCopyMemRequest 或 queueStorageRequest；有此方法时由插件完成一个逻辑节点，返回后才启动后继。所有工作线程退出后，调用可选 finishBatchExecution 清理本次执行的插件资源。

FEMU 的 executeBatchEntry 解释 NEED_STORE/LOAD/FREE：完成内部 I/O，把 spill_id 填入本次执行上下文的 cparam2，再次运行 CSF，直到获得普通结果。公共调度器不解释这三个返回值；其他设备可以合法返回相同数值而不触发 FEMU 协议。配置的请求不被修改，因此可重复提交。

## FEMU 私有内存、I/O 与临时区

AFDM 保存输入/输出；排序状态、hash表等使用设备 libcs-workmem.so，可跨入口保留。共享池是合作式配额，不是 native 沙箱。

插件通过 Identify 查询 namespace 格式，GET_INFO 查询 FDM/CU；CU 数用于公共调度器的并发上限。固定 CSEE 已分配全部 CR，NumAvailableCRs=0 不代表瞬时忙。

临时池按 GPT PARTLABEL=femu-spill 发现，以 namespace LBA 分配区段；数据通过内部 D0/D1 传输。同进程同路径的多个设备句柄共享池，分区节点的合作式 flock 防止其他本库进程重叠分配。成功 batch 保留 spill_id；失败 batch 等在途节点结束后回收本批新增区段；显式 FREE 或最后一个参与池的句柄关闭时清理。算法私有状态由算子自己的清理入口释放，没有崩溃恢复。

FEMU 程序目录仅记录进程内经本插件加载的函数；需避免跨进程程序索引冲突。AFDM→AFDM 软件回退仍经 Guest F2/F5；内部 LOAD/STORE 不经 Guest 中转。异步请求不复制 HostVAddress 数据，调用者须保留到完成。当前运行中的 CSF 不可抢占。
