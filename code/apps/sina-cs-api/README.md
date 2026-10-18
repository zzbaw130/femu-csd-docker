# FEMU 的 Computational Storage API

本目录在 `sina_csd_dev` 分支实现 SNIA Computational Storage API v1.1（2025-05-31）的 Linux Guest API 库。目录和分支沿用项目约定的 `sina` 名称，规范机构名称为 **SNIA**。

应用包含 `cs.h`，或兼容入口 `cs.sh`，并链接 `libsina-cs-api`，无需自己编码 NVMe 命令、包含 Linux NVMe 结构或重复实现 `csd_xxxx`。`cs.sh` 是 C 头文件，不是 shell 脚本。FEMU CSD 代码和原有 `femu_csd_args` 保持不变，batch 在 Guest 库中编排。设备能力受实现限制，所有 33 个规范函数都有声明和可链接入口；不支持的资源或选项返回规范状态，不能将此表述为所有可选功能都已实现。完整范围见 [兼容性说明](doc/compatibility.md)。

```c
#include "cs.sh"   /* 也可以使用 cs.h */

CS_DEV_HANDLE dev;
CS_MEM_HANDLE mem;
CsMemFlags flags = {.FDMId = 1, .Flags = CS_FDM_CLEAR};
CS_STATUS status = csOpenCSx("/dev/nvme0n1", NULL, &dev);
if (status == CS_SUCCESS)
    status = csAllocMem(dev, 4096, &flags, &mem, NULL);
/* 通过 csQueueCopyMemRequest / csQueueStorageRequest / csQueueComputeRequest 使用 mem。
 * 完成后 csFreeMem(mem, &flags)，再 csCloseCSx(dev)。 */
```

## 目录与执行位置

| 目录 | 内容 | 实际执行位置 |
| --- | --- | --- |
| `include/` | 规范 API、类型、兼容头及 FEMU 原生 CSF ABI | 头文件，无独立执行位置 |
| `src/` | 句柄管理、NVMe 传输、发现、内存、计算、异步及软件 batch 图调度 | Guest 应用进程及其线程，使用 Guest vCPU |
| `examples/` | 只调用标准 API 的应用示例 | Guest |
| `tests/` | mock transport 的接口、图及生命周期检查 | 构建机；不代替 NVMe 集成验证 |
| `doc/` | 架构、能力范围、协议和验证记录 | 文档 |
| `../../csd-programs/sina-cs-api/` | 示例设备程序 | 容器内 FEMU CU 线程，逻辑上是 CSD CPU |
| `../../FEMU/hw/femu/csd/` | 保持原生的 CSD 命令、FDM 及 CU 管理 | 容器内 FEMU/QEMU 进程 |

当前 batch 是标准允许的**软件编排**：Guest API 库根据依赖提交各节点，应用只接收整批完成。它没有将整图作为一次 NVMe 命令下发，也没有替代设备固件调度。因此 `CSxProperties.Flags.BatchRequestsSupported` 为 0，软件 batch API 仍可使用。

## 编译与使用

在 Guest 内：

```bash
cd /home/femu/apps/sina-cs-api
make BUILD_DIR=/tmp/sina-cs-api-guest all examples
cc application.c -Iinclude /tmp/sina-cs-api-guest/libsina-cs-api.a -pthread -o application
```

也可 `sudo make install PREFIX=/usr/local`，之后使用 `-lsina-cs-api -pthread`。安装动态库后应运行 `sudo ldconfig`，或者为运行环境设置相应库搜索路径。静态链接不需要修改动态库路径。

从宿主编译设备程序：

```bash
docker compose exec -T femu-csd make -C /workspace/code/csd-programs sina-cs-scale.so
```

`csCSFDownload` 使用 FEMU 的 vendor-specific CSF 类型。数据描述符为 `文件名\0入口符号\0`，文件位于容器中配置的 `csd_program_dir`；不接受 Guest 传入任意宿主路径。`examples/api-smoke.c` 展示完整的加载、激活、发现、内存分配、异步 batch、读取和回收。

本库直接使用原生 FEMU CSD 命令，不需要为 SNIA API 修改或重编译 FEMU。

原生设备没有能力查询，运行前必须显式配置本实验使用的设备、FDM 字节数、CU 数。例如当前 64 MiB FDM、4 CU：

```bash
export SINA_CS_FEMU_DEVICES=/dev/nvme0n1:67108864:4
```

多个设备使用逗号分隔。此值需与 FEMU 启动配置一致；不是设备实测结果。缺少配置时打开失败，避免把普通 NVMe namespace 误认为支持 CSD。实时 CU 属性与设备全局统计目前返回不支持。

在对应测试 Guest 中：

```bash
sudo env SINA_CS_FEMU_DEVICES=/dev/nvme0n1:67108864:4 /tmp/sina-cs-api-guest/api-smoke /dev/nvme0n1
# 期望：SNIA API batch PASS: 4 12 28 44
```

示例使用程序索引 400；该索引应由测试环境预留。库拒绝覆盖本进程目录中已激活的程序；它无法枚举其他进程已加载的程序，因此需要应用预留索引。应用中的所有状态返回都应检查。设备访问权限仍由 Guest Linux 的设备节点和 NVMe ioctl 权限控制。

## 验证

```bash
make -C code/csd-programs sina-cs-scale.so
make -C code/apps/sina-cs-api check
```

检查使用独立 mock NVMe transport，并动态加载真实的示例设备 `.so`，覆盖类型化参数、偏移、内存初始化、图描述快照、环检测、并行分支和异步资源引用。真实设备验证另外记录在 [开发记录](doc/development.md)。

设计说明见 [架构与内存](doc/architecture.md)；NVMe 编码见 [FEMU 传输绑定](doc/transport.md)。

参数传递使用原生 E1 的 MR：库额外写入一个 4 KiB AFDM 参数输入缓冲，CSF 使用可选 `cs_kernel.h` 自行解析；不扩展 FEMU runtime。CSF 解码结果和私有工作区不在 AFDM 中。

清零/填充通过 Guest F5 写入；AFDM→AFDM 的软件 fallback 会经 Guest F2/F5 往返。连续 batch 节点可直接复用同一个 AFDM 作为前驱输出/后继输入，无需调用该复制路径。
