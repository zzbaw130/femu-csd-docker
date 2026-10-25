# 四路并行排序、单路归并后计算平均值

Guest应用为cs_batch_sort_avg.c，普通CPU基线为code/csd-programs/examples/tests/read_merge_sort_avg.c；设备merge-sort.cpp和avg.c在容器编译为merge-sort.so和cs-example-avg.so，由FEMU的CU线程执行。Guest应用只包含cs.h并链接libsnia.so。batch_sort.h是示例共用的数据布局、排序、大根堆和平均值累加实现。

## 编译与运行

使用已挂载的femu-data分区，FEMU配置非零csd_spill_size_mb，运行配套新固件与插件。在宿主项目根目录：

```sh
bash tests/snia-test.sh build
bash tests/snia-test.sh example
bash tests/snia-test.sh bench
```

example创建或覆盖/data/cs-batch-input.bin，写入256MiB确定性伪随机int32（含正负数），生成时独立计算参考总和，最终核对完整消费的数据个数、总和与均值。文件保留供后续bench使用，实验期间不能修改或移动。程序索引420–422，需要避免与其他进程的设备程序冲突。

SDK已安装时，也可在Guest独立编译：

```sh
cc cs_batch_sort_avg.c -O2 -lsnia -o cs-batch-sort-avg
sudo ./cs-batch-sort-avg /dev/nvme0n1 /data/cs-batch-input.bin
sudo ./cs-batch-sort-avg /dev/nvme0n1 /data/cs-batch-input.bin --existing
```

--existing只读复用文件，不生成数据或在Guest重新读取正文来计算参考值。bench交替运行普通路径和CSD各3轮，检查每次sum、count、average完全一致，count必须等于67,108,864；同时核对spill读写量一致、内存峰值不超过64MiB。普通程序在容器用兼容Guest的工具链编译，和同一workmem库一起传入Guest的/tmp/csd-sort-bench。

## 两条pipeline

```text
第一条，HYBRID batch，四条分支并行，每轮共16MiB，重复16轮：
  LOAD(input[0]) -> sort_sink(input[0], task[0], io[0])
  LOAD(input[1]) -> sort_sink(input[1], task[1], io[1])
  LOAD(input[2]) -> sort_sink(input[2], task[2], io[2])
  LOAD(input[3]) -> sort_sink(input[3], task[3], io[3])

所有sink完成，Guest把四个不透明状态句柄写入group AFDM：
  R0 ─┐
  R1 ─┼-> sort_source(sorted, group, io[0], state0) -> avg_int32(sorted, result)
  R2 ─┤                 第二条使用SERIAL batch，重复到EOF
  R3 ─┘
```

第一条由csAllocBatchRequest、csConfigureBatchEntry构造8个节点：每个sink只依赖自己的LOAD，四条分支之间没有依赖。每轮csQueueBatchRequest等待全部完成，再更新四个LOAD的文件extent。四个sink使用同一个程序ID420，由设备最多4个CU执行；不是每个sink内部创建线程，也不是设备自动拆分计算。

task[i]是该分支的小型输入输出结构：workers=4指定本例的工作区分摊，state首次为0。sink首次创建自己的workmem状态，把64位句柄写回task[i].state；后续batch直接复用该AFDM。Guest不能解引用句柄，也不依赖HYBRID batch的单个CompValue取得四个结果。每个sink生成16个4MiB run，四个状态独立，cparam2仍留给设备重入I/O。

全部输入完成后，Guest读取四个task，只把句柄写到group.states[0..3]。第二条source以cparam1接收state0，首次调用将其余三组run的描述接入第一个状态，只复制描述，不复制run数据。每个run保留原来的workmem context归属；source从所有64个run产生一条全局降序流。group中其余三个句柄清零，表示它们的资源已交由source统一管理。

第二条每次输出最多4MiB，AVG消费每批数据，直至source FREE全部spill token并结束四个context。最后一批finished=1、group.states[0]=0；应用结束循环。没有增加Gather Merge、create、combine或abort CSF，也未实现并行归并。

## AVG如何跨批次累计

result AFDM中的average_result保存：

| 字段 | 用途 |
| --- | --- |
| sum（int64_t） | 目前所有已消费整数的精确总和 |
| count（uint64_t） | 目前已消费的整数总数 |
| average（double） | 每批更新为(double)sum/count |
| finished | 只跟随source的EOF，不作为提前停止信号 |
| aggregate、metrics | AVG自身及排序执行的诊断计时、内存和spill统计 |

result首次以CS_FDM_CLEAR分配，之后每个batch复用同一AFDM。AVG读取前次sum/count，加上当前chunk的总和及条数，再计算均值。不能把两批均值直接取平均，例如[10,20]与[90]的正确结果是120/3=40，而不是(15+90)/2。

256MiB包含2^26个int32，最坏总和绝对值不超过2^57，int64足够，累计过程中没有浮点求和误差。均值最后转换为double；count=0时average暂置0，由count区分无数据。该整数示例不包含SQL NULL语义。avg_int32的普通CompValue返回累计count，但应用只在EOF结束。

正常输出示例为AVERAGE count=67108864 sum=… value=…。总和与个数能检查聚合和消费完整性，不能证明输出有序，因此设备测试仍将全部排序输出与std::sort逐项比较。

## AFDM与私有工作内存

| AFDM | 大小 | 生产者与消费者 |
| --- | --- | --- |
| input[4] | 每个4MiB | 四路内部LOAD写入，各自sink的MR[0]读取 |
| task[4] | 每个sizeof(sort_sink_task)，AFDM按4KiB分配 | Guest设置workers/state；sink的MR[1]保存自己的状态句柄 |
| io[4] | 每个256KiB | sink的MR[2]；source复用io[0]作为MR[2]。始终是请求中最后一个AFDM，供STORE/LOAD使用 |
| group | sizeof(sort_run_group)，按4KiB分配 | Guest写入四个句柄；source的MR[1]接收并更新资源归属 |
| sorted | sizeof(sorted_chunk)，约4MiB | source的MR[0]输出；AVG的MR[0]输入 |
| result | sizeof(average_result)，按4KiB分配 | AVG的MR[1]保存累计结果；Guest读取这块小结果 |

四个sink各有一个workmem context。先扣除全部AFDM缓冲的分配大小，约21.03MiB；剩余约42.97MiB平均分给四个context，每份约10.74MiB，包含状态、run、归并读缓冲和分配器块头。CPU路径也保留相同的缓冲预算，采用相同的context配额；不会给每个线程/CU各64MiB。

每个4MiB run优先驻留，申请失败才淘汰本context编号最小的驻留run。一个run保存成16个256KiB spill块；分块I/O不会增加run数量。source为spill run申请256KiB头部缓存，配额不足时也只淘汰同一个context拥有的驻留run，防止从其他context释放空间却无法解决本context的配额不足。大根堆仍只有一份，最多64项。

WORKLOAD的peak是“各context历史峰值之和＋预留的全部I/O缓冲”的保守上界，各context峰值不一定同时发生。两条路径都必须小于64MiB。代码、线程栈和库固定管理开销不属于算法预算，未设置整个进程RSS上限。

普通路径用4个pthread，各自O_DIRECT读取交错分配的16个输入块、排序并按需spill；临时文件在输入目录创建后立即unlink，也使用O_DIRECT，原子分配互不重叠的写入区段。所有线程join后，主线程接入四组run并完成全局归并和AVG。CSD临时块位于设备隐藏空间，设备处理NEED_STORE/LOAD/FREE并自主重入，不经过Guest。

出错时，Guest先读取group或task中仍然有效的句柄，再逐个调用source的单AFDM用法释放工作内存。source已接管四组run时，只清理state0；随后关闭设备，回收尚存的spill token。原来的双MR单sink ABI仍供原生及SNIA回归使用，不影响本例的三MR并行方式。

## extent与请求构造

文件生成并fsync后，用FIEMAP查询物理extent。其地址相对于数据分区，因此LOAD地址需要加sysfs查询到的分区起始LBA；每个逻辑4MiB范围内的一个或多个extent按顺序转为CSBlockRange。原始正文直接从NVM载入input，不经过Guest中转；不支持空洞、未写入或不对齐的extent。

CsComputeRequest描述计算，CsBatchRequest是batch节点外层，ReqType区分计算和存储。main复用一份LOAD描述、一份计算描述，为四条分支分别配置快照；build_compute_request仅填写内部计算请求。csConfigureBatchEntry保存描述副本并保留资源引用，不复制AFDM数据，因此先释放batch，再释放其AFDM。

## 计时与性能测试

TIMING两阶段依次为“读取、生成run及必要spill”和“完整归并、AVG及回收”。排除文件生成/打开、extent查询、程序下载/激活、初始AFDM及工作池初始化；运行中的内存分配、I/O、重入、完整计算和临时数据回收均计时。

PROFILE区分sink_work、source_work与average_work，报告墙钟及线程CPU时间；run_sort_s是sink_work的子集，不能重复相加。设备统计各次CSF入口的执行，普通路径从阶段耗时扣除同步文件I/O和AVG时间得到相同口径的source_work。

bench启用SNIA_FEMU_PROFILE=1，Guest插件在设备关闭时向stderr输出EXEC_GRAPH及单次E1、D0、D1、TRIM、F2的次数与累计ioctl等待。此例的存储/计算batch提交整张设备图，因此图内节点不计入Guest的E1/D0/D1次数，内部spill也不产生这些Guest命令。EXEC_GRAPH包含内部I/O与CSF执行，不能再与算子时间相加。HYBRID中各分支时间存在重叠；sink_work、run_sort_s及ioctl墙钟均是各次调用耗时之和，不能当作第一阶段的实际延迟。第一阶段延迟以TIMING为准；sink线程CPU累计时间与阶段墙钟之比可以观察并行利用率。

原始日志、timings.tsv和profiles.tsv写入脚本打印的/tmp/snia-sort-avg.*。CPU/CSD是平均总延迟之比，大于1表示CSD更快。设备算子实际运行在宿主CU线程，CPU基线在Guest运行，结果不能直接代表真实SSD计算核性能。
