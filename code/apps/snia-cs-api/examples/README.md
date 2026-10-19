# 文件排序并取最大的5个整数

`cs_batch_sort_limit.c` 是 Guest 应用，只通过 `cs.h` 和 libsnia-cs-api.so 访问设备。`batch_sort.h` 定义本示例输入规模和输出数据布局，由应用与设备算子共同采用。设备 sort/limit 源码在 `code/csd-programs/examples`，在容器内编译。

## 编译运行

先准备已挂载的 femu-data 分区和未挂载的 femu-spill 分区；已有分区无需重建。宿主项目根目录：

```sh
bash tests/snia-cs-api-test.sh build
bash tests/snia-cs-api-test.sh example
```

也可在 Guest 独立编译（SDK 已安装时）：

```sh
cc cs_batch_sort_limit.c -O2 -lsnia-cs-api -o cs-batch-sort-limit
sudo ./cs-batch-sort-limit /dev/nvme0n1 /data/cs-batch-input.bin
```

应用会创建或覆盖指定文件，写入256MiB伪随机 int32，包括正数和负数；保留文件供查看。生成时独立计算参考前5，最终逐项核对 CSD 输出。固定程序索引420–425，运行期间不要让其他程序复用这些索引。

## 文件 extent 到 LOAD

应用用 fsync 将数据落盘，再用 FIEMAP 查询文件的物理 extent。FIEMAP 地址相对于文件系统所在分区；csQueueStorageRequest 的 StartLba 相对于原始 namespace，所以必须加上分区起始位置。应用从 sysfs 读取分区起始扇区和 namespace id，从块设备读取逻辑块大小，并检查文件确实位于该设备上。

每次取文件逻辑范围中的4MiB，将它覆盖的一个或多个 extent 转成 CSBlockRange，按文件逻辑顺序放入同一 LOAD 请求。内部 D0 会把这些范围连续放进输入 AFDM。未分配、未写入、编码或不对齐的 extent 被拒绝；示例不修改文件布局，也不把文件正文重新读回 Guest。文件保持打开并持有协作式独占锁，实验期间不能写入或移动它。

## AFDM 与算子参数

| 名称 | 大小 | 谁写入、谁读取 |
| --- | --- | --- |
| input | 4MiB | LOAD 写入，sort_sink 的 MR[0] 读取 |
| io | 256KiB | sort_sink 的 MR[1] 写待 spill 块；sort_source 的 MR[1] 接收 LOAD 块；始终是重入请求的最后一个 AFDM |
| sorted | sizeof(sorted_chunk)，约4KiB | sort_source 的 MR[0] 写全局降序输出，limit_five 的 MR[0] 读取 |
| result | sizeof(top_five)，24字节 | limit_five 的 MR[1] 写，Guest 最后读回 |

sort_create 返回排序私有状态的64位标识。后续 sort 入口以 cparam1 接收它，cparam2 保留给库填写重入完成的 spill_id。标识指向设备工作内存对象，Guest 不能解引用。limit 没有私有状态。

## 两个阶段的 batch

排序必须先看到全部输入，才能确认全局最大值来自哪个 run。因此实际组织为：

```text
阶段一，同一 batch 配置重复64次，仅更新 LOAD 的 extent：
  LOAD(input) -> sort_sink(input, io, sort_handle)
                    |
                    +-- NEED_STORE -> 内部 STORE -> 重入记录 token，直到4MiB全部写完

全部输入完成，释放阶段一 batch。

阶段二，一个新 batch：
  sort_source(sorted, io, sort_handle) -> limit_five(sorted, result)
      |
      +-- NEED_LOAD -> 内部 LOAD -> 重入归并，直到产出一个全局降序 chunk

Guest 读 result，检查前5；用单节点 batch 调 sort_destroy，逐个 FREE spill，释放私有状态。
```

这是 LOAD → sort → limit 的阻塞执行方式。不能在每个输入块的 sort_sink 后直接接 limit 并把局部输出当作全局结果。两个阶段均通过 csAllocBatchRequest、csConfigureBatchEntry 和 csQueueBatchRequest 提交，算子内部返回 NEED_* 后由 FEMU 插件处理 I/O 和重入，应用不手工推进这些步骤。

## 设备重入和内存

sort_sink 在第一次入口把输入复制到 workmem，原地降序排序成一个4MiB run，再逐个256KiB返回 csNeedStore。再次进入时用 csIsReentry 判断，csReentryValue 取得刚完成的 spill_id，记录后继续 STORE；本次 run 全部保存后才完成 batch 节点。

全部64个 run 共占256MiB spill，超过64MiB工作内存。sort_source 为每个 run 留一个256KiB缓存，通过 NEED_LOAD 请求下一块，执行多路归并。私有内存约20MiB加少量元数据；所有动态算法内存都用 cs_workmem 申请。输入和输出 AFDM 合计约4.26MiB。

sort_source 每次最多输出1024个全局有序整数，可以重复调用直到 count=0。本示例只需要前5，所以取首个输出 chunk 后结束归并。sort_destroy 仍会释放全部 spill token。错误路径调用 sort_abort 释放私有内存，并关闭设备句柄回收剩余临时区；临时 token 不提供崩溃恢复。
