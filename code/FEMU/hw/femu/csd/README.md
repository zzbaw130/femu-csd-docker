# CSD 通用设备图与隐藏临时空间

设备图是一份由主机提交的有限有向无环图。节点只有 LOAD、EXEC、STORE，FEMU 不理解 SQL、tuple 或 pipeline 的业务含义。主机决定计算函数、AFDM 缓冲及依赖，设备负责在前驱完成后自动启动后继。

SNIA 应用仍包含 `cs.h`、链接 `-lsnia`。Guest 中的 FEMU 插件将纯存储/计算 batch 编码为设备图；下文描述的是本项目设备接口，不是 SNIA 标准的新 API。

## 执行流程

手动测试CPU亲和性时，可使用同级`scripts/pin.sh`：`QEMU_PID=<pid> bash pin.sh <FIRST_CPU>`。CSD模式会先依次绑定CU、FTL和poller，再绑定Guest vCPU，其余QEMU线程使用剩余CPU。应先查看宿主CPU/NUMA拓扑，选择能容纳设备线程组的起点；连续CPU编号不保证属于同一NUMA节点。脚本不在启动时自动执行，绑核收益需要单独测量。

```text
Guest 应用 -> libsnia -> FEMU 插件 -> 一条 E9 EXEC_GRAPH
                                       |
FEMU 原有 FTL 线程：取出到期节点，提交正常 FTL/NAND I/O
                  用 pqueue 保存完成时间，完成时解锁后继并加入就绪队列
                                       |
FEMU nr_cu 个 CU 线程：执行就绪 CSF，返回后释放 CU
                                       |
全图结束 -> 一次 NVMe 完成 -> Guest 的 callback/event
```

I/O 等待不占 CU；多个独立分支可以同时处于 I/O 或计算阶段，实际读取和计算可在不同分支间重叠。一个 EXEC 节点仍是一次 CSF 调用，不自动把算子拆分为多 CU。主机应使用不同输入块和缓冲构建独立分支，显式为共享 AFDM 的读写添加依赖，设备不猜测数据冲突。

SERIAL 一次启动一个节点；无 SERIAL 标志时，所有前驱完成的节点均可启动。PARALLEL 与 HYBRID 的区别体现在主机构造的依赖边：前者没有边，后者等待全部前驱。使用原有 CU 线程池，不另起代理或调度进程。

完成时间队列复用 FEMU NVMe poller 的 pqueue 实现，但由 CSD 自己保存节点事件，避免混用 poller 所属的 NVMe 请求队列。节点完成时只处理其后继，用剩余前驱计数判断就绪，不再扫描全部在途图及节点。FTL 主循环仍轮询，但空闲路径仅检查缓存的最早完成时间和就绪队列是否非空，不取 CSD 锁、不遍历图。CU 返回时向完成时间队列插入事件；就绪 CSF 通过原有条件变量唤醒 CU。

设备在接受图时复制描述并保留程序、AFDM 引用；描述校验失败不执行节点。节点必须等待 NAND 延迟或 CSF 模拟执行时间到期后才能解锁后继。失败时停止新节点，等待在途操作结束并回收本图新增的临时 token。取消不抢占 native CSF，须等正在运行的入口返回。

## NVMe 命令

### E9：EXEC_GRAPH

I/O 命令，NSID 为目标 namespace，PRP 指向完整描述，CDW10 为描述字节数。E9 的数据方向是 Host→Device，与原生 E1 相同。整图至多64节点、描述至多1MiB，同时至多64张在途图。完成值通过64位 CQE 返回：成功为最后完成节点的普通返回值；节点失败为从1开始的节点编号，取消为0。多个末端的完成顺序不固定，各分支结果应保存在各自输出 AFDM 中。

线格式见同目录 `csd-graph.h`，整数均为小端，保留字段填0：

| 结构 | 大小 | 字段与含义 |
| --- | ---: | --- |
| femu_graph_header | 32字节 | count、flags；session、cookie；bytes、reserved |
| femu_graph_node | 64字节 | dependencies；cparam1、cparam2；operation、program；payload、count；afdm、data_offset；afdm_offset；data_bytes、reserved |
| femu_graph_range | 16字节 | nsid、blocks、lba；blocks 是实际 LBA 数，不是 NLB-1 |

header 后紧跟 count 个 node，其后存放 MR、I/O range 和附加字节。payload/data_offset 为描述开头起算的字节偏移，不能指向结构区。dependencies 的第 i 位表示依赖节点 i，支持第63位；不允许环或指向不存在节点的边。cookie 为主机提供的非零图编号，在同一会话内持续递增，不能复用仍有 token 的图编号。

- LOAD/STORE：payload 指向 count 个 range；afdm/afdm_offset 指定输入或输出缓冲，range 按顺序连续填入该缓冲。namespace LBA 必须在可见容量内，支持多 extent。
- EXEC：program 为已激活的程序编号；payload 指向 count 个原生32字节 MR。MR 的 namespace 为0、length为0表示整块 AFDM，参数映射继续复用 E1 的实现。零 MR 可执行纯标量 CSF。两个 cparam 各自独立，不拼接为128位。
- EXEC 的 data_offset/data_bytes 可指定附加输入，直接映射原有 data_buffer/buffer_len。它属于描述的设备副本，修改不自动写回主机。当前 SNIA 的 compute 参数尚未映射附加字节，故插件填0；原生设备调用可以使用。

### EA：GRAPH_SESSION

无数据载荷的 I/O 命令。CDW10 为操作，CDW11 为会话编号，CDW12/13 为64位 cookie。

| 操作 | 值 | 用途 |
| --- | ---: | --- |
| OPEN | 0 | 建立 token 归属会话，64位完成值返回32位会话编号；每台设备最多1024会话 |
| CLOSE | 1 | 无在途图时释放会话及其全部 token |
| CANCEL | 2 | 停止对应图；也可先记录取消以处理取消早于接收图的竞态 |
| FORGET_CANCEL | 3 | 清除对应 cookie 的取消记录，供插件处理完成与取消竞态 |
| ROLLBACK | 4 | 回收该已完成图新建的 token，供含主机复制的混合 batch 后续失败时收尾 |

会话是 FEMU 内存中的归属记录，不是进程，也不绑定 CU。FEMU 插件在设备句柄首次提交图时 OPEN，关闭句柄时 CLOSE。成功图的 token 可在同一设备句柄后续 batch 使用，不能跨会话访问。ROLLBACK 在相关图已停止后使用；不能用于回滚已有 token 的数据修改或普通 namespace 写入。

## CSF 的内部 STORE/LOAD/FREE

继续使用现有 `femu_csd_args` 和 `cs_csf.h`，不用增加 runtime 指针，也不直接给 CSF 暴露 FTL 内存地址。可重入 CSF 遵守以下项目约定：

1. cparam2 初值为0，并保留给重入确认；cparam1 和 MR 按自己的调用约定使用。
2. 当前 EXEC 的最后一个 MR 为临时 I/O 缓冲，放待 STORE 数据或接收 LOAD 数据。
3. 入口通过 csIsReentry 判断、csReentryValue 取得 token。自己的工作内存保存 pending 操作，知道这次确认的是哪一步。
4. 需要写出时返回 csNeedStore(bytes)。设备分配隐藏区段、写入最后一个 MR 的指定字节；完成后令 cparam2=新 token，重新排队调用同一入口。
5. 返回 csNeedLoad(token) 时，设备读取创建时记录的精确字节数；csNeedFree(token) 时回收映射。两者完成后均以 cparam2=原 token 重入。其余 MR、cparam1、data_buffer 保持原内容。
6. 普通非负完成值结束节点，负值表示执行失败。高4位为1/2/3/4的结果保留给 NEED_*，低60位表示长度、token或请求数量。这不是新增 CS_STATUS。

```text
EXEC(sort_sink)
  -> NEED_STORE(bytes)
  -> 设备 AFDM→隐藏 NVM，等待写入完成（释放 CU）
  -> 重入 sort_sink，cparam2=token
  -> 普通完成值
  -> 后继节点
```

token 只表示隐藏区段，不包含算子的 run 列表或 workmem 状态。成功图保留 token；显式 FREE、图失败回收或关闭会话释放。算子私有工作内存仍由算子清理入口释放，设备不知道其内部结构。单独原生 E1 或 SNIA csQueueComputeRequest 保持单次执行行为，只有设备图自动处理内部重入。

CSF也可在最后一个MR开头写入1～64个小端femu_graph_read记录，返回csNeedNvmRead(count)（高4位为4）。记录指定当前namespace的LBA/块数，以及本次调用的目标MR下标和字节偏移。设备先复制并验证整批描述，再通过既有FTL/AFDM复制及I/O完成事件逐范围读取；全部完成后以cparam2=count重入原CSF。描述区可以与目标区重叠，未通过验证不会发起部分读取。这里只处理物理范围，不认识文件、extent或SQL；与隐藏spill token互不混淆，CSF须自行记录正在等待哪类I/O。

## 隐藏空间配置

`-device femu,...,csd_spill_size_mb=256` 设置256MiB隐藏逻辑页，属性默认0（不分配）。`scripts/run-csd.sh` 读取 `CSD_SPILL_SIZE_MB`，未设置时使用256MiB。配置只对新启动设备生效。

隐藏页位于 namespace 可见逻辑地址之后，主机普通读写及设备图的显式 LOAD/STORE 均无法访问。它们共享正常 NAND、FTL 映射、时序与 GC，因此消耗真实模拟容量，不是无时延的另一块工作内存。设备用页位图分配连续区段，保留精确有效长度；FREE 走 FTL trim。模拟 NAND 数据由设备 backing 保存，不计入 FDM/workmem 配额。

不能把全部 OP 都划成临时页：namespace + 隐藏页仍须通过正常容量检查，为 GC 留出原有保留量；空间不足时拒绝启动。可降低 devsz_mb 或增大几何容量来预留空间。目前隐藏临时空间要求单 namespace、page mapping，关闭 namespace management、power_loss、metadata、Streams、FDP 和块退休；不提供持久化或崩溃恢复。

无需 Guest 分区。原有 femu-spill 分区不会自动使用、删除或合并，已有数据盘布局不因本功能改变。

## 针对性验证

新增 qtest 用独立 QEMU 实例验证设备图，测试程序直接走 NVMe 队列，不需要启动或重启业务 Guest。现有 build-femu 构建中：

```sh
ninja qemu-system-x86_64 tests/qtest/qos-test
QTEST_QEMU_BINARY=./qemu-system-x86_64 tests/qtest/qos-test \
  -p /x86_64/pc/i440FX-pcihost/pci-bus-pc/pci-bus/femu/femu-tests/csd-exec-graph
```

其余针对性用例为 csd-exec-graph-spill、csd-exec-graph-spill-4k、csd-exec-graph-cancel，分别覆盖跨图 token、空间不足/隔离/回收、4KiB LBA 和取消。旧接口回归使用 csd-nvm-copy、csd-info-result64、csd-exec-concurrency。

## 设备图性能诊断

Guest 的 SNIA_FEMU_PROFILE=1 同时令插件在设备图 flags 中设置 PROFILE（位1，值2）。这是本项目私有诊断位，不是能力查询标志，不改变 CSF 参数或标准 SNIA API。未开启时不采集下列详细计时。会话关闭时 FEMU stderr 输出一行 CSD_GRAPH，可从容器日志查看：

| 字段 | 含义 |
| --- | --- |
| graphs、graph_ns | 已完成的诊断图数、从设备接受请求至交回完成队列的累计时间 |
| events、event_late_ns | 已处理到期事件数、观察时间晚于 deadline 的累计时间；包含宿主调度和模拟 I/O 的实际处理超过模型延迟等因素，不等于纯调度器 CPU |
| io_calls、io_model_ns | 内部 I/O 段数和 FTL 返回的累计模拟延迟，包含 NAND 排队 |
| io_work_ns | FTL 处理及 backing/AFDM 复制的累计实际经过时间；使用墙钟，不是线程 CPU 时钟 |
| cu_calls、cu_wall_ns | 图内 CSF 调用次数、实际运行墙钟之和 |
| cu_queue_ns | CSF 入队到 CU 取走任务的累计等待 |
| dispatch_ns | 处理到期事件和就绪图的累计墙钟，包含内部 I/O 处理，是 io_work_ns 的上层范围 |

这些累计值会跨分支重叠；dispatch_ns 包含 io_work_ns，不能相加。它们应与 PG 独立采集的算子 CPU、阶段墙钟、AFDM 和 spill 统计共同分析。
