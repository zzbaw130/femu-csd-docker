# 文件排序并取最大的5个整数

cs_batch_sort_limit.c 在 Guest 运行，只通过 cs.h 和 libsnia-cs-api.so 调用设备。batch_sort.h 是本示例共享的数据布局；设备 sort.cpp、limit.c 位于 code/csd-programs/examples，在容器编译。排序只有 sink、source 两个入口。

## 编译运行

使用已挂载的 femu-data 分区和未挂载的 femu-spill 分区。在宿主项目根目录：

```sh
bash tests/snia-cs-api-test.sh build
bash tests/snia-cs-api-test.sh example
```

SDK 已安装时，也可在 Guest 独立编译：

```sh
cc cs_batch_sort_limit.c -O2 -lsnia-cs-api -o cs-batch-sort-limit
sudo ./cs-batch-sort-limit /dev/nvme0n1 /data/cs-batch-input.bin
```

应用创建或覆盖指定文件，写入256MiB伪随机 int32（含正负数），生成时独立维护参考前5，最终核对结果。程序索引420–422；文件保留供查看。实验期间不能写入或移动该文件。

## 简单性能对比

已有示例生成的数据文件后，在宿主运行：

```sh
bash tests/snia-cs-api-test.sh bench
```

普通路径是独立 C 程序 [read_sort_limit.c](../../../csd-programs/examples/tests/read_sort_limit.c)，不调用 SNIA 或 NVMe API。它和设备 sort.cpp 共用 batch_sort.h 中的原地降序 quicksort，以及 libcs-workmem.so 分配器实现（Guest 与 FEMU 进程各有独立的池）。两边都单线程执行以下流程：

1. 顺序读取同一256MiB文件，每4MiB原地排序，产生64个 run。
2. 申请 run 内存成功就驻留，失败才淘汰编号最小的驻留 run。每次完整写出4MiB，分16次256KiB I/O。
3. 为 spill run 按需申请256KiB头部读缓冲；申请失败仍淘汰驻留 run。用大根堆缓存各run的当前头部，每次只推进堆顶对应run并调整堆，完成全量归并。
4. 每4MiB输出交给 LIMIT，保留首5个数，但继续归并到 EOF，计时包含释放工作内存和临时数据。

两条路径的**算法内存总预算都是64MiB**：run 数据、状态、读缓冲、分配块头，以及 input、io、sorted、result 四个缓冲都计入。先从64MiB扣除这四个缓冲，剩余约55.75MiB给 workmem（容量向下按页对齐）。CPU 缓冲是进程内存；设备缓冲是 AFDM，设备通过 context 配额扣除同样的空间，FDM 容量和共享 workmem 的全局配置仍各64MiB。这是算法与 I/O 缓冲预算，不是操作系统对整个 Guest/FEMU 进程 RSS 的限制；代码、线程栈和库的固定管理开销不在此预算中。

普通路径的输入和临时文件均使用同步 O_DIRECT，绕过 Guest 页缓存，避免临时数据借用不受上述预算限制的缓存。临时文件创建在输入旁边（/data，正常数据分区），创建后立即 unlink，关闭时释放；CSD 走内部 LOAD/STORE，在 femu-spill 分区保存临时块。两者访问同一 FEMU namespace，仍分别包含文件系统路径和内部设备路径的实际成本。O_DIRECT 表示绕过页缓存，不额外承诺掉电持久性。

脚本在容器用与 Guest 兼容的工具链编译普通 C 程序，并把二进制及同一 workmem 库传到 Guest /tmp/csd-sort-bench；CSD 应用仍在 Guest 编译。bench 交替运行两种路径各3轮，检查前5一致、run 数与逻辑 spill 读写量一致、算法内存峰值不超过64MiB。CSD 使用 --existing，不重新生成文件。

计时排除文件打开、临时文件创建、文件生成、extent 查询、程序下载/激活、初始 I/O 缓冲和内存池初始化。两边均计入运行中的状态/run/归并缓冲申请、I/O、排序、完整归并和临时数据回收；CSD 另含 batch 配置、重入和小结果复制。TIMING 行的两阶段统一为“读入+生成run+必要spill”和“归并+LIMIT+回收”。WORKLOAD 行打印 run 数、spill run 数、读写字节、总内存峰值和预算。

日志、timings.tsv及profiles.tsv写入脚本打印的 /tmp/snia-sort-limit.*。CPU/CSD 为平均总延迟之比，大于1表示 CSD 更快。设备算子实际在宿主 CU 线程运行，普通路径在 Guest CPU 运行；测量用于比较当前仿真实现，不能直接代表真实 SSD 计算核性能。此前 CPU 全量内存排序10.19秒与 CSD 61.34秒的比较使用不同内存预算和算法，不能据此得出同等条件下的加速比。

2026-10-20改为大根堆前的历史结果：同一 Guest（8 vCPU、8GiB）和文件（3个extent），CPU单线程、CSD使用SERIAL batch：

| 轮次 | 普通文件路径（秒） | CSD路径（秒） |
| --- | ---: | ---: |
| 1 | 85.431 | 61.988 |
| 2 | 85.177 | 61.805 |
| 3 | 85.096 | 61.521 |
| 平均 | 85.234 | 61.771 |

两条路径每轮均为64个run，累计spill 54个run，写入与回读各216MiB。CPU内存峰值63.753MiB、CSD为63.762MiB，差别主要来自CPU文件偏移与设备token描述的大小不同。全部前5一致。普通路径两个阶段平均8.632/76.603秒，CSD为8.459/53.312秒，总延迟比CPU/CSD为1.380，CSD延迟降低约27.5%。原始数据：/tmp/snia-sort-limit.tvUcHU/timings.tsv。

这次两边均执行同一逐值遍历64个run的归并逻辑，归并阶段仍占主要时间。两阶段包含计算与I/O，没有单独测量各项开销，不能把23.29秒的归并阶段差距全部归因于内部带宽；Guest与宿主的执行成本、编译器生成的循环、文件系统/内部I/O和重入均可能影响结果。

## 细分计时与大根堆归并

batch_sort.h中的大根堆由设备和普通C程序共用，只有64个固定槽，内存计入workmem。初始化时读取各run头部；之后输出堆顶，只推进该run并沿堆向下调整。堆里保存整数值和run编号，不保存可能在spill时失效的数据指针。跨块LOAD或STORE返回后，source先完成上次尚未完成的堆顶更新，再输出下一值，输出chunk边界也保留这个状态。

PROFILE行的计时含义：

- sink_work/source_work：算法执行墙钟和线程CPU时间，包含复制、分配、排序/堆操作及状态管理。设备统计各次CSF入口的累计执行时间，不含入口之间等待Guest处理I/O的时间。普通路径用阶段时间扣除同步文件I/O时间得到同口径工作时间。
- run_sort_s：run原地排序墙钟时间，是sink_work的子集，不能再次相加。
- 普通路径input_read/spill_write/spill_read/release：对应文件I/O及临时文件关闭的累计等待与线程CPU时间。
- 设置SNIA_FEMU_PROFILE=1后，Guest FEMU插件在关闭设备时向stderr输出E1、D0、D1、TRIM、F2的请求次数与累计ioctl墙钟时间。bench已自动开启并收集stderr；其他应用默认不开启。统计针对设备句柄生命周期，初始化或错误收尾中的同类命令也会计入。
- E1时间包含CSF执行、排队、模型延迟和命令往返，不能与sink_work/source_work直接相加。两者之差还包含LIMIT计算及测量误差，不能全部解释为协议开销。D0同时包含原始文件LOAD和spill LOAD。墙钟减线程CPU时间也不单独等于NAND等待。HYBRID并发时不同请求的累计时间可能重叠，不能直接相加当作总延迟；这里采用SERIAL进行拆分。

保留SERIAL模式、原4MiB run和256KiB spill块、完整输出及64MiB预算；此阶段只改变归并选择算法，没有自动分配多CU或添加预取。多CU由应用显式构造独立任务和HYBRID依赖；底层不会推测算子能否并行。

2026-10-20新增计时后先测原算法一轮，CSD总延迟62.344995秒：source累计执行52.925500秒、线程CPU 52.903354秒；sink执行7.974355秒，其中run排序7.804513秒。E1累计61.220167秒，D0/D1/TRIM分别0.595320/0.246173/0.260565秒。原归并阶段主要消耗CPU，而不是等待内部I/O。

随后切换大根堆，交替测量3轮：

| 轮次 | 普通文件路径（秒） | CSD路径（秒） |
| --- | ---: | ---: |
| 1 | 13.337534 | 13.624559 |
| 2 | 13.470178 | 13.289712 |
| 3 | 13.531890 | 13.485604 |
| 平均 | 13.446534 | 13.466625 |

相对原有三轮均值85.234/61.771秒，普通路径总延迟约缩短6.34倍，CSD约缩短4.59倍。新的CPU/CSD约0.999，两条路径基本相当，不再表现出原逐run扫描实现下的1.38倍差距。两边均64个run、54个spill run、各216MiB写入和回读，内存峰值仍小于64MiB，全部前5一致。

CSD三轮平均分解如下，表中各行基本互斥：

| 内容 | 秒 | 口径 |
| --- | ---: | --- |
| run排序 | 7.796 | sink中的原地quicksort |
| sink其他工作 | 0.229 | sink累计执行减run排序 |
| source执行 | 4.570 | 堆归并、复制、分配、状态及回收；线程CPU为4.567秒 |
| E1其余部分 | 0.214 | E1累计12.809秒减sink与source执行，包含LIMIT执行和命令路径 |
| 内部LOAD D0 | 0.217 | 原始数据64次与spill 864次，共928次 |
| 内部STORE D1 | 0.296 | 864次 |
| TRIM | 0.126 | 864次 |
| 结果读取F2 | 0.002 | 64次 |

剩余约0.016秒为上述ioctl之外的应用/库执行与计时边界差。当前主要时间已转移到run排序，其次是堆归并；D0+D1累计约0.513秒，即使完全隐藏也只占总延迟几个百分点。计数与字节量未减少，说明本次收益主要来自归并计算，而不是省略数据或spill。

普通路径sink/source工作时间平均8.097/4.585秒，run排序7.879秒；输入读取、spill写入、spill回读、临时文件关闭分别约0.263/0.348/0.154/0.00019秒。日志和两份TSV位于/tmp/snia-sort-limit.f3wFHU，before-CPU.log、before-CSD.log保存新增计时后的原算法基线。profiles.tsv每行是一个模式、轮次和操作的累计值；run_sort_s仍是sink子集，不可重复相加。

## LIMIT提前结束的现有支持

当前limit_five返回累计条数。对于本例source→limit的同步SERIAL batch，最后节点的CompValue就是这个条数，应用可以传入完成值指针，并在值达到5后停止提交下一批；也可以读取结果AFDM的count。为判断source是否已自行销毁，仍应读取现有finished字段，先在finished=1时将sort_handle清零，再判断是否break。带有其他并行分支时，batch的最终完成值不一定来自LIMIT，读取LIMIT的输出AFDM更明确。无需增加SNIA返回状态。

现有result.finished仅表示source已到EOF并释放了状态，不能在LIMIT满足时直接置它为1，否则应用会误以为排序状态已经销毁。若以后需要显式通知，可在算子输出中增加独立的limit_done；这是示例算子的数据约定，不是SNIA状态码。

提前break后若sort_handle仍非零，应用还必须走source的现有收尾用法（只传io和状态标识），释放workmem；关闭设备回收未释放的spill。若要保持设备句柄继续使用，则还需要让source在收尾时通过batch逐个NEED_FREE，不能仅丢弃这个handle。当前收尾用法不会主动FREE所有token。

停止应用循环只能避免下一批，不能中断已经执行的source或省去首个4MiB输出中的多余归并。进一步优化需让source知道每次最多输出几条，或把输出chunk做小。此次性能测试仍读完全部数据。

## extent 与内部 LOAD

fsync 后用 FIEMAP 查询物理 extent。FIEMAP 地址相对于数据分区；LOAD 的 StartLba 相对于 namespace，所以要加分区起始位置。应用从 sysfs 读取分区起始扇区和 namespace id，从块设备读取逻辑块大小，检查文件位于指定设备上。

每次取文件逻辑范围中的4MiB，将其覆盖的一个或多个 extent 按逻辑顺序转为 CSBlockRange，放入同一 csQueueStorageRequest。内部 D0 连续写入 input AFDM，正文不经过 Guest 中转。空洞、未写入、编码或不对齐的 extent 被拒绝；文件保持打开并持有协作式独占锁。

## AFDM 与参数

| AFDM | 大小 | 使用方式 |
| --- | --- | --- |
| input | 4MiB | LOAD 写入，sink 的 MR[0] 读取 |
| io | 256KiB | sink/source 的 MR[1]，最后一个 AFDM；保存一个待 STORE 或刚 LOAD 的 spill 块 |
| sorted | sizeof(sorted_chunk)，约4MiB | source 的 MR[0] 写 count、finished、运行统计和全局降序整数；limit 的 MR[0] 读 |
| result | sizeof(top_five) | limit 的 MR[1] 保留前5并传递 finished；Guest 只读这个小结果 |

首次 sink 只传 input、io，cparam1 默认为0。sink 创建状态、保存第一个 run，并把64位状态标识作为普通 CompValue 返回。Guest 把它加入之后 sink/source 请求的第一个标量位置。cparam2 始终留给库在重入时传入刚完成 I/O 的 spill_id。Guest 不解引用状态标识。

## batch 执行

`CsComputeRequest` 描述一次计算；`CsBatchRequest` 是 batch 节点的外层结构，用 ReqType 区分计算、复制或存储，计算内容放在 u.Compute 中。示例的 build_compute_request 只填写 CsComputeRequest；main 显式设置外层类型，再交给 csConfigureBatchEntry。直接调用 csQueueComputeRequest 时则传入内部的计算请求。

main 只申请一份 LOAD 节点缓冲和一份计算节点缓冲。csConfigureBatchEntry 会复制请求描述，因此同一计算缓冲可以依次配置 sink、source、limit，不必为每个节点再申请一份。结果复制请求直接在读取循环旁构造并提交，提前收尾直接调用 source 的计算请求。

```text
阶段一，重复64次，仅更新 LOAD 的 extent：
  LOAD(input) -> sort_sink(input, io, sort_handle)
                     |
                     +-- workmem 申请成功：排序并驻留，不执行 STORE
                     +-- workmem 申请失败：spill 一个驻留 run，重入记录 token，再申请新 run

第一批返回 sort_handle，更新 sink 参数；后续批次继续使用同一个状态。
全部输入完成，释放阶段一 batch。

阶段二，重复提交同一 batch，直到 result.finished=1：
  sort_source(sorted, io, sort_handle) -> limit_five(sorted, result)
      |
      +-- 直接读驻留 run；spill run 通过 NEED_LOAD/重入读取下一块
      +-- 归并读缓冲申请失败：先 spill 一个驻留 run 腾出内存
      +-- 最后一个 chunk：FREE 所有 spill token，结束 workmem，输出 finished=1
```

排序是阻塞算子，因此所有输入都到达 sink 后，source 才开始输出全局有序数据。两个阶段都通过 csAllocBatchRequest、csConfigureBatchEntry 和 csQueueBatchRequest 组织；算子返回 NEED_* 后，FEMU 插件自动执行内部 I/O 和重入。

## 4MiB run 与256KiB spill 块

4MiB 是一次排序的输入和一个 run 的大小。256KiB 是 I/O 及归并缓存的大小；只有需要淘汰 run 时，才连续 STORE 16次，保存完整4MiB。归并只需为每个 spill run 缓存下一段，而不是同时把所有完整 run 载入内存。

所有 run 数据、读缓冲和描述都由同一 workmem context 分配。示例从64MiB总预算扣除 AFDM 缓冲后限制 context；256MiB输入会触发申请失败、淘汰和重入。没有按输入字节计数的固定 spill 阈值。

始终只有64个4MiB run，并没有把它们重新排序成1024个小 run。若同时为64个 run 各准备4MiB读缓冲，需要256MiB；各用256KiB只需16MiB，因此可以在这个预算内一次归并全部 run。256KiB是本示例选取的块大小，不是规范要求；它减少归并缓存占用，也增加 STORE/LOAD 次数。

source 每次最多输出4MiB，最后一批同时带 finished=1；即使最后一批恰好装满，也已经销毁状态，不能再调用一次探测 EOF。本示例把整个有序流读完，以展示正常结束自动回收；limit 始终保留首5，后续 chunk 不覆盖它们。

错误或提前停止时，通过 source 的收尾用法（仅传 io 和状态标识）释放私有内存，随后关闭设备句柄回收临时区。正常 EOF 则由 source 主动 FREE 后销毁。状态/token 不提供崩溃恢复。
