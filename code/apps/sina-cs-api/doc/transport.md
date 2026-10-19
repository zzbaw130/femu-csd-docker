# FEMU 传输绑定

应用参数直接映射原生 `femu_csd_args`，结构不变。AFDM 依次生成 MR，整数依次填入 cparam1/2，不生成额外的参数缓冲。

| 行为 | 传输 |
| --- | --- |
| 设备身份、namespace 容量/LBA | Identify Controller / Namespace |
| 资源查询 | admin C0 GET_INFO，CDW10=0版本、1 FDM总量、2 FDM已分配量、3 CU数、4功能位 |
| AFDM 分配/释放 | I/O B0 / C0 |
| Host ↔ AFDM | F5 / F2 |
| 内部 LOAD / STORE | D0 / D1；SLBA=CDW10/11，NLB-1=CDW12低16位，AFDM ID=CDW13，offset=CDW14/15 |
| CSF 加载/卸载、激活 | admin 25 / 22、23 |
| 执行 | E1，IO64 ioctl 接收完整 int64_t 返回值 |
| 临时区释放 | DSM 09，AD=1 |
| batch、event/callback | Guest 库实现 |

计算的 AFDM offset 目前只支持 0；多个 AFDM 保留相互顺序，标量保留相互顺序，最多两个 64 位标量位置。128 位值占两个位置。零 MR 请求直接执行纯标量 CSF，不分配占位 AFDM。native MR length=0 表示整个 AFDM。

## 原生附加输入：当前未映射

原生 E1 的 `dlen` 位于命令 CDW4，表示 inline MR 列表和附加输入的总字节数。FEMU 将 `dlen` 字节读入本次执行的输入副本，MR 列表占 `numr * 32` 字节，其后的内容通过 `data_buffer/buffer_len` 交给算子。整份输入上限为 1 MiB，随执行任务释放；不会把算子对副本的修改自动写回 Guest。该路径用于带 inline MR 的执行。

当前库只提交 MR 列表，`dlen=0` 时设备按 MR 列表大小推导，因此算子收到 `data_buffer=NULL, buffer_len=0`。Linux `nvme_passthru_cmd64.data_len` 只是驱动映射的用户缓冲长度，不会自动填入设备命令的 CDW4；ioctl 的 `metadata` 是独立元数据缓冲地址，也不能当作原始 dlen 使用。标准驱动的命令构造见 [Linux NVMe ioctl 实现](https://github.com/torvalds/linux/blob/v6.6/drivers/nvme/host/ioctl.c)。

SNIA 请求没有直接对应原生附加载荷的主机字节数组成员。标准 AFDM 输入仍可携带应用自己定义的参数结构；本库未实现的 descriptor 类型不能视作已接通的附加载荷支持。这是 API 到设备的映射缺口，不是 SNIA 禁止传递参数结构。

## 重入

重入算子保留 cparam2，初值 0。同一个 CSF 请求中的最后一个 AFDM 参数是 I/O 缓冲，cparam1 可以放算子自己的状态 handle。算子入口用 csIsReentry 判断，csReentryValue 读取上次完成 I/O 的 spill_id。这个位置是本项目约定，不是对整批请求的排序要求。

CompValue 的高四位为操作：1 STORE、2 LOAD、3 FREE，低 60 位为字节数或 spill_id；这三个范围用于 batch I/O 请求。其他非负值完成节点，负 int64_t 转为执行错误。没有附加的控制参数。

库按请求完成内部 I/O，再在执行副本中填入 cparam2，重新执行 E1。STORE 返回新 ID，LOAD/FREE 返回原 ID；算法自己的 pending 状态解释确认。原 batch 描述不变。单个计算请求不自动推进重入，batch 才自动推进。
