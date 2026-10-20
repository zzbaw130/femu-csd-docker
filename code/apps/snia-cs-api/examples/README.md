# 四路并行排序、单路归并后计算平均值

Guest应用为cs_batch_sort_avg.c，普通CPU基线为code/csd-programs/examples/tests/read_merge_sort_avg.c；设备merge-sort.cpp和avg.c在容器编译为merge-sort.so和cs-example-avg.so，由FEMU的CU线程执行。Guest应用只包含cs.h并链接libsnia-cs-api.so。batch_sort.h是示例共用的数据布局、排序、大根堆和平均值累加实现。

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

task[i]是该分支的小型输入输出结构：workers=4指定本例的工作区分摊，state首次为0。sink首次创建自己的workmem状态，把64位句柄写回task[i].state；后续batch直接复用该AFDM。Guest不能解引用句柄，也不依赖HYBRID batch的单个CompValue取得四个结果。每个sink生成16个4MiB run，四个状态独立，cparam2仍留给SNIA重入I/O。

全部输入完成后，Guest读取四个task，只把句柄写到group.states[0..3]。第二条source以cparam1接收state0，首次调用将其余三组run的描述接入第一个状态，只复制描述，不复制run数据。每个run保留原来的workmem context归属；source从所有64个run产生一条全局降序流。group中其余三个句柄清零，表示它们的资源已交由source统一管理。

第二条每次输出最多4MiB，AVG消费每批数据，直至source FREE全部spill token并结束四个context。最后一批finished=1、group.states[0]=0；应用结束循环。没有增加Gather Merge、create、combine或abort CSF，也未实现并行归并。

## 两路 K-Way Merge Path 对照

保留的设备实现为 `code/csd-programs/examples/merge-sort.cpp`，产物 `merge-sort.so`；普通主机基线为 `tests/read_merge_sort_avg.c`，产物 `read-merge-sort-avg`。新实现分别是 `merge-path-sort.cpp` / `merge-path-sort.so` 和 `tests/read_merge_path_sort_avg.c` / `read-merge-path-sort-avg`。两个算子文件独立，仍各自只有sort_sink、sort_source两个入口。

两种方式都先用四个sink生成run。默认示例采用merge-sort；增加 `--merge-path` 选择两路归并：

```sh
sudo /tmp/snia-cs-api-guest/cs-batch-sort-avg /dev/nvme0n1 /data/cs-batch-input.bin --existing --merge-path
sudo /tmp/csd-sort-bench/examples/read-merge-path-sort-avg /data/cs-batch-input.bin
bash tests/snia-cs-api-test.sh bench
```

bench现在交替比较两种算法的CPU/CSD各3轮，按算法分别检查两条路径的spill工作量一致。全部输出的sum/count/average必须一致，内存上界都不得超过64MiB。

新版本第二条pipeline如下；source 0先执行一个普通source→AVG batch，完成准备、分区并输出第一块，随后两条分支并行：

```text
R0～R3的全部run → 准备两份读缓冲 → 计算全局输出第N/2项的边界
                                         ↓
          source(part=0，输出[0,N/2)) → AVG(result[0])
          source(part=1，输出[N/2,N)) → AVG(result[1])
                                         ↓
                         主机相加两个sum/count，计算最终均值
```

分区计算采用[DuckDB介绍的K-Way Merge Path](https://duckdb.org/2025/09/24/sorting-again)：按剩余输出条数计算探测步长，读取各run的候选值，推进可以确定属于前半段的run前缀。不是固定按某个值分组。相同值按run编号确定先后，得到精确的N/2条记录，支持重复值和倾斜数据。主机与设备共用 `merge_path_sort.h` 中的分区计算，文件和NVMe I/O各自实现。随机探测命中spill run时仍通过NEED_LOAD重入读取，边界计算的额外I/O没有排除在计时之外。

sink的三个MR仍为input、task、io。两路source的四个MR为sorted、共享group、各自的merge_path_task、各自的io；cparam1是group首句柄，cparam2仍保留给重入。merge_path_task.part为0或1，其余字段输出准备耗时、分区耗时和分区读取字节数。共享group只保存状态句柄，不保存算法运行工作区。

两份source状态都在workmem中，各有游标、堆、每个spill run的256KiB读缓冲。并行读取前完成所有必要分配和spill；之后run内容及token只读。先到EOF的source先复制自己的结果和计时，再发布完成标志，不再访问共享状态；最后到EOF的source负责FREE全部token并销毁四个context。不会在一个分区完成时释放另一个分区仍需要的数据。

新版本预留全部AFDM缓冲26,259,456字节，工作区40,849,408字节，仍在64MiB内，工作区由四个sink平均分摊。第二阶段两个读缓存合计最多32MiB；内存压力可能导致更多驻留run被spill，准备阶段会如实统计。CPU使用同样配额和读缓冲，先准备并计算边界、输出part 0第一块，再启动两个归并线程。PROFILE新增prepare和partition的实际墙钟时间，partition另报read_bytes；source_work包含其中的计算，不能与这些阶段重复相加。

2026-10-21，同一256MiB文件、64MiB算法预算，四路sink保持不变，交替测量各3轮，平均延迟如下（秒）：

| 路径 | 归并方式 | 阶段1 | 阶段2（含准备/边界/AVG/回收） | 总延迟 |
| --- | --- | ---: | ---: | ---: |
| CPU | merge-sort，单路 | 2.646397 | 4.819976 | 7.466374 |
| CPU | Merge Path，两路 | 2.587540 | 3.143420 | 5.730961 |
| CSD | merge-sort，单路 | 2.519082 | 5.931433 | 8.450514 |
| CSD | Merge Path，两路 | 2.488781 | 4.807581 | 7.296363 |

CPU总延迟降低23.2%，整体加速1.303倍；CSD总延迟降低13.7%，整体加速1.158倍。第二阶段加速分别1.533/1.234倍。这里用同批次重测的merge-sort比较，没有用前一阶段的历史结果替代基线。

全部12次输出均一致：count=67108864，sum=15688155813636，average=233771.73861318827。merge-sort为60个spill run，写入/回读各240MiB；两路版本为64个spill run，写入256MiB，回读569.5MiB，其中297.75MiB用于边界探测。其余271.75MiB是两路流式归并读取：边界所在块可能被两路各读一次。CPU/CSD同算法的这些字节数完全一致。

两路版本的内存峰值上界CPU66,133,632字节、CSD66,165,632字节，均小于67,108,864字节。准备平均CPU0.102秒、CSD0.070秒；边界计算平均CPU0.263秒、CSD0.526秒，均已计入阶段2。并行归并降低了延迟，但更多缓存占用、边界探测、重复边界块读取和请求重入抵消了部分收益，不能期待第二阶段直接减半。

原始每轮日志、timings.tsv和profiles.tsv：`/tmp/snia-merge-compare.0kvahH`。设备算子在宿主CU线程运行，CPU基线在Guest运行，比较只代表当前模拟器配置。

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

普通路径用4个pthread，各自O_DIRECT读取交错分配的16个输入块、排序并按需spill；临时文件在输入目录创建后立即unlink，也使用O_DIRECT，原子分配互不重叠的写入区段。所有线程join后，主线程接入四组run并完成全局归并和AVG。CSD临时块位于femu-spill分区，通过内部D0/D1移动。

出错时，Guest先读取group或task中仍然有效的句柄，再逐个调用source的单AFDM用法释放工作内存。source已接管四组run时，只清理state0；随后关闭设备，回收尚存的spill token。原来的双MR单sink ABI仍供原生及SNIA回归使用，不影响本例的三MR并行方式。

## extent与请求构造

文件生成并fsync后，用FIEMAP查询物理extent。其地址相对于数据分区，因此LOAD地址需要加sysfs查询到的分区起始LBA；每个逻辑4MiB范围内的一个或多个extent按顺序转为CSBlockRange。原始正文直接从NVM载入input，不经过Guest中转；不支持空洞、未写入或不对齐的extent。

CsComputeRequest描述计算，CsBatchRequest是batch节点外层，ReqType区分计算和存储。main复用一份LOAD描述、一份计算描述，为四条分支分别配置快照；build_compute_request仅填写内部计算请求。csConfigureBatchEntry保存描述副本并保留资源引用，不复制AFDM数据，因此先释放batch，再释放其AFDM。

## 计时与性能测试

TIMING两阶段依次为“读取、生成run及必要spill”和“完整归并、AVG及回收”。排除文件生成/打开、extent查询、程序下载/激活、初始AFDM及工作池初始化；运行中的内存分配、I/O、重入、完整计算和临时数据回收均计时。

PROFILE区分sink_work、source_work与average_work，报告墙钟及线程CPU时间；run_sort_s是sink_work的子集，不能重复相加。设备统计各次CSF入口的执行，普通路径从阶段耗时扣除同步文件I/O和AVG时间得到相同口径的source_work。

bench启用SNIA_FEMU_PROFILE=1，Guest插件在设备关闭时向stderr输出E1、D0、D1、TRIM、F2次数及累计ioctl等待。E1包含三个CSF的执行，不能再与算子时间相加；D0包含文件LOAD和spill LOAD。HYBRID中各分支时间存在重叠；sink_work、run_sort_s及ioctl墙钟均是各次调用耗时之和，不能当作第一阶段的实际延迟。第一阶段延迟以TIMING为准；sink线程CPU累计时间与阶段墙钟之比可以观察并行利用率。

原始日志、timings.tsv和profiles.tsv写入脚本打印的/tmp/snia-sort-avg.*。CPU/CSD是平均总延迟之比，大于1表示CSD更快。设备算子实际运行在宿主CU线程，CPU基线在Guest运行，结果不能直接代表真实SSD计算核性能。此前sort→LIMIT的数据保留在[开发记录](../doc/development.md)，不作为当前sort→AVG的结果。


2026-10-21，同一Guest（8vCPU、8GiB）、同一256MiB文件（3个extent），主机4线程和CSD 4CU各交替运行3轮。下表为平均延迟，单位秒：

| 路径 | 第一阶段：读取/并行生成run | 第二阶段：单路归并/AVG/回收 | 总延迟 |
| --- | ---: | ---: | ---: |
| 主机4线程 | 2.549060 | 4.928473 | 7.477533 |
| CSD 4CU | 2.527259 | 5.485313 | 8.012572 |

三轮总延迟：主机7.552882、7.449779、7.429938秒；CSD 7.849219、7.782363、8.406133秒。六次输出均为count=67108864、sum=15688155813636、average=233771.73861318827。两边均64个run、60个spill run，写入/回读各240MiB；内存上界主机66,104,320字节、CSD 66,135,168字节，均低于64MiB。

还从b4bbe5a51构建了临时的单线程应用，在同一文件、同一FEMU环境交替运行各3轮；CSD通过仍支持的双MR方式调用当前sort SO。主机单线程平均13.651499秒，CSD单路平均14.328387秒，第一阶段分别8.584020/8.714043秒。四路改造的整体加速分别1.826/1.788倍，第一阶段分别3.368/3.448倍。单路缓冲更少，spill为54个run；这反映的是完整改造效果，包含并发缓冲和内存分摊的影响。

当前CSD总延迟比主机四线程高约7.2%。第二阶段source执行本身平均主机4.688秒、CSD 4.735秒，接近；阶段总延迟相差约0.557秒，其余差距主要落在I/O、重入、请求调度和临时块回收等路径。累计sink线程CPU时间主机8.252秒、CSD 8.175秒，而阶段墙钟约2.55/2.53秒，说明并行已实际生效。此结论不代表真实SSD计算核的相对性能。

原始并行日志及TSV：/tmp/snia-sort-avg.Yv1VKK；单线程对照：/tmp/sort-serial-control.gWE4Mj。单线程临时程序来自上述提交，当前默认示例及bench均为四路sink。

以下为改造前的单线程历史记录，当前默认运行四路sink。2026-10-20，同一Guest（8vCPU、8GiB）、2个extent的256MiB文件，64MiB算法预算、单线程/SERIAL模式实测：

| 轮次 | 普通路径（秒） | CSD（秒） |
| --- | ---: | ---: |
| 1 | 13.827569 | 13.388101 |
| 2 | 13.538000 | 13.345029 |
| 3 | 13.739177 | 13.465496 |
| 平均 | 13.701582 | 13.399542 |

六次结果及生成时的独立参考一致：count=67108864，sum=15688155813636，average=233771.73861318827。两边均64个run、累计spill 54个，写入/回读各216MiB，算法内存峰值约63.77MiB以内。当前第二条pipeline完整消费所有排序输出。

普通路径读取/生成run阶段平均8.780秒，归并/AVG/回收4.922秒；CSD分别8.383/5.016秒。AVG自身平均普通路径0.055秒、CSD 0.059秒。CSD run排序平均7.774秒，source执行4.550秒，内部D0/D1/TRIM累计0.239/0.229/0.115秒。原始日志及两份TSV位于/tmp/snia-sort-avg.GQq8lV。
