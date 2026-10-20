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

脚本编译两个 Guest 程序，交替运行3轮，并检查每次前5完全一致。CPU 基线是 tests/read_sort_limit.cpp：普通 read 读完整256MiB，用单线程 std::sort 全量降序排序，再取前5；每轮先用 POSIX_FADV_DONTNEED 请求丢弃该文件的 Guest 页缓存。CSD 用 --existing 复用同一个文件，不重新生成数据，继续执行现有64MiB workmem下的按需spill和完整归并。

计时排除文件生成和打开：CPU 从数组申请之后、第一次 read 之前开始；CSD 从 extent 查询、算子下载/激活及 AFDM/请求缓冲分配完成后开始，包含 batch 配置、内部 I/O、运行中的 workmem 申请、计算、重入、结果读取和 source 的 EOF 回收。日志中的 TIMING 行依次输出模式、第一阶段秒数、第二阶段秒数、总秒数：CPU 两阶段是 read 和 sort+limit，CSD 两阶段是 LOAD+sink（含排序与spill）和 source+limit（含归并与回收），阶段含义不同。

脚本输出3轮平均总耗时及 CPU/CSD 比值，大于1表示这次测量中CSD更快。原始日志和 timings.tsv 写入打印出的 /tmp/snia-sort-limit.* 目录。此测试比较当前两条实现路径：CPU 使用约256MiB排序数组，CSD workmem为64MiB并产生额外临时I/O；FEMU的设备算子实际运行在宿主线程，结果不能直接代表真实SSD的CPU性能。

2026-10-20 实测：Guest为8 vCPU、8GiB，CPU排序单线程，CSD batch使用SERIAL，FDM/workmem各64MiB；同一文件有3个extent，所有结果一致。

| 轮次 | CPU总耗时（秒） | CSD总耗时（秒） |
| --- | ---: | ---: |
| 1 | 10.447 | 61.348 |
| 2 | 10.120 | 61.576 |
| 3 | 10.005 | 61.105 |
| 平均 | 10.191 | 61.343 |

本次CSD耗时约为CPU的6.02倍。CPU平均read为0.580秒、sort+limit为9.611秒；CSD平均LOAD+sink为8.124秒、source+limit为53.219秒。后者含spill读取、完整归并及回收，占CSD总时间约87%。当前source每输出一个整数会遍历全部run，这与CPU一次性内存排序的算法和I/O开销不同；这些阶段计时尚不能单独量化比较、重入和内部I/O各自的占比。

## extent 与内部 LOAD

fsync 后用 FIEMAP 查询物理 extent。FIEMAP 地址相对于数据分区；LOAD 的 StartLba 相对于 namespace，所以要加分区起始位置。应用从 sysfs 读取分区起始扇区和 namespace id，从块设备读取逻辑块大小，检查文件位于指定设备上。

每次取文件逻辑范围中的4MiB，将其覆盖的一个或多个 extent 按逻辑顺序转为 CSBlockRange，放入同一 csQueueStorageRequest。内部 D0 连续写入 input AFDM，正文不经过 Guest 中转。空洞、未写入、编码或不对齐的 extent 被拒绝；文件保持打开并持有协作式独占锁。

## AFDM 与参数

| AFDM | 大小 | 使用方式 |
| --- | --- | --- |
| input | 4MiB | LOAD 写入，sink 的 MR[0] 读取 |
| io | 256KiB | sink/source 的 MR[1]，最后一个 AFDM；保存一个待 STORE 或刚 LOAD 的 spill 块 |
| sorted | 4MiB+8字节 | source 的 MR[0] 写 count、finished 和全局降序整数；limit 的 MR[0] 读 |
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

所有 run 数据、读缓冲和描述都由同一 workmem context 分配。内存充足时没有 spill；256MiB输入在64MiB工作池下会触发申请失败、淘汰和重入。没有额外的固定 spill 阈值或预留算法内存池。

source 每次最多输出4MiB，最后一批同时带 finished=1；即使最后一批恰好装满，也已经销毁状态，不能再调用一次探测 EOF。本示例把整个有序流读完，以展示正常结束自动回收；limit 始终保留首5，后续 chunk 不覆盖它们。

错误或提前停止时，通过 source 的收尾用法（仅传 io 和状态标识）释放私有内存，随后关闭设备句柄回收临时区。正常 EOF 则由 source 主动 FREE 后销毁。状态/token 不提供崩溃恢复。
