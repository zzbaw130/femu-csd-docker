# 完整排序后分批计算平均值

Guest应用为cs_batch_sort_avg.c，普通CPU基线为code/csd-programs/examples/tests/read_sort_avg.c；设备sort.cpp和avg.c在容器编译为cs-example-sort.so和cs-example-avg.so，由FEMU的CU线程执行。Guest应用只包含cs.h并链接libsnia-cs-api.so。batch_sort.h是示例共用的数据布局、排序、大根堆和平均值累加实现。

## 编译与运行

使用已挂载的femu-data分区及未挂载的femu-spill分区。在宿主项目根目录：

```sh
bash tests/snia-cs-api-test.sh build
bash tests/snia-cs-api-test.sh example
bash tests/snia-cs-api-test.sh bench
```

example创建或覆盖/data/cs-batch-input.bin，写入256MiB确定性伪随机int32（含正负数），生成时独立计算参考总和，最终核对完整消费的数据个数、总和与均值。文件保留供后续bench使用，实验期间不能修改或移动。程序索引420–422，需要避免与其他进程的设备程序冲突。

SDK已安装时，也可在Guest独立编译：

```sh
cc cs_batch_sort_avg.c -O2 -lsnia-cs-api -o cs-batch-sort-avg
sudo ./cs-batch-sort-avg /dev/nvme0n1 /data/cs-batch-input.bin
sudo ./cs-batch-sort-avg /dev/nvme0n1 /data/cs-batch-input.bin --existing
```

--existing只读复用文件，不生成数据或在Guest重新读取正文来计算参考值。bench交替运行普通路径和CSD各3轮，检查每次sum、count、average完全一致，count必须等于67,108,864；同时核对spill读写量一致、内存峰值不超过64MiB。普通程序在容器用兼容Guest的工具链编译，和同一workmem库一起传入Guest的/tmp/csd-sort-bench。

## 两条pipeline

```text
第一条，重复64次，每次4MiB：
  csQueueStorageRequest(LOAD, input)
    -> sort_sink(input, io, sort_handle)

全部输入完成后，反复执行第二条，直到result.finished=1：
  sort_source(sorted, io, sort_handle)
    -> avg_int32(sorted, result)
```

两条均使用csAllocBatchRequest、csConfigureBatchEntry和csQueueBatchRequest构造SERIAL batch。第一条每次按文件extent更新LOAD；第一次sink创建workmem状态，以CompValue返回64位sort_handle，后续sink/source通过cparam1原样传入。cparam2留给库在NEED_STORE/LOAD/FREE完成后传入重入标识，Guest不解引用sort_handle。

第二条的AVG消费每一批排序输出并更新结果，不会因为已经有一个均值就提前退出。只有source真正到EOF才传递finished=1；此时source已FREE所有spill token并销毁workmem，应用将sort_handle清零并结束循环。保留完整排序是为了验证阻塞算子、spill、全量归并和下游分批聚合。

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
| input | 4MiB | 内部LOAD写入；sink的MR[0]读取 |
| io | 256KiB | sink/source的MR[1]，最后一个AFDM，供重入STORE/LOAD使用 |
| sorted | sizeof(sorted_chunk)，约4MiB | source的MR[0]输出；AVG的MR[0]输入 |
| result | sizeof(average_result) | AVG的MR[1]保存累计结果；Guest只读取这块小结果 |

sort的run、描述、堆及归并读缓冲全部来自同一个workmem context。每个4MiB run优先驻留，申请失败才淘汰编号最小的驻留run，将完整run保存成16个256KiB spill块；归并为每个spill run缓存256KiB。始终是64个run，分块I/O不会增加run数量。

大根堆缓存run头部的整数和编号，避免每输出一个值都扫描全部run。重入后先完成待更新的堆顶，再输出下一值，跨输出chunk边界也保留该状态。最后一个chunk即使恰好装满，也已经到EOF，不能再用旧handle调用source。

两条路径的64MiB算法总预算包含四个I/O缓冲，以及run、状态、堆、归并缓存和workmem块头。扣除缓冲后，工作区约55.75MiB。设备用context配额限制，CPU建立同样大小的独立工作池；FEMU全局FDM/workmem仍各64MiB。代码、线程栈和库固定管理开销不属于算法预算，未设置整个进程RSS上限。

普通路径用O_DIRECT读输入、读写输入目录中的临时文件；临时文件创建后立即unlink，关闭时释放。CSD的LOAD/STORE在设备内部完成，临时块位于femu-spill分区。错误收尾仍可调用source的单AFDM用法释放workmem，随后关闭设备回收剩余spill。

## extent与请求构造

文件生成并fsync后，用FIEMAP查询物理extent。其地址相对于数据分区，因此LOAD地址需要加sysfs查询到的分区起始LBA；每个逻辑4MiB范围内的一个或多个extent按顺序转为CSBlockRange。原始正文直接从NVM载入input，不经过Guest中转；不支持空洞、未写入或不对齐的extent。

CsComputeRequest描述计算，CsBatchRequest是batch节点外层，ReqType区分计算和存储。main只复用一份LOAD描述、一份计算描述；build_compute_request仅填写内部计算请求。csConfigureBatchEntry保存描述副本并保留资源引用，不复制AFDM数据，因此先释放batch，再释放其AFDM。

## 计时与性能测试

TIMING两阶段依次为“读取、生成run及必要spill”和“完整归并、AVG及回收”。排除文件生成/打开、extent查询、程序下载/激活、初始AFDM及工作池初始化；运行中的内存分配、I/O、重入、完整计算和临时数据回收均计时。

PROFILE区分sink_work、source_work与average_work，报告墙钟及线程CPU时间；run_sort_s是sink_work的子集，不能重复相加。设备统计各次CSF入口的执行，普通路径从阶段耗时扣除同步文件I/O和AVG时间得到相同口径的source_work。

bench启用SNIA_FEMU_PROFILE=1，Guest插件在设备关闭时向stderr输出E1、D0、D1、TRIM、F2次数及累计ioctl等待。E1包含三个CSF的执行，不能再与算子时间相加；D0包含文件LOAD和spill LOAD。HYBRID时累计时间可能重叠，本测试保留SERIAL，不自动调度多CU或预取。

原始日志、timings.tsv和profiles.tsv写入脚本打印的/tmp/snia-sort-avg.*。CPU/CSD是平均总延迟之比，大于1表示CSD更快。设备算子实际运行在宿主CU线程，CPU基线在Guest运行，结果不能直接代表真实SSD计算核性能。此前sort→LIMIT的数据保留在[开发记录](../doc/development.md)，不作为当前sort→AVG的结果。


2026-10-20，同一Guest（8vCPU、8GiB）、2个extent的256MiB文件，64MiB算法预算、单线程/SERIAL模式实测：

| 轮次 | 普通路径（秒） | CSD（秒） |
| --- | ---: | ---: |
| 1 | 13.827569 | 13.388101 |
| 2 | 13.538000 | 13.345029 |
| 3 | 13.739177 | 13.465496 |
| 平均 | 13.701582 | 13.399542 |

六次结果及生成时的独立参考一致：count=67108864，sum=15688155813636，average=233771.73861318827。两边均64个run、累计spill 54个，写入/回读各216MiB，算法内存峰值约63.77MiB以内。当前第二条pipeline完整消费所有排序输出。

普通路径读取/生成run阶段平均8.780秒，归并/AVG/回收4.922秒；CSD分别8.383/5.016秒。AVG自身平均普通路径0.055秒、CSD 0.059秒。CSD run排序平均7.774秒，source执行4.550秒，内部D0/D1/TRIM累计0.239/0.229/0.115秒。原始日志及两份TSV位于/tmp/snia-sort-avg.GQq8lV。
