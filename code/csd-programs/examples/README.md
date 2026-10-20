# FEMU 设备算子示例

这里的 SO 在容器内编译，在 FEMU CU 线程执行。Guest 应用通过 NVMe 或 SNIA API 调用，设备源码不在 Guest 编译。

| 源码 | SO / 用途 |
| --- | --- |
| csd-vadd.c | csd-vadd.so，原生向量加法 |
| csd-original-kernels.cc | csd-original-kernels.so，原生 knn/sql/grep/lz4 |
| csd-vadd.bpf.c | 可选 BPF 对象 |
| femu-csd-kernel.h | 原生参数定义，保留原有样例 |
| sort.cpp | cs-example-sort.so，只有 sort_sink、sort_source 两个入口 |
| limit.c | cs-example-limit.so，从全局降序流保留前5 |
| tests/sort_spill_merge.cpp | 内存驻留、按需 spill、完整归并与自动释放测试 |
| tests/read_sort_limit.c | 独立 Guest CPU 性能基线，相同排序/归并逻辑、64MiB总算法预算，普通文件直接I/O和临时文件 |

使用 csd-programs 根 CMakeLists.txt 构建，所有 SO 进入 build。每个 native SO 统一动态链接 libcs-workmem.so；[内存使用规范](../README.md#统一内存规范)同样适用于示例。每个算子的实现放在自己的 .c/.cpp 中，不再单独维护无关的标量/scale/delay 演示函数。

## 排序的两个入口

Guest 应用是 [cs_batch_sort_limit.c](../../apps/snia-cs-api/examples/cs_batch_sort_limit.c)，参数布局在旁边的 batch_sort.h；完整 batch 及 AFDM 组织见[示例说明](../../apps/snia-cs-api/examples/README.md)。

sort_sink 首次接收 cparam1=0，创建 workmem context 和排序状态。为公平比较，示例从64MiB总算法预算扣除四个 AFDM 缓冲，剩余约55.75MiB限制该 context；设备共享 workmem 全局容量不变。随后申请4MiB，复制输入并使用与 C 基线相同的原地 quicksort 降序排序。只要申请成功，整个 run 就保留在 workmem。入口返回状态的64位标识，Guest 从第一次 batch 的 CompValue 取得它，随后原样传入 cparam1。

后续 run 申请4MiB失败时，选择一个驻留 run，将它完整 spill；STORE 完成并重入后记录 token，腾出的 workmem 再用于新 run。一个4MiB run 以16个256KiB块保存，合计仍是4MiB。小块用于归并时按需读取 run 的下一段，避免每个 spill run 都要占4MiB读缓冲。

sort_source 直接访问同一状态中的全部 run，驻留 run 直接读，spill run 申请256KiB读缓冲；读缓冲申请失败也会先 spill 一个驻留 run 腾出空间。它以固定64槽大根堆缓存run头部，每次推进堆顶对应run；重入后先完成待更新的堆顶，再按全局降序输出，每个 chunk 最多4MiB。最后一个 chunk 生成后自动 FREE 所有 spill token、结束 context，并把输出 finished 置1；调用者不能再使用这个状态标识。

正常 source 的 MR[0] 为输出，MR[1] 为 spill I/O。提前停止或出错时，可调用同一个 source，仅提供 I/O AFDM（numr=1），释放私有状态；随后关闭设备句柄回收尚存的临时 token。这是同一入口的收尾用法，不需要额外 create/destroy/abort CSF。

limit_five 的 MR[0] 接收 sorted_chunk，MR[1] 输出 top_five；它在连续 chunk 之间保留最早的5个值，并传递 finished。示例会把 source 读完以展示 EOF 自动销毁，后面的数据不会覆盖已经选出的前5。

## 原生命令示例

原生入口仍为 `int64_t csf(struct femu_csd_args *args)`，未增加 SQL/SNIA 参数。Guest 示例：

```sh
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 smoke-so csd-vadd.so
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 smoke-so-all csd-original-kernels.so
```

可选 BPF 用根构建目录的 csd-vadd-bpf 目标。原生 kernels 依赖 liblz4 开发包。

## 普通文件排序对比

read_sort_limit.c 独立编译为 build/examples/read-sort-limit，只链接同一个 workmem 分配器，不调用设备接口。输入、输出和 I/O 缓冲加 workmem 的峰值不超过64MiB；输入 O_DIRECT 读取，临时文件在输入所在目录创建并 unlink，也用 O_DIRECT 读写。两边同样生成64个4MiB run，优先驻留、申请失败才 spill，归并每个 spill run 缓存256KiB，共用同一个大根堆实现，全部输出到 EOF 后取前5。由宿主 tests/snia-cs-api-test.sh bench 传到 Guest 运行并检查 spill 工作量一致。

示例统计sink/source的累计墙钟和线程CPU时间，以及run排序墙钟时间，经最后一个输出chunk传给Guest。统计使用普通时钟，不修改FEMU命令或runtime。source的统计包含各次入口执行，LOAD/STORE/TRIM等待则由Guest插件SNIA_FEMU_PROFILE=1记录；两种计时的包含关系见Guest示例说明。
