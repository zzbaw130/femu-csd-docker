# SNIA Computational Storage API

应用只需 `#include "cs.h"` 并链接 `libsnia.so`。`cs.h` 包含全部类型、状态和 33 个 API 声明，不依赖其他项目头文件。当前提供 FEMU 设备插件，公共库不包含 NVMe/FEMU 适配代码。

## 一、项目：编译、安装与执行位置

| 目录 | 内容与执行位置 |
| --- | --- |
| `include/cs.h` | 应用唯一入口，C/C++ 均可包含 |
| `src/core/` | 九个 SNIA 分类 .cpp，API 与所属实现放在同一文件 |
| `src/core/internal/cs_handle_registry.cpp` | 内部句柄、类型检查与资源生命周期 |
| `src/core/internal/cs_request_execution.cpp` | 内部请求快照、资源绑定、同步/异步执行 |
| `src/core/internal/cs_internal.hpp` | 私有资源类和内部声明 |
| `src/interface/cs_backend_interface.h` | 插件边界的 C 操作表，标准方法与 SNIA API 对应 |
| `src/plugins/femu/` | Guest FEMU 适配器；各 femu_*.c 按功能命名 |
| `src/plugins/femu/internal/` | 插件内部实现，含传输、请求验证/执行、目录、资源查询和工厂入口 |
| `src/plugins/femu/include/` | FEMU 设备算子的原生参数及重入辅助头 |
| `tests/common/`、`tests/femu/` | 按 SNIA 功能命名的测试，见 tests/README.md |
| `examples/` | 256MiB 文件完整排序后分批计算平均值的完整 batch 应用 |
| `doc/` | 架构、传输、兼容性和开发记录 |
| `../../csd-programs/examples/` | 设备算子；容器内编译，在 FEMU CU 线程执行，逻辑上属于 CSD CPU |
| `../../csd-programs/csd-workmem/` | 设备算子的共享私有工作内存分配器 |

依赖为 C 编译器、支持 C++17 的 C++ 编译器、CMake 3.16 及以上。Ubuntu 包管理器的 CMake 足够。容器与 Guest 是两套系统，容器安装软件不会同时安装到 Guest；首次镜像创建的包列表在 docker-compose.yml 的 command 中配置。

Guest 启动后执行一次 `/home/femu/scripts/init-csd-env.sh`，即可准备数据/spill 分区并编译安装 SNIA 到 `/usr/local`。从宿主执行同一脚本：

```sh
./scripts/sh-guest.sh scripts/guest/init-csd-env.sh
```

首次运行或分区标签不匹配会重建分区并清除模拟盘数据；标签匹配时复用已有分区。脚本安装后应用可直接包含 `cs.h` 并使用 `-lsnia`。

仅需手动编译和安装 SNIA 时：

```sh
cd /home/femu/apps/snia
cmake -S . -B /tmp/snia-guest -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/snia-guest -j2
sudo cmake --install /tmp/snia-guest --prefix /usr/local
sudo ldconfig
```

构建产物为公共动态库 `libsnia.so` 和 `snia/libcs-femu.so`。安装后对应 `lib/libsnia.so`、`lib/snia/libcs-femu.so`（部分平台 lib 为 lib64）。公共库根据自身实际路径扫描旁边的 `snia/*.so`，通过 `dlopen` 加载插件，无需应用选择 FEMU 或链接插件。移动构建产物时保留这个相对位置。

在宿主项目根目录运行已有测试入口：

```sh
# 宿主编译算子及 mock 测试
bash tests/snia-test.sh unit
# 容器编译算子，Guest 编译公共库与 FEMU 插件
bash tests/snia-test.sh build
# Guest 真实设备测试，需新 FEMU 固件及隐藏临时空间
bash tests/snia-test.sh smoke
# 完整文件排序示例
bash tests/snia-test.sh example
```

也可在项目目录手动运行 `cmake -S . -B build && cmake --build build -j2 && ctest --test-dir build --output-on-failure`；FEMU mock 测试加载的设备算子须事先编译。设备 `.so` 仍输出到 `code/csd-programs/build/`，与 Guest 插件是不同产物。

加入其他 CSx 设备时，在 `src/plugins/` 新增一个适配器并编译为独立动态库，在其 CMakeLists.txt 中定义构建和安装规则，由项目根 CMakeLists.txt 加入该目录，实现 `src/interface/cs_backend_interface.h` 的设备操作，导出 `cs_backend_create` 和 `cs_backend_destroy` 两个 C 工厂符号。插件可以内部使用 C++，跨动态库边界仍使用 C 接口。每个适配器自行识别设备，公共层不判断设备类型。公共实现使用 C++17，内部资源通过虚析构、final 具体类和智能指针管理；插件边界继续使用 C 函数指针表。完整职责与调用路径见 [架构说明](doc/architecture.md)。

新插件可安装到默认插件目录，也可用 `csRegisterPlugin` 显式加载绝对路径；`csDeregisterPlugin` 在仍有设备或请求引用插件时返回 `CS_HANDLE_IN_USE`。这个 C 工厂契约是本项目的适配器接口，不是 SNIA 规定的通用插件二进制格式。

FEMU 插件用 Identify 校验 CSD 身份及 namespace 格式，用 GET_INFO 查询 FDM/CU，无需手工配置。仅含存储与计算的 batch 编码为一条 EXEC_GRAPH（E9）命令，由设备按依赖自主执行：SERIAL 逐节点执行，PARALLEL 独立节点就绪，HYBRID 等全部前驱完成。设备最多使用 nr_cu 个 CU，内部 LOAD/STORE 等待不占 CU。含主机内存复制的 batch 仍由公共层调度，其计算节点各提交一条设备图。应用继续使用原有 csQueueBatchRequest，无需区分这两条路径。

设备图是本项目的 FEMU 扩展，不新增公开能力查询标志。此版本 FEMU 插件要求同时更新设备固件；不探测旧固件再静默回退。私有插件操作表升级为 ABI 3，公共库与插件须一起重新编译；应用的 cs.h 与链接规则不变。

## 二、上层应用：参数按算子约定填写

```c
#include "cs.h"
```

安装后编译：

```sh
cc application.c -lsnia -o application
```

未安装时，在项目目录使用动态库：

```sh
cc application.c -Iinclude -Lbuild -lsnia \
    -Wl,-rpath,"$PWD/build" -o application
```

完整应用见 [文件排序示例](examples/README.md)：创建256MiB随机整数文件，按 FIEMAP extent 提交内部 LOAD，以可重入 sort.sink 生成 spill run，再运行 sort.source → avg，完整消费排序输出并核对总和、计数与平均值。

以下参数映射、程序下载和 spill 约定均属于 FEMU 插件；其他插件按其设备能力实现。

FEMU 的 CSF 下载描述为 `文件名\0入口符号\0`，例如 `merge-sort.so\0sort_sink\0`。文件在容器的 `csd_program_dir` 中，Guest 不需要持有设备 `.so`。

**应用和算子自行约定参数含义。库只映射原生字段，不附加参数描述或输入 AFDM。**

| 请求中的参数 | 算子收到的字段 |
| --- | --- |
| 第一个、第二个……AFDM | `mr_addr[0]`、`mr_addr[1]`……以及相应 `mr_len` |
| 第一个、第二个普通整数 | `cparam1`、`cparam2`；8/16/32 位值按无符号扩展 |

两个 `long long` 参数始终独立：第一个整数进入 `cparam1`，第二个进入 `cparam2`，应用按算子约定分别填写。当前实现只支持 8/16/32/64 位整数；标准枚举中的 128 位类型保留名称，但请求返回 `CS_UNSUPPORTED_TYPE`。

原生 `femu_csd_args` 还有 `data_buffer/buffer_len`，用于接收**执行命令中紧跟 MR 列表的附加输入字节**。例如原生命令的输入为 `[MR0][MR1][SortParams]`，那么 `data_buffer` 指向 `SortParams` 的设备端副本，`buffer_len` 是其长度。FEMU 在这次执行结束后释放副本；算子修改它不会自动写回 Guest。

**当前库尚未接通这条附加载荷路径，因此经本库调用时这两个字段为 NULL/0。** 这属于当前适配器的限制。SNIA 的 `CsComputeArg` 明确定义了标量和 AFDM 参数，没有直接对应这两个原生字段的主机字节数组成员；`CS_DESCRIPTOR_TYPE` 虽然列在枚举中，正文没有给出完整的 descriptor 数据结构，本库也未实现它。应用现在可以把自己定义的参数结构放入输入 AFDM，由算子直接按约定读取；不能把“当前未映射到原生附加载荷”说成 SNIA 禁止传递这类数据。

当前原生 MR 只表示整块 AFDM，计算请求的非零 `ByteOffset` 返回 `CS_UNSUPPORTED`。需要偏移时，应用按算子自己的接口把偏移放入标量或输入数据中，算子自行计算地址。超过两个 64 位标量位置也返回不支持；复杂配置直接作为应用定义的输入数据传入。

完整四路排序示例中，sort_sink 的 MR[0] 为4MiB输入、MR[1] 为分支状态 task、MR[2] 为256KiB spill I/O。task.state 首次为0，sink 创建状态并写回64位句柄，后续请求复用 task；source 接收包含四个句柄的 group AFDM 及 cparam1 中的首句柄，归并全部 run。cparam2 保留给重入协议。

应用先用一个 batch 反复提交 LOAD → sink，全部输入完成后再提交 source → avg。csConfigureBatchEntry 复制请求描述并保留算子/AFDM句柄，因此之后可释放原请求结构；它不会复制 AFDM 内的数据。batch 仍引用的 AFDM 不能释放，应先释放 batch。完整请求构造、参数更新和回收见[可执行示例](examples/cs_batch_sort_avg.c)。

当前重入协议选取**当前这一个计算请求中最后一个 AFDM**作为 STORE 数据来源或 LOAD 目的地。四路 sort 中就是 io，设备端为 MR[2]；这不是整个 batch 最后一个 AFDM，也不是 SNIA 规定的位置，而是本设备适配器和算子共同采用的约定。

## 三、设备算子：在入口判断是否重入

这些设备侧头文件位于 `src/plugins/femu/include/`，安装到 `include/snia/femu/`，不属于应用的 `cs.h`。普通算子包含 `femu-csd-kernel.h`，直接读取 `femu_csd_args`。支持重入时再包含 `cs_csf.h`。这两个头不要求算子链接 Guest 的 `libsnia.so`。

所有设备算子统一链接 `libcs-workmem.so`。排序缓冲、run 列表等私有数据用 `cs_work_begin/cs_work_alloc/cs_work_end` 管理；context 可跨多次入口保留。AFDM 保存输入、输出和交接数据，私有算法状态留在工作内存中。

[merge-sort.cpp](../../csd-programs/examples/merge-sort.cpp) 只导出 sink 和 source。sink 首次创建工作内存，成功申请的4MiB run 留在内存；只有后续申请失败才淘汰一个驻留 run。一个 run 分为16个256KiB块，重入逐次记录 STORE 的标识，合计保存完整4MiB。

设备入口通过 csIsReentry 判断本次是不是内部 I/O 后的重入，通过 csReentryValue 取得完成 I/O 的 spill_id。算子自己的 pending 状态记录正在 STORE、LOAD 还是 FREE，因此无需库增加控制 AFDM 或事件参数。

source 直接使用驻留 run，按需 LOAD spill 块；读缓冲申请失败也可淘汰驻留 run。最后一个输出 chunk 产生后，source FREE token、结束 workmem，在输出中设置 finished=1。提前停止或失败也由同一 source 入口收尾，具体参数见[示例说明](examples/README.md)。

实际调用顺序是算子返回 csNeedStore(bytes)，FEMU 设备从隐藏临时空间分配区段，将最后一个 AFDM 的数据写入 NVM，等待内部写入完成，再将 token 填入 cparam2 并排队调用相同 CSF。CSF 的重入分支记录 token 并继续工作；LOAD 和 FREE 同理。整个过程不返回 Guest，也不额外提交 D0/D1/E1 命令。原有 femu_csd_args 不变，MR、cparam1、data_buffer 保持本次请求的内容。

| 算子返回 | FEMU 设备执行 | 再次调用 CSF 时 |
| --- | --- | --- |
| `csNeedStore(bytes)` | 分配区段，I/O AFDM → NVM | `cparam2=新 spill_id` |
| `csNeedLoad(spill_id)` | NVM → I/O AFDM | `cparam2=同一 spill_id` |
| `csNeedFree(spill_id)` | DSM/TRIM 后释放区段 | `cparam2=同一 spill_id` |
| 普通结果，如 `0` 或行数 | 完成当前逻辑节点 | 不重入 |

这些请求通过标准 `CompValue` 返回，不新增 `CS_STATUS`。仅用一个 64 位值的高 4 位表示 STORE/LOAD/FREE、低 60 位表示长度或标识；这三个结果范围供 batch I/O 请求使用，没有版本字段、magic 或 128 位控制封装。负的原生返回值仍表示执行错误。

FEMU 在设备图节点内自动处理整个循环，直到得到普通结果才启动后继。整图结束后，Guest SNIA 才发布一次 callback/event；不会对内部 spill 逐次回调。单独 `csQueueComputeRequest` 仍通过 E1 执行一轮并返回原始 `CompValue`。

异步请求完成并释放请求快照后，SNIA 再调用标准 callback；此时已归还该请求的并发名额。应用可在 callback 中用 SNIA API 重配已完成的 batch 并续交下一块，包括已有 64 路请求时。应用自行保证缓冲可复用，并在取消或释放上下文前停止续交、等待回调结束；callback 不应调用所属应用中只允许主线程使用的接口。

公共库按需创建应用进程内的异步工作线程，空闲时等待并复用，库卸载时停止并等待线程结束；不为每个请求反复创建线程。同步请求仍在调用线程直接执行。工作线程数量不固定为CU数：callback可能等待另一个异步请求，公共层须允许其继续执行；设备CU配额仍由适配器和设备管理。64是在途异步请求的当前实现上限，并非SNIA规定的CU数。

设备编译可参考 `code/csd-programs/examples/CMakeLists.txt`；根 CMakeLists.txt 的 add_csd_program 统一输出位置、头文件和 workmem 链接。完整可执行重入示例见 [merge-sort.cpp](../../csd-programs/examples/merge-sort.cpp)。

## spill 区与资源生命周期

新启动 FEMU 时设置设备属性 `csd_spill_size_mb`，单位 MiB。设备属性默认0（关闭隐藏临时空间）；`run-csd.sh` 使用环境变量 `CSD_SPILL_SIZE_MB`，未设置时取256MiB。总 NAND 容量必须容纳 namespace、隐藏空间及原有 GC 保留量，容量不足会拒绝启动。隐藏空间不出现在 Identify 的 namespace 容量中，也不属于 AFDM 或 workmem。

内部临时 I/O 使用正常 FTL 的映射、NAND 延迟和 GC，数据不经过 Guest。算子只需保证最后一个 AFDM 能容纳实际请求的字节数；设备记录精确长度，在内部按页分配，不要求算子填充页尾。显式 csQueueStorageRequest 的 LOAD/STORE 仍访问应用指定的 namespace LBA，二者用途不同。

每个打开的 FEMU 设备句柄延迟创建一个设备会话；会话是设备保存 token 归属的对象，不是额外进程。同一会话成功 batch 的 token 可供后续 batch 使用，不能传给另一个设备句柄。显式 FREE 或关闭所属设备句柄时回收。失败/取消的图回收本图新增 token；含主机复制的混合批次失败时，插件通知设备回收本批先前节点新增的 token。此前成功 batch 的 token 保留。算子私有 workmem 状态仍须由应用调用算子清理入口释放；token 不含这些状态，也没有崩溃恢复。

新的临时路径不再发现或访问 GPT `femu-spill` 分区。已有初始化脚本仍保留80%/20%的历史分区布局，避免此次修改重建数据盘；其中20%不再用于新设备 spill。已挂载数据盘时无需为了本功能重新分区。新固件与新插件应配套启用，勿把新插件安装到仍运行旧固件的 Guest。

设备指令布局和执行流程见 [FEMU 设备图说明](../../FEMU/hw/femu/csd/README.md)。

真实 Guest 测试使用完整示例：`sudo build/cs-batch-sort-avg /dev/nvme0n1 /data/cs-batch-input.bin`。程序索引为420–422；库不能枚举其他进程加载的程序，应避免索引冲突。
