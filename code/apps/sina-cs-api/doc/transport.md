# 原生 FEMU NVMe 传输适配

SNIA 定义应用 API，并不要求增加设备命令。本库仅使用初始 FEMU 已有的命令，不修改 `code/FEMU/hw/femu/csd/`、设备 ABI 或现有 `.so` 加载机制。

## 命令映射

| API 行为 | 原生命令及适配 |
| --- | --- |
| AFDM 分配/释放 | B0 / C0，4096 字节分配粒度 |
| Host↔AFDM | F5 / F2，最多 1 MiB 一次 |
| AFDM 初始化 | Guest 生成模式，再用 F5 分块写入 |
| AFDM→AFDM | Guest 临时缓冲，F2→F5；支持重叠范围，存在外部传输 |
| block LOAD | D0，按真实 namespace LBA 大小计算 |
| native CSF 加载/卸载 | 原有 admin 25 / 22 |
| CSF 激活/去激活 | 原有 admin 23 |
| CSF 计算 | 原有 E1，MR 列表与两个 cparam |
| batch/callback/event | Guest 软件实现，无新增设备命令 |
| 属性与程序发现 | 显式实验配置、进程内程序目录；不能获取的信息返回不支持 |

不使用新增的 C1/B1/E3，不需要 `cs-api-wire.h` 或 `cs-api.inc`。SDK 只依赖自身头文件。

## CSF 参数适配

原生 `femu_csd_args` 保持原样：`numr / mr_addr / mr_len / cparam1 / cparam2 / data_buffer / buffer_len`。Linux NVMe passthrough 的 metadata 字段不能用来随意指定命令 DW4/5，因此不借用它传入扩展载荷。

每次计算：

1. 库将应用 AFDM 参数按出现顺序编码为原生 MR。
2. 分配一个 4096 字节 AFDM，将 header 和最多 64 个类型化参数写入。该缓冲是 CSF 输入，不是 runtime 工作内存。
3. 将参数缓冲追加为最后一个 MR，MR length 精确给出描述长度，提交 E1。前两个标量的低 64 位也填入 cparam1/2。
4. `.so` 用可选的 `cs_kernel.h` 解码器解析最后一个 MR；AFDM 参数保存 MR 索引和 offset，整数保存最多 128 位值。FEMU 仍只解析 MR 并调用 `.so`。
5. E1 完成后释放参数缓冲，主机调度后继或报告完成。

描述整数使用 little endian，不包含进程指针。解码后的指针和参数数组只在 CSF 私有内存中产生。不使用原生 E1 的附加载荷字段，也不改变它的 dlen/runtime 解释。

`cs_kernel.h` 是库与采用该约定的 CSF 之间的可选适配头，**不是 SNIA 规定的 CSF ABI，也不是 FEMU runtime 扩展**。现有 CSF 保持原来的接口；要通过本库使用完整类型化参数，需在 CSF 入口采用此解码约定。不能假定任意旧 `.so` 都能直接解释它。

## 返回与限制

原生 FEMU 不解释 CSF 返回值的正负。本库的 native profile 约定：0..INT32_MAX 为 completion，负的 32 位值为执行错误；由 Guest 根据 E1 completion 转换状态。大量结果写入 AFDM。

原生 block LOAD 及其时序保持不变。设备不执行整图；本库的并发配置不能替代设备 CU 能力或提供运行中的 CSF 抢占。
