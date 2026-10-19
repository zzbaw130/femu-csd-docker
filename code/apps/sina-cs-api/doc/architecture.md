# 执行位置与内存

Guest 应用调用 Guest SNIA 库，经 Linux NVMe passthrough 提交给容器内 FEMU；设备 `.so` 在 FEMU CU 线程执行，逻辑上属于 CSD CPU。没有常驻代理。

## 参数

应用按算子约定构造参数。库只把 AFDM 转为 MR，把标量转为 cparam1/2；CSF 直接访问这些字段。无参数描述缓冲或额外解码层。原生两个标量位置无法表达的请求返回不支持，复杂输入由应用自己定义数据布局。

AFDM 保存输入/输出。私有排序状态、hash 表和缓冲在设备 libcs-workmem.so 中申请，可跨入口保留；共享池是合作式配额，不是 native 沙箱。

## batch 与重入

图和依赖在 Guest 内存中，最多 64 节点。SERIAL 单线程；PARALLEL 独立节点；HYBRID 等全部前驱完成。Before 为后继、After 为前驱；JOIN 添加 After→Before。描述深拷贝并保留资源引用，配置阶段拒绝环。

batch 节点先执行普通 E1。返回 STORE/LOAD/FREE 请求时，库完成 I/O，在请求副本上把 cparam2 填为 spill_id，再次执行。节点直到返回普通结果才完成；请求副本保留原输入，配置图可重复提交。重入算子自行保存 pending 操作、长度和 run 列表。

临时池按 GPT PARTLABEL=femu-spill 发现。区段按实际 namespace LBA 分配，数据走内部 D0/D1，Guest 只管理控制信息。池锁覆盖区段 I/O，避免重用正被读取的区段，执行 CSF 不持池锁。同进程共享池，分区节点上的合作式独占 flock 防止其他本库进程重叠分配。

成功批次保留 spill_id；失败批次等在途节点退出后释放本批新增区段；显式 FREE 或最后一个参与池的设备关闭时清理。查询私有状态由算子自己的清理入口释放。没有跨进程标识共享或崩溃恢复。

## 资源与限制

Identify 验证身份和 namespace 格式；GET_INFO 获取 FDM/CU，FDM 属性使用设备值，CU 数限制工作线程。固定 native CSEE 已占用全部 CR，所以 NumAvailableCRs=0，含义是没有未分配资源，不是瞬时繁忙。

程序目录只记录当前进程经本库加载的函数，同路径的多个句柄共享；需预留程序索引。AFDM/CSF/device 被图或请求引用时不能释放。异步 HostVAddress 数据由调用者保留到完成，callback 不持句柄表锁。运行中的 native CSF 不能抢占。

清零/填充使用 Guest F5；AFDM→AFDM fallback 经 Guest F2/F5。LOAD/STORE 数据留在设备内部。重入需要新的 E1 和控制提交，不再增加参数 AFDM 的分配、写入和释放。
