# FEMU 设备算子示例

这里的 SO 在容器内编译，在 FEMU CU 线程执行。Guest 应用通过 NVMe 或 SNIA API 调用，设备源码不在 Guest 编译。

| 源码 | SO / 用途 |
| --- | --- |
| csd-vadd.c | csd-vadd.so，原生向量加法 |
| csd-original-kernels.cc | csd-original-kernels.so，原生 knn/sql/grep/lz4 |
| csd-vadd.bpf.c | 可选 BPF 对象 |
| femu-csd-kernel.h | 原生参数定义，保留原有样例 |
| snia_compute_functions.c | cs-example-functions.so，SNIA 标量/AFDM/并行/spill 回归函数 |
| sort.cpp | cs-example-sort.so，可重入外部排序 |
| limit.c | cs-example-limit.so，从全局降序数据取前5 |
| tests/sort_spill_merge.cpp | 模拟批次内部 I/O，检查完整归并输出与资源释放 |

使用 csd-programs 根 CMakeLists.txt 构建，所有 SO 进入 build。每个 native SO 统一动态链接 libcs-workmem.so；[内存使用规范](../README.md#统一内存规范)同样适用于示例。

## 文件排序示例

Guest 应用是 [cs_batch_sort_limit.c](../../apps/snia-cs-api/examples/cs_batch_sort_limit.c)，具体参数约定位于它旁边的 batch_sort.h。调用及 AFDM 布局见[完整示例说明](../../apps/snia-cs-api/examples/README.md)。

sort.cpp 导出：

- sort_create：创建工作内存 context 和排序状态，返回64位标识。
- sort_sink：复制一个4MiB输入块至工作内存，原地降序排序；每256KiB返回一次 csNeedStore。重入后记录新 spill_id，再写下一块。
- sort_source：为每个 run 缓存一个256KiB块，通过 csNeedLoad 和重入填充缓冲；执行多路归并，输出一个全局降序 sorted_chunk。支持重复调用直到 count=0。
- sort_destroy：通过 csNeedFree 逐个释放 spill 块，最后结束工作内存 context。
- sort_abort：出错时只释放私有内存，剩余 spill 由 Guest 关闭设备句柄回收。

sort 使用64个 run、每 run 16个块，能处理256MiB输入。私有工作区约为4MiB排序缓冲、16MiB归并缓冲和少量状态；所有这些动态申请均经过 cs-workmem。std::sort 使用 GCC 的原地排序实现。AFDM 保存输入、输出和 spill I/O，不作为私有工作区。

limit_five 的 MR[0] 接收 sorted_chunk，MR[1] 输出 top_five。它只读取全局有序的首个 chunk，不对局部 top5 再聚合；因此排序阶段会读取整个文件，而归并阶段可在产出前5之后提前结束。

## 原生命令示例

原生入口仍为 `int64_t csf(struct femu_csd_args *args)`，未增加 SQL/SNIA 参数。Guest 示例：

```sh
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 smoke-so csd-vadd.so
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 smoke-so-all csd-original-kernels.so
```

可选 BPF 用根构建目录的 csd-vadd-bpf 目标。原生 kernels 依赖 liblz4 开发包。
