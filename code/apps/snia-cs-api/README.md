# SNIA Computational Storage API

应用只需 `#include "cs.h"` 并链接 `libsnia-cs-api.so`。`cs.h` 包含全部类型、状态和 33 个 API 声明，不依赖其他项目头文件。当前提供 FEMU 设备插件，公共库不包含 NVMe/FEMU 适配代码。

## 一、项目：编译、安装与执行位置

| 目录 | 内容与执行位置 |
| --- | --- |
| `include/cs.h` | 应用唯一入口，C/C++ 均可包含 |
| `src/core/` | 九个 SNIA 分类 .cpp，API 与所属实现放在同一文件 |
| `src/common/cs_handle_registry.cpp` | 共用句柄、类型检查与资源生命周期 |
| `src/common/cs_request_execution.cpp` | 共用请求快照、资源绑定、同步/异步执行 |
| `src/common/cs_common.hpp` | 私有资源类和共用声明 |
| `src/interface/cs_backend_interface.h` | 插件边界的 C 操作表，标准方法与 SNIA API 对应 |
| `src/plugins/femu/` | Guest FEMU 适配器；各 femu_*.c 按功能命名 |
| `src/plugins/femu/include/` | FEMU 设备算子的原生参数及重入辅助头 |
| `tests/common/`、`tests/femu/` | 按 SNIA 功能命名的测试，见 tests/README.md |
| `examples/` | 256MiB 文件排序取前5的完整 batch 应用 |
| `doc/` | 架构、传输、兼容性和开发记录 |
| `../../csd-programs/examples/` | 设备算子；容器内编译，在 FEMU CU 线程执行，逻辑上属于 CSD CPU |
| `../../csd-programs/csd-workmem/` | 设备算子的共享私有工作内存分配器 |

依赖为 C 编译器、支持 C++17 的 C++ 编译器、CMake 3.16 及以上。Ubuntu 包管理器的 CMake 足够。容器与 Guest 是两套系统，容器安装软件不会同时安装到 Guest；首次镜像创建的包列表在 docker-compose.yml 的 command 中配置。

Guest 中编译和安装：

```sh
cd /home/femu/apps/snia-cs-api
cmake -S . -B /tmp/snia-cs-api-guest -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/snia-cs-api-guest -j2
sudo cmake --install /tmp/snia-cs-api-guest --prefix /usr/local
sudo ldconfig
```

构建产物为公共动态库 `libsnia-cs-api.so` 和 `snia-cs-api/libcs-femu.so`。安装后对应 `lib/libsnia-cs-api.so`、`lib/snia-cs-api/libcs-femu.so`（部分平台 lib 为 lib64）。公共库根据自身实际路径扫描旁边的 `snia-cs-api/*.so`，通过 `dlopen` 加载插件，无需应用选择 FEMU 或链接插件。移动构建产物时保留这个相对位置。

在宿主项目根目录运行已有测试入口：

```sh
# 宿主编译算子及 mock 测试
bash tests/snia-cs-api-test.sh unit
# 容器编译算子，Guest 编译公共库与 FEMU 插件
bash tests/snia-cs-api-test.sh build
# Guest 真实设备测试，需已准备 femu-spill 分区
bash tests/snia-cs-api-test.sh smoke
# 完整文件排序示例
bash tests/snia-cs-api-test.sh example
```

也可在项目目录手动运行 `cmake -S . -B build && cmake --build build -j2 && ctest --test-dir build --output-on-failure`；FEMU mock 测试加载的设备算子须事先编译。设备 `.so` 仍输出到 `code/csd-programs/build/`，与 Guest 插件是不同产物。

加入其他 CSx 设备时，在 `src/plugins/` 新增一个适配器并编译为独立动态库，在其 CMakeLists.txt 中定义构建和安装规则，由项目根 CMakeLists.txt 加入该目录，实现 `src/interface/cs_backend_interface.h` 的设备操作，导出 `cs_backend_create` 和 `cs_backend_destroy` 两个 C 工厂符号。插件可以内部使用 C++，跨动态库边界仍使用 C 接口。每个适配器自行识别设备，公共层不判断设备类型。公共实现使用 C++17，内部资源通过虚析构、final 具体类和智能指针管理；插件边界继续使用 C 函数指针表。完整职责与调用路径见 [架构说明](doc/architecture.md)。

新插件可安装到默认插件目录，也可用 `csRegisterPlugin` 显式加载绝对路径；`csDeregisterPlugin` 在仍有设备或请求引用插件时返回 `CS_HANDLE_IN_USE`。这个 C 工厂契约是本项目的适配器接口，不是 SNIA 规定的通用插件二进制格式。

FEMU 插件用 Identify 校验 CSD 身份及 namespace 格式，用 GET_INFO 查询 FDM/CU，无需手工配置。公共 batch 使用插件报告的并发数调度：SERIAL 单线程、PARALLEL 独立节点并行、HYBRID 等全部前驱完成。FEMU 不接收整张图，硬件 batch 标志仍为 0。

## 二、上层应用：参数按算子约定填写

```c
#include "cs.h"
```

安装后编译：

```sh
cc application.c -lsnia-cs-api -o application
```

未安装时，在项目目录使用动态库：

```sh
cc application.c -Iinclude -Lbuild -lsnia-cs-api \
    -Wl,-rpath,"$PWD/build" -o application
```

完整应用见 [文件排序示例](examples/README.md)：创建256MiB随机整数文件，按 FIEMAP extent 提交内部 LOAD，以可重入 sort.sink 生成 spill run，再运行 sort.source → limit，输出全局最大的5个数并核对参考值。

以下参数映射、程序下载和 spill 约定均属于 FEMU 插件；其他插件按其设备能力实现。

FEMU 的 CSF 下载描述为 `文件名\0入口符号\0`，例如 `cs-example-functions.so\0cs_scale\0`。文件在容器的 `csd_program_dir` 中，Guest 不需要持有设备 `.so`。

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

这些设备侧头文件位于 `src/plugins/femu/include/`，安装到 `include/snia-cs-api/femu/`，不属于应用的 `cs.h`。普通算子包含 `femu-csd-kernel.h`，直接读取 `femu_csd_args`。支持重入时再包含 `cs_csf.h`。这两个头不要求算子链接 Guest 的 `libsnia-cs-api.so`。

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
3. Guest 中的 FEMU 插件分配 spill LBA，通过 D1 在设备内部写入，保留 `spill_id → LBA/长度` 的映射。
4. FEMU 插件再次调用相同 CSF，保留输入参数，把 `spill_id` 放入 `cparam2`。
5. 此时 `csIsReentry(args)` 为真，`csReentryValue(args)` 返回该标识。算子把它和已知长度加入自己的 run 列表，再继续或完成。

sort.source 也在入口做相同判断：首次根据自己的 run 游标返回 `csNeedLoad(run.spill_id)`；重入时 I/O AFDM 已有数据，消费数据并推进游标。需要释放 run 时返回 `csNeedFree(spill_id)`，重入后更新自己的记录。算子自己的 pending 状态区分刚完成的是哪项操作，不需要库另传事件配置。

| 算子返回 | FEMU 插件执行 | 再次调用 CSF 时 |
| --- | --- | --- |
| `csNeedStore(bytes)` | 分配区段，I/O AFDM → NVM | `cparam2=新 spill_id` |
| `csNeedLoad(spill_id)` | NVM → I/O AFDM | `cparam2=同一 spill_id` |
| `csNeedFree(spill_id)` | DSM/TRIM 后释放区段 | `cparam2=同一 spill_id` |
| 普通结果，如 `0` 或行数 | 完成当前逻辑节点 | 不重入 |

这些请求通过标准 `CompValue` 返回，不新增 `CS_STATUS`。仅用一个 64 位值的高 4 位表示 STORE/LOAD/FREE、低 60 位表示长度或标识；这三个结果范围供 batch I/O 请求使用，没有版本字段、magic 或 128 位控制封装。负的原生返回值仍表示执行错误。

FEMU 插件在 batch 节点内自动处理整个循环，直到得到普通结果才启动后继或发布 callback/event。单独 `csQueueComputeRequest` 执行一轮并返回原始 `CompValue`。

设备编译可参考 `code/csd-programs/examples/CMakeLists.txt`；根 CMakeLists.txt 的 add_csd_program 统一输出位置、头文件和 workmem 链接。完整可执行重入示例见 [sort.cpp](../../csd-programs/examples/sort.cpp)。

## spill 区与资源生命周期

Guest 先执行 `/home/femu/scripts/mount-nvme.sh`：模拟盘的 80% 格式化挂载到 `/data`，20% 作为不格式化、不挂载的 spill 区。首次分区会清除模拟盘。FEMU 插件按 GPT PARTLABEL=`femu-spill` 识别专区，不依赖 p1/p2 编号。

STORE/LOAD 数据走 D1/D0，不经过 Guest。I/O AFDM 必须容纳按 LBA 向上取整后的长度，算子应初始化最后的填充字节。长度本来就由算子掌握，不在重入时重复传递。

成功 batch 的 spill_id 可供后续 batch 使用；显式 FREE 或最后一个参与池的设备句柄关闭时回收。失败 batch 回收本批新分配的区段，之前成功 batch 的区段保留；算子私有状态由应用通过设备程序自己的清理入口销毁。标识仅在当前进程和 namespace 生命周期内有效，没有崩溃恢复。

真实 Guest 测试：`sudo build/cs-batch-scheduling-live /dev/nvme0n1`。测试预留程序索引 401–407、410–416；文件排序示例预留420–425。库不能枚举其他进程已加载的程序，应用需避免索引冲突。
