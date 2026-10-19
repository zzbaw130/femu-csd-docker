# 架构、执行位置和资源

```text
Guest 应用 → Guest SNIA 库（图、线程、event、临时 token）
          → Linux NVMe passthrough → 容器内 FEMU
          → CU 线程调用 native .so → CQE → Guest 库推进逻辑节点
```

无常驻代理。库使用 Guest vCPU，设备 `.so` 在容器内 FEMU CU 线程执行，逻辑上属于 CSD CPU。设备只理解通用命令和 `femu_csd_args`，不解析 SQL 或恢复协议。

## 能力与内存

Identify Controller 验证 CSD Model Number；Identify Namespace 获取实际 LBA/容量；vendor admin GET_INFO 获取 FDM、CU、功能位，不使用环境变量绑定。FDM 属性对应设备容量；CU 映射为一个 CSE 中的多个 CR，固定 native CSEE 已分配所有 CR，`NumAvailableCRs=0` 不代表瞬时繁忙。

AFDM 保存输入、输出和共享中间数据；每个 E1 额外使用 4 KiB AFDM 输入描述。解码数组在 CU 栈上，私有堆统一使用设备共享 `libcs-workmem.so`，与 FDM 配额独立；这是一种合作式配额，并非 native 指令沙箱。

CSF 名称/GlobalId/激活状态由 Guest 进程内目录记录，同进程同路径共享。无法枚举别的进程加载的程序，仍需预留索引。设备重启后重新初始化应用。

## batch

最多 64 节点，配置深拷贝描述并持有资源引用。SERIAL 单线程；PARALLEL 独立节点；HYBRID 在全部前驱完成后启动。Before 为后继，After 为前驱；JOIN 加 After→Before，SPLIT 双索引切断 Before→After。环检测拒绝并回滚非法配置。并发工作线程数来自设备 CU 查询，最多 64；FEMU 实际执行仍限制 CU 数和同一程序 ID 的并发。

每个可恢复节点在独立请求副本上循环：E1 → NEED_STORE/LOAD/FREE → 标准 storage 请求或 DSM → 带确认控制值的 E1 → DONE。存入节点的原始描述保持不变，因此图可重复提交。整个循环完成后才将该节点置为 done，依赖、callback 和 event 都基于逻辑完成。错误后不启动新节点，等待所有在途节点，然后回收本批新增 spill 区段。

临时池在首次使用时按 GPT PARTLABEL 找到 `femu-spill` 分区，用 sysfs 的 512 字节单位换算为实际 namespace LBA。数据始终通过 namespace 上的 D0/D1 命令传送，区段在 Guest 进程内按 LBA 排序进行首次适配分配。LOAD/STORE/FREE 在池锁内执行，避免复用正在读取的区段；运行 CSF 时不持池锁。独占 flock 持有在分区节点上，阻止另一个合作进程分配重叠范围。

成功 batch 的 token 可供后续 batch 使用；算子在私有状态中保存 token 和自己的算法状态。失败批次仅释放本批新增区段；旧 token 需显式 FREE。最后一个参与池的设备关闭时释放剩余区段。资源不是持久化数据库，不提供崩溃恢复。

## 生命周期与适配成本

句柄为登记表的 opaque token。图及请求持有 AFDM/CSF/device 引用，使用中的资源不能释放。异步完成先释放请求引用再通知，callback 不持登记表锁。HostVAddress 指向的数据不复制，调用者保留至完成。已开始的 native CSF 不支持强制抢占。

清零/填充通过 Guest F5；AFDM→AFDM fallback 使用 Guest 的 F2/F5 临时缓冲，支持重叠，具有外部传输。共享 AFDM 的相邻 CSF 无需复制。LOAD/STORE 的数据不经过 Guest。每次重入需要一次额外参数 AFDM 分配/写入/释放和 E1，这部分是调度开销，不能算成纯 CSF 计算性能。
