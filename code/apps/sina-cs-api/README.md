# FEMU 的 SNIA Computational Storage API

本项目实现 SNIA Computational Storage API v1.1（2025-05-31）的 Linux Guest 库。目录沿用 `sina` 命名，规范机构名称是 **SNIA**。上层应用包含 `cs.h` 或兼容入口 `cs.sh` 并链接 `libsina-cs-api.so`，不再自行封装 NVMe ioctl。`cs.sh` 是 C 头文件。

## 一、项目：编译、部署与能力

| 位置 | 内容与执行位置 |
| --- | --- |
| `include/` | 标准 API/类型；`cs_kernel.h` 为原生 CSF 参数适配；`cs_csf.h` 为可恢复 CSF 扩展约定 |
| `src/` | Guest 应用进程中的资源管理、NVMe 传输、batch 调度、callback/event、spill 区段管理；使用 Guest vCPU |
| `examples/`、`tests/` | Guest 应用示例，以及 mock/真实 NVMe 测试 |
| `doc/` | 架构、传输、兼容性与开发记录 |
| `../../csd-programs/sina-cs-api/` | 设备 `.so` 源码；在容器内编译，由容器内 FEMU CU 线程执行，逻辑上使用 CSD CPU |
| `../../csd-programs/csd-workmem/` | 设备算子共享的有限私有工作内存分配器 |
| `../../FEMU/hw/femu/csd/` | 通用设备命令、FDM、CU 与内部 I/O；不理解 SQL 或本库的恢复协议 |

在 Guest 内编译、安装：

```sh
cd /home/femu/apps/sina-cs-api
make all examples live
sudo make install PREFIX=/usr/local
sudo ldconfig
```

生成 `build/libsina-cs-api.so` 和 `.a`，安装标准头文件、两个可选扩展头及库。可用 `BUILD_DIR=/tmp/sina-cs-api-guest` 改变构建路径。应用和库在 Guest 中编译，设备算子在容器中编译，二者不要混用二进制。

在宿主项目根目录编译设备示例和执行库测试：

```sh
docker compose exec -T femu-csd make -C /workspace/code/csd-programs sina-cs-scale.so
docker compose exec -T femu-csd make -C /workspace/code/apps/sina-cs-api BUILD_DIR=/tmp/sina-cs-api-check check
```

所有设备 `.so` 输出到 `code/csd-programs/build/`。mock 测试会加载真实示例 `.so`，因此在与算子相同的容器环境测试，避免 glibc 版本不匹配。

当前 FEMU backend 先用 Identify Controller 核对 `FEMU Computational Storage Controller`，再用 Identify Namespace 获取 LBA 大小/容量，通过通用 vendor GET_INFO 查询 FDM 容量、CU 数与功能位。**不再需要 `SINA_CS_FEMU_DEVICES`**。旧值中的容量仅用于属性报告，CU 数用于软件 batch 工作线程上限；现在都以设备查询结果为准，真实分配与执行仍受设备限制。

[SNIA v1.1](https://www.snia.org/csapi) 第 6.12.1 节定义了 `csQueryDeviceProperties`：`FDMInfo.FDMSize` 报告容量；本 backend 将 FEMU 的 CU 映射为一个 CSE 的多个 CR，`CSEInfo.NumCRs` 报告数量。固定 native CSEE 已占有全部 CR，所以 `NumAvailableCRs=0`；这是“未分配资源数”，不是“当前空闲线程数”。标准没有规定必须用哪条 NVMe 命令查询。

软件 batch 支持 SERIAL、PARALLEL、HYBRID 及依赖图。图在 Guest 库中调度，每个节点仍是独立命令；硬件 `BatchRequestsSupported=0`。全部 33 个 API 有声明和可链接入口，选项范围见 [兼容性说明](doc/compatibility.md)，不代表所有可选能力均已实现。

## 二、上层应用：包含头文件、链接与提交 batch

```c
#include "cs.sh"
/* 仅使用可恢复算子时额外包含： */
#include "cs_csf.h"
```

已安装库时：

```sh
cc application.c -lsina-cs-api -pthread -o application
```

不安装、直接使用本地动态库时，在项目目录执行：

```sh
cc application.c -Iinclude -Lbuild -lsina-cs-api -pthread \
    -Wl,-rpath,"$PWD/build" -o application
```

应用依次打开设备、分配 AFDM、加载并激活 CSF、构造请求和图，再提交。示例 [api-smoke.c](examples/api-smoke.c) 包含完整生命周期；在 Guest 内运行 `sudo build/api-smoke /dev/nvme0n1`，期望 `SNIA API batch PASS: 4 12 28 44`。示例预留程序索引 400，真实测试使用 401–407、410–416。

`csCSFDownload` 使用 vendor-specific 类型，描述符为 `文件名\0入口符号\0`。文件名在容器的 `csd_program_dir` 下解析，例如 `sina-cs-scale.so\0cs_spill_roundtrip\0`；Guest 无需拥有该 `.so` 文件，也不能传入任意宿主路径。激活后用 `csGetCSFId` 获取 API 句柄。

以下将一个已加载的可恢复算子 `function` 加入 batch。`io` 是已分配的 8192 字节 AFDM：

```c
CsBatchRequest *r = calloc(1, sizeof(*r) + sizeof(CsComputeArg));
r->ReqType = CS_QUEUE_COMPUTE;
r->u.Compute.CSFId = function;
r->u.Compute.NumArgs = 2;
csHelperSetComputeArg(&r->u.Compute.Args[0], CS_AFDM_TYPE, io, (unsigned long)0);
csHelperSetComputeArg(&r->u.Compute.Args[1], CS_128BIT_VALUE_TYPE,
                     csCsfControl(CS_CSF_START, 0, 0));
CS_BATCH_HANDLE batch;
CS_BATCH_INDEX index;
csAllocBatchRequest(CS_BATCH_SERIAL, 1, &batch);
csConfigureBatchEntry(batch, CS_BATCH_ADD, r, 0, 0, &index);
u64 value;
CS_STATUS status = csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, &value);
/* 检查 status；成功时 csCsfPayload(value) 是 DONE 的结果。 */
csFreeBatchRequest(batch);
free(r);
```

示例省略了逐次错误处理，正式应用需检查每个状态。batch 配置复制描述并保留资源引用，执行期间不能修改。最后两个参数固定为 **I/O AFDM、128 位控制值**，前面可继续放普通输入/输出 AFDM、计数、私有算子状态 handle 等参数。普通 CSF 不带控制标记时按原方式执行，不会被当作可恢复算子。

- SERIAL 按依赖/配置顺序单线程执行；PARALLEL 独立节点并行；HYBRID 所有就绪节点可并行，后继等待全部前驱完成。
- `After` 是前驱，`Before` 是后继；JOIN 添加 `After → Before`。需要使用同一 AFDM 中的数据时显式建立依赖，库不推断读写冲突。
- 可恢复节点经历若干次计算和内部 I/O，直到 DONE 才算完成；后继、callback、event 不会在 NEED_STORE/NEED_LOAD 时提前完成。
- LOAD/STORE 均可作为标准 `CsStorageRequest` 配置为图节点；恢复协议的 I/O 由库内部自动提交同一标准请求。
- 单独调用 `csQueueComputeRequest` 只执行一次并返回原始 `CompValue`；**自动恢复由 `csQueueBatchRequest` 提供**。

真实测试先在 Guest 执行 `/home/femu/scripts/mount-nvme.sh`，再运行 `sudo build/live-api /dev/nvme0n1`。首次分区会清除该模拟盘。脚本给正常区域和 spill 区域分别标记 `femu-data`、`femu-spill`；库根据 **GPT PARTLABEL=femu-spill** 识别专区，不假定一定是 p2。专区不格式化、不挂载，应用普通文件系统只使用正常分区。

## 三、设备算子：参数、私有内存与 spill

原生算子入口仍为 `int64_t fn(struct femu_csd_args *)`，结构保持 52 字节。FEMU 已经把命令中的 MR 转换为 `mr_addr/mr_len`，算子可以直接访问这些字段和 `cparam1/2`。

本库为了传递 SNIA 请求中的任意参数序列（最多 64 项，含 128 位整数和 AFDM offset），另外在最后一个 MR 中放入类型化参数描述。`cs_kernel_decode` 只负责读取这份额外描述，方便示例算子按原参数次序取值；它不是 SNIA 要求，也不是所有 FEMU 算子必须调用的接口。当前库每次计算都额外分配 4 KiB 参数 AFDM，因此不能把本库当前的参数布局与原生 MR 布局直接混用。

需要恢复协议时再包含 `cs_csf.h`。**算子不链接 Guest 的 libsina-cs-api.so**；解码和协议辅助函数都在头文件中。所有算子统一链接设备侧 `libcs-workmem.so`，私有工作区使用 `cs_work_begin/cs_work_alloc/cs_work_free/cs_work_end`；可跨多次入口保留同一 context，结束或 ABORT 时释放。多个 `.so` 引用同一共享库、同一个总配额。AFDM 仅用于输入、输出及在算子间传递的数据。

设备侧编译命令的等价形式为：

```sh
cc -shared -fPIC operator.c \
    -I../../apps/sina-cs-api/include -I../csd-workmem/include \
    -L../build -Wl,--no-as-needed -lcs-workmem -Wl,-rpath,'$ORIGIN' \
    -o ../build/operator.so
```

`../mk/csf.mk` 只是公共 Makefile 片段，复用 `build/` 输出位置、`libcs-workmem.so` 链接选项和 `$ORIGIN` 搜索路径。现有组件通过 `include` 使用它；手写相同编译命令也可以，它不参与算子运行或 spill 调度。

spill 的实际流程只有：算子返回 NEED_STORE 和待写长度 → 库分配 LBA 并将 I/O AFDM 写入 → 库再次调用同一 CSF，参数中带回写入完成及 spill 信息 → CSF 更新自己的 run 记录并继续。LOAD 同理。CSF 不需要自己发存储命令，但需区分首次调用与 I/O 完成后的重入，避免重复提出同一个请求。

当前实现把这些返回信息打包在一个 128 位参数中，称为“控制值”；它就是 spill 完成信息，不是额外的系统配置。下面的辅助函数只是这个打包格式的访问方式，均属本项目约定。操作请求放在标准 `CompValue` 中，不新增 `CS_STATUS`：

| 算子返回 `csCsfCompletion(action, payload)` | 库的动作 | 下一次入口的控制事件 |
| --- | --- | --- |
| `CS_CSF_NEED_STORE, valid_bytes` | 分配临时区段，将声明的 I/O AFDM 经 D1 写入 NVM | `CS_CSF_STORE_DONE`，token 与有效字节数 |
| `CS_CSF_NEED_LOAD, token` | 查找区段，经 D0 读入同一 I/O AFDM | `CS_CSF_LOAD_DONE`，token 与有效字节数 |
| `CS_CSF_NEED_FREE, token` | DSM/TRIM 后回收区段 | `CS_CSF_FREE_DONE` |
| `CS_CSF_DONE, result` | 当前逻辑节点完成，推进后继 | 不重入 |

控制参数用 `csCsfEvent(value_lo)`、`csCsfToken(value_lo)`、`csCsfBytes(value_hi)` 读取。`CompValue` 带 magic/version/action 和 40 位 payload；控制参数为 128 位，包含版本、事件、40 位 token、32 位有效字节数。私有排序 runs、hash 表和游标仍由算子管理，不塞进这个整数。需要传给下一条 pipeline 时，可输出一个私有状态 handle；保存内存块引用和临时 token 的是 handle 背后的设备私有对象。

完整可编译示例在 [scale.c](../../csd-programs/sina-cs-api/scale.c)：`cs_spill_roundtrip` 在 START 生成 8191 字节，STORE_DONE 后清空 I/O AFDM 并请求 LOAD，LOAD_DONE 校验数据并请求 FREE，FREE_DONE 返回 DONE。`cs_spill_store` 与 `cs_spill_load` 展示 token 跨两个 batch 使用。两者没有主机中转 spill 数据。

排序算子可在 STORE_DONE 时把 token 追加到自己的 run 列表，然后继续处理；source 再按列表请求 LOAD。库只管理区段和恢复事件，不更新排序器私有结构。一次入口最多提出一个 I/O 请求，多段 run 按多次恢复循环处理。

边界与生命周期：

- STORE 的有效字节数须大于 0；I/O 缓冲必须容纳按 namespace LBA 向上取整后的长度，算子应初始化末尾填充字节。LOAD 也需要容纳完整的取整长度。
- token 是 Guest 库当前进程/namespace 内的 opaque 标识，不是 LBA、指针或可持久化文件名。成功 batch 后保留，显式 NEED_FREE 或最后一个参与该 spill 池的 device handle 关闭时释放；不可跨进程或设备重启复用。
- 同进程同 namespace 共享区段分配器，通过专区设备节点的独占 `flock` 阻止另一个本库进程同时分配。锁是协作约定，专区不能被其他程序直接写入。控制元数据保存在 Guest 私有内存。
- batch 失败时等已运行节点退出，再回收该 batch 新增的临时区段；更早成功 batch 的 token 保留。TRIM 失败的区段保留到关闭，避免提前重用。进程崩溃没有持久化恢复，重新打开后专区重新使用。
- 中途发生错误时，库尝试以 `CS_CSF_ABORT` 重入出错的当前可恢复节点，让它释放私有工作内存；ABORT 返回的 I/O 请求不会被执行。其他节点的查询私有状态仍需应用在失败后统一销毁，库不会回滚算法状态。原生运行中的 CSF 仍不能被强行抢占。
- 原生返回值使用完整有符号 64 位：负值映射为执行错误；非负值原样作为 `CompValue`，大结果写输出 AFDM。扩展标记和 action 都不是 SNIA 标准状态。

更多细节见 [架构](doc/architecture.md)、[传输绑定](doc/transport.md) 和 [开发记录](doc/development.md)。
