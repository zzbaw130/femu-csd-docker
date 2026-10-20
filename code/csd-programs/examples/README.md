# FEMU 设备算子示例

这里的 SO 在容器内编译，在 FEMU CU 线程执行。Guest 应用通过 NVMe 或 SNIA API 调用，设备源码不在 Guest 编译。

| 源码 | SO / 用途 |
| --- | --- |
| csd-vadd.c | csd-vadd.so，原生向量加法 |
| csd-original-kernels.cc | csd-original-kernels.so，原生 knn/sql/grep/lz4 |
| csd-vadd.bpf.c | 可选 BPF 对象 |
| femu-csd-kernel.h | 原生参数定义，保留原有样例 |
| merge-sort.cpp | merge-sort.so，四路sink、单路全局归并 |
| merge-path-sort.cpp | merge-path-sort.so，四路sink、两路K-Way Merge Path |
| avg.c | cs-example-avg.so，分批累计总和与计数，输出平均值 |
| tests/sort_spill_merge.cpp | 内存驻留、按需 spill、完整归并与自动释放测试 |
| tests/merge_path_sort.cpp | 精确分区、重复/倾斜/极值、两路归并和并发回收测试 |
| tests/read_merge_path_sort_avg.c | 独立主机两路Merge Path对照，64MiB总算法预算 |
| tests/read_merge_sort_avg.c | 独立 Guest CPU 性能基线，相同排序/归并逻辑、64MiB总算法预算，普通文件直接I/O和临时文件 |

使用 csd-programs 根 CMakeLists.txt 构建，所有 SO 进入 build。每个 native SO 统一动态链接 libcs-workmem.so；[内存使用规范](../README.md#统一内存规范)同样适用于示例。每个算子的实现放在自己的 .c/.cpp 中，不再单独维护无关的标量/scale/delay 演示函数。

## 排序的两个入口

Guest 应用是 [cs_batch_sort_avg.c](../../apps/snia-cs-api/examples/cs_batch_sort_avg.c)，参数布局在旁边的 batch_sort.h；完整 batch 及 AFDM 组织见[示例说明](../../apps/snia-cs-api/examples/README.md)。

sort_sink的并行用法为MR[0]=4MiB输入、MR[1]=sort_sink_task、MR[2]=256KiB spill I/O。Guest为每条分支设置workers=4、state=0；第一次执行时创建context，把句柄写回task.state。后续执行复用该task。四个分支使用同一CSF程序ID，各自保留run和重入状态。

所有AFDM分配大小都计入64MiB总算法预算，剩余工作区平均分给四个context；普通CPU基线采取相同配额。申请4MiB成功就将run留在内存，使用共用的原地quicksort降序排序。申请失败才spill本context内的一个驻留run；一个4MiB run保存为16个256KiB块，归并时按块读取。

sort_source的并行输入用法为MR[0]=sorted_chunk输出、MR[1]=包含四个句柄的sort_run_group、MR[2]=spill I/O，cparam1为第一个句柄。首次调用汇集四组run描述、接管全部context，并清零group中的其余句柄；run的数据及内存归属不变。source使用单个64槽大根堆全局归并，不并行归并。读缓冲不足时，spill同一context内的驻留run。

EOF时先FREE所有spill token，再结束四个context，将group首句柄清零，并在最后一个输出chunk设置finished=1。错误收尾仍使用同一个source，numr=1、cparam1为有效句柄；已接管其他状态的source会一起释放四个context，剩余token由设备关闭回收。无需独立create/abort入口。旧的双MR调用仍供单sink回归使用。

avg_int32的MR[0]接收sorted_chunk，MR[1]为初始清零、跨batch复用的average_result。每批累加int64总和与uint64计数，再计算double均值；finished跟随source EOF。结果与计时都保存在输出AFDM中，Guest无需维护批次总和。完整数据消费结束后才结束第二条pipeline。

## 原生命令示例

原生入口仍为 `int64_t csf(struct femu_csd_args *args)`，未增加 SQL/SNIA 参数。Guest 示例：

```sh
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 smoke-so csd-vadd.so
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 smoke-so-all csd-original-kernels.so
```

可选 BPF 用根构建目录的 csd-vadd-bpf 目标。原生 kernels 依赖 liblz4 开发包。

## 普通文件排序对比

read_merge_sort_avg.c 独立编译为 build/examples/read-merge-sort-avg，只链接同一个 workmem 分配器，不调用设备接口。输入、输出和 I/O 缓冲加 workmem 的峰值不超过64MiB；输入 O_DIRECT 读取，临时文件在输入所在目录创建并 unlink，也用 O_DIRECT 读写。两边第一阶段均四路并行，第二阶段单路归并；同样生成64个4MiB run，优先驻留、申请失败才 spill，归并每个 spill run 缓存256KiB，共用同一个大根堆实现，完整排序输出均送入AVG，直到EOF后得到全表均值。由宿主 tests/snia-cs-api-test.sh bench 传到 Guest 运行并检查 spill 工作量一致。

并行sink的墙钟统计是各次调用之和，不能与阶段延迟等同。示例统计sink/source/AVG的累计墙钟和线程CPU时间，以及run排序墙钟时间，经最后一个输出chunk传给Guest。统计使用普通时钟，不修改FEMU命令或runtime。source的统计包含各次入口执行，LOAD/STORE/TRIM等待则由Guest插件SNIA_FEMU_PROFILE=1记录；两种计时的包含关系见Guest示例说明。

两路版本的source接收output、共享group、分区task、io四个MR，先由part 0准备缓冲和精确边界，再由主机构建两个source→AVG分支。分区、额外spill和重复读取边界块均计时；具体布局、预算及比较命令见[Guest示例说明](../../apps/snia-cs-api/examples/README.md#两路-k-way-merge-path-对照)。
