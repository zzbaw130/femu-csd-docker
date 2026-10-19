# FEMU 的 SNIA Computational Storage API

本目录提供运行在 Linux Guest 的 SNIA API 库。应用包含 `cs.h` 或 `cs.sh` 并链接 `libsnia-cs-api.so`，由库提交 NVMe 命令。`cs.sh` 是 C 头文件。

## 一、项目：编译与执行位置

| 目录 | 内容与执行位置 |
| --- | --- |
| `include/` | 应用 API、原生 `femu-csd-kernel.h`、设备侧重入辅助函数 `cs_csf.h` |
| `src/` | Guest 应用进程中的 NVMe 传输、资源管理、batch、event/callback 和 spill 分配 |
| `examples/`、`tests/` | Guest 示例、mock 与真实 NVMe 测试 |
| `doc/` | 架构、传输、兼容性和开发记录 |
| `../../csd-programs/snia-cs-api/` | 设备算子；容器内编译，由 FEMU CU 线程执行，逻辑上属于 CSD CPU |
| `../../csd-programs/csd-workmem/` | 设备算子的共享私有工作内存分配器 |

Guest 中编译和安装：

```sh
cd /home/femu/apps/snia-cs-api
make all examples live
sudo make install PREFIX=/usr/local
sudo ldconfig
```

生成 `build/libsnia-cs-api.so` 和 `.a`。可以指定 `BUILD_DIR=/tmp/snia-cs-api-guest`。宿主项目根目录编译设备算子、运行 mock 测试：

```sh
docker compose exec -T femu-csd make -C /workspace/code/csd-programs snia-cs-scale.so
docker compose exec -T femu-csd make -C /workspace/code/apps/snia-cs-api BUILD_DIR=/tmp/snia-native-check check
```

设备 `.so` 输出到 `code/csd-programs/build/`。Guest 库在 Guest 编译，设备算子在容器编译。mock 测试加载真实设备 `.so`，应在相同容器环境运行。

库用 Identify 校验 FEMU CSD 身份、获取 namespace LBA 大小和容量，用通用 GET_INFO 查询 FDM/CU。无需手工配置 FDM 容量和 CU 数。FDM 容量用于属性报告；CU 数限制 batch 工作线程数，实际资源由设备限制。

batch 图在 Guest 库中调度；SERIAL 单线程，PARALLEL 并行独立节点，HYBRID 在全部前驱完成后启动节点。设备不接收整张图，硬件 batch 标志为 0。完整能力范围见 [兼容性说明](doc/compatibility.md)。

## 二、上层应用：参数按算子约定填写

```c
#include "cs.sh"
```

安装后编译：

```sh
cc application.c -lsnia-cs-api -pthread -o application
```

未安装时，在项目目录使用动态库：

```sh
cc application.c -Iinclude -Lbuild -lsnia-cs-api -pthread \
    -Wl,-rpath,"$PWD/build" -o application
```

`examples/api-smoke.c` 包含加载、激活、AFDM、batch 和回收的完整示例。Guest 中执行 `sudo build/api-smoke /dev/nvme0n1`，期望输出 `SNIA API batch PASS: 4 12 28 44`。

CSF 下载描述为 `文件名\0入口符号\0`，例如 `snia-cs-scale.so\0cs_scale\0`。文件在容器的 `csd_program_dir` 中，Guest 不需要持有设备 `.so`。

**应用和算子自行约定参数含义。库只映射原生字段，不附加参数描述或输入 AFDM。**

| 请求中的参数 | 算子收到的字段 |
| --- | --- |
| 第一个、第二个……AFDM | `mr_addr[0]`、`mr_addr[1]`……以及相应 `mr_len` |
| 第一个、第二个普通整数 | `cparam1`、`cparam2`；8/16/32 位值按无符号扩展 |

两个 `long long` 参数始终独立：第一个整数进入 `cparam1`，第二个进入 `cparam2`，应用按算子约定分别填写。当前实现只支持 8/16/32/64 位整数；标准枚举中的 128 位类型保留名称，但请求返回 `CS_UNSUPPORTED_TYPE`。

原生 `femu_csd_args` 还有 `data_buffer/buffer_len`，用于接收**执行命令中紧跟 MR 列表的附加输入字节**。例如原生命令的输入为 `[MR0][MR1][SortParams]`，那么 `data_buffer` 指向 `SortParams` 的设备端副本，`buffer_len` 是其长度。FEMU 在这次执行结束后释放副本；算子修改它不会自动写回 Guest。

**当前库尚未接通这条附加载荷路径，因此经本库调用时这两个字段为 NULL/0。** 这属于当前适配器的限制。SNIA 的 `CsComputeArg` 明确定义了标量和 AFDM 参数，没有直接对应这两个原生字段的主机字节数组成员；`CS_DESCRIPTOR_TYPE` 虽然列在枚举中，正文没有给出完整的 descriptor 数据结构，本库也未实现它。应用现在可以把自己定义的参数结构放入输入 AFDM，由算子直接按约定读取；不能把“当前未映射到原生附加载荷”说成 SNIA 禁止传递这类数据。

当前原生 MR 只表示整块 AFDM，计算请求的非零 `ByteOffset` 返回 `CS_UNSUPPORTED`。需要偏移时，应用按算子自己的接口把偏移放入标量或输入数据中，算子自行计算地址。超过两个 64 位标量位置也返回不支持；复杂配置直接作为应用定义的输入数据传入。

例如 `cs_scale` 约定两个 AFDM 为输入和输出，`cparam1` 为元素数，`cparam2` 为输入/输出偏移。普通从起始地址计算时，应用传两个 AFDM 和一个 count 即可，未传的 `cparam2` 为 0。

应用用 `csAllocBatchRequest` 创建一个 batch，再用 `csConfigureBatchEntry` 向其中加入 LOAD、计算等请求，并指定请求之间的先后依赖，最后调用 `csQueueBatchRequest` 提交。这里的 batch 是 Guest 库管理的一组请求及其依赖关系。

下面以向 batch 加入一个 sort.sink 请求为例。假定应用已分配 `input`、`io` 两块 AFDM，获得了算子函数句柄 `sort_sink`；设备端排序初始化函数已创建 `SortState` 并返回其标识 `sort_handle`。`sort_handle` 只标识这个排序状态对象，里面保存 run 列表、待写长度等内容，与 batch 是两个不同的对象。

重入算子仍使用普通 `CsComputeRequest`，约定保留 `cparam2`，应用只传至多一个标量输入。这个 sort.sink 的参数约定为：MR[0] 是待排序输入 `input`，MR[1] 是放置待写 run 的 `io`，cparam1 是 `sort_handle`。

```c
CsBatchRequest *r = calloc(1, sizeof(*r) + 2 * sizeof(CsComputeArg));
r->ReqType = CS_QUEUE_COMPUTE;
r->u.Compute.CSFId = sort_sink;
r->u.Compute.NumArgs = 3;
csHelperSetComputeArg(&r->u.Compute.Args[0], CS_AFDM_TYPE, input, 0UL);
csHelperSetComputeArg(&r->u.Compute.Args[1], CS_AFDM_TYPE, io, 0UL);
csHelperSetComputeArg(&r->u.Compute.Args[2], CS_64BIT_VALUE_TYPE, sort_handle);
CS_BATCH_HANDLE batch;
CS_BATCH_INDEX sort_entry;
csAllocBatchRequest(CS_BATCH_SERIAL, 1, &batch);
csConfigureBatchEntry(batch, CS_BATCH_ADD, r, 0, 0, &sort_entry);
free(r);
u64 result;
CS_STATUS status = csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, &result);
csFreeBatchRequest(batch);
```

当前重入实现从**当前这一个计算请求的参数**中取最后一个 AFDM，作为 STORE 的数据来源或 LOAD 的数据目的地。上例中它就是 `Args[1]` 的 `io`，设备端看到的是 `mr_addr[1]`；后面的 `Args[2]` 是整数，不参与这个选择。这是本项目的算子参数约定，不是整批任务的最后一块 AFDM，也不是 SNIA 规定的位置。

`csConfigureBatchEntry` 会复制 `r` 中的请求描述，记录它用到的算子和 AFDM 句柄，所以之后可以 `free(r)`；它不复制 AFDM 中的数据。只要 batch 还引用这些句柄，它们就不能释放，应先释放 batch 再释放 AFDM。示例为突出流程省略逐次错误处理，实际应用应检查各 API 返回的状态。

## 三、设备算子：在入口判断是否重入

普通算子包含 `femu-csd-kernel.h`，直接读取 `femu_csd_args`。支持重入时再包含 `cs_csf.h`。这两个头不要求算子链接 Guest 的 `libsnia-cs-api.so`。

所有设备算子统一链接 `libcs-workmem.so`。排序缓冲、run 列表等私有数据用 `cs_work_begin/cs_work_alloc/cs_work_end` 管理；context 可跨多次入口保留。AFDM 保存输入、输出和交接数据，私有算法状态留在工作内存中。

以 sort.sink 为例，下面是算法伪代码；`find_sort`、`sort_chunk` 等是算子自己的函数：

```c
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"

int64_t sort_sink(struct femu_csd_args *args)
{
    SortState *s = find_sort(args->cparam1);

    if (csIsReentry(args)) {
        /* 上次请求的 STORE 已完成；只更新记录，不重复处理输入。 */
        uint64_t spill_id = csReentryValue(args);
        append_run(s, spill_id, s->pending_bytes);
        s->pending_bytes = 0;
        return 0;  /* 本次输入处理完成。后续输入继续使用同一排序状态。 */
    }

    sort_chunk(s, args->mr_addr[0], args->mr_len[0]);
    if (need_spill(s)) {
        s->pending_bytes = write_sorted_run(s, args->mr_addr[1]);
        return csNeedStore(s->pending_bytes);
    }
    return 0;
}
```

实际执行顺序：

1. 首次入口 `cparam2=0`，`csIsReentry(args)` 为假。
2. 算子在声明的 I/O AFDM 中放入待写数据，返回 `csNeedStore(bytes)`。
3. SNIA 分配 spill LBA，通过 D1 在设备内部写入，保留 `spill_id → LBA/长度` 的映射。
4. SNIA 再次调用相同 CSF，保留输入参数，把 `spill_id` 放入 `cparam2`。
5. 此时 `csIsReentry(args)` 为真，`csReentryValue(args)` 返回该标识。算子把它和已知长度加入自己的 run 列表，再继续或完成。

sort.source 也在入口做相同判断：首次根据自己的 run 游标返回 `csNeedLoad(run.spill_id)`；重入时 I/O AFDM 已有数据，消费数据并推进游标。需要释放 run 时返回 `csNeedFree(spill_id)`，重入后更新自己的记录。算子自己的 pending 状态区分刚完成的是哪项操作，不需要库另传事件配置。

| 算子返回 | SNIA 执行 | 再次调用 CSF 时 |
| --- | --- | --- |
| `csNeedStore(bytes)` | 分配区段，I/O AFDM → NVM | `cparam2=新 spill_id` |
| `csNeedLoad(spill_id)` | NVM → I/O AFDM | `cparam2=同一 spill_id` |
| `csNeedFree(spill_id)` | DSM/TRIM 后释放区段 | `cparam2=同一 spill_id` |
| 普通结果，如 `0` 或行数 | 完成当前逻辑节点 | 不重入 |

这些请求通过标准 `CompValue` 返回，不新增 `CS_STATUS`。仅用一个 64 位值的高 4 位表示 STORE/LOAD/FREE、低 60 位表示长度或标识；这三个结果范围供 batch I/O 请求使用，没有版本字段、magic 或 128 位控制封装。负的原生返回值仍表示执行错误。

batch 自动处理整个循环，直到得到普通结果才启动后继或发布 callback/event。单独 `csQueueComputeRequest` 执行一轮并返回原始 `CompValue`。

设备编译可参考 `code/csd-programs/snia-cs-api/Makefile`；`../mk/csf.mk` 只是复用输出目录和链接选项的 Makefile 片段。完整可执行重入示例见 [scale.c](../../csd-programs/snia-cs-api/scale.c)。

## spill 区与资源生命周期

Guest 先执行 `/home/femu/scripts/mount-nvme.sh`：模拟盘的 80% 格式化挂载到 `/data`，20% 作为不格式化、不挂载的 spill 区。首次分区会清除模拟盘。库按 GPT PARTLABEL=`femu-spill` 识别专区，不依赖 p1/p2 编号。

STORE/LOAD 数据走 D1/D0，不经过 Guest。I/O AFDM 必须容纳按 LBA 向上取整后的长度，算子应初始化最后的填充字节。长度本来就由算子掌握，不在重入时重复传递。

成功 batch 的 spill_id 可供后续 batch 使用；显式 FREE 或最后一个参与池的设备句柄关闭时回收。失败 batch 回收本批新分配的区段，之前成功 batch 的区段保留；算子私有状态由应用通过设备程序自己的清理入口销毁。标识仅在当前进程和 namespace 生命周期内有效，没有崩溃恢复。

真实 Guest 测试：`sudo build/live-api /dev/nvme0n1`。测试预留程序索引 401–407、410–416；普通示例预留 400。库不能枚举其他进程已加载的程序，应用需避免索引冲突。
