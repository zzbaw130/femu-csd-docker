# FEMU 传输绑定

SNIA 定义应用 API；具体 NVMe 绑定属于本 backend。设备端只有通用资源查询、64 位完成值和内部写入扩展，没有 SNIA 专用 wire 头、SQL runtime 或图命令。`femu_csd_args` 结构不变。

| API 行为 | NVMe 传输 |
| --- | --- |
| 设备身份、LBA/容量 | Identify Controller / Namespace |
| 能力 | vendor admin C0 GET_INFO，CDW10=0 版本、1 FDM 字节、2 已分配 FDM、3 CU 数、4 功能位；64 位 CQE |
| AFDM 分配/释放 | I/O B0 / C0，4096 字节分配粒度 |
| Host↔AFDM | F5 / F2，最多 1 MiB 一次 |
| 清零/填充、AFDM→AFDM | Guest F5 或 F2→F5 软件适配 |
| block LOAD / STORE | D0 / D1，SLBA=CDW10/11，NLB-1=CDW12低16位，AFDM ID=CDW13，byte offset=CDW14/15；无 Guest 数据载荷 |
| native CSF 加载/卸载、激活 | admin 25 / 22、23 |
| CSF 计算 | 原生 E1 MR 列表，使用 NVME_IOCTL_IO64_CMD 保留完整完成值 |
| 临时区段释放 | DSM 09，AD=1，控制描述为绝对 namespace LBA/块数 |
| batch、callback、event、恢复协议 | Guest 库实现，没有整图 NVMe 提交 |

GET_INFO 功能位 bit0=内部 STORE，bit1=64 位 CSF 返回。使用 NVME_IOCTL_ADMIN64_CMD 获取查询值。非负 CSF 返回原样作为 CompValue，负 int64_t 返回转换为 CS_ERROR_IN_EXECUTION。

## 类型化参数

1. AFDM 参数按出现顺序编码为原生 MR。
2. 将 header 和最多 64 个类型化参数写入额外的 4 KiB AFDM 输入缓冲，再追加为最后一个 MR，长度精确描述 payload。
3. 前两个标量低 64 位同步填入 cparam1/2；完整 128 位值保存在参数描述中。
4. 设备 `.so` 用 `cs_kernel.h` 在本地栈解码。FEMU 只解析 MR 并调用 `.so`，不解析描述内容。
5. E1 完成后释放参数缓冲，失败路径也释放。

整数 little endian，无进程指针，不借用 Linux ioctl metadata 字段去传任意 DW4/5。该约定不是 SNIA 规定的设备 ABI，旧 CSF 若直接依赖 MR 次序，应按自己的接口适配。

## 可恢复 CSF v1

`cs_csf.h` 是 Guest 库和设备程序共同包含的约定，不被 FEMU include。

- 可恢复请求最后两参：IO AFDM、128 位控制值，初值 `csCsfControl(START,0,0)`。
- CompValue `[63:48]=0x4353, [47:44]=版本1, [43:40]=action, [39:0]=payload`。
- 控制低64位 `[63:56]=0, [55:48]=版本1, [47:40]=event, [39:0]=token`；高64位 `[63:32]=0x43534631, [31:0]=有效字节数`。
- STORE payload=有效字节数，LOAD/FREE payload=token，DONE payload=结果。输入控制事件为 START、STORE_DONE、LOAD_DONE、FREE_DONE、ABORT。

batch 调度器在执行副本上改写控制参数，重新走普通 E1。算子收到 STORE_DONE 后自行更新私有 run 列表；库不读写算法状态。STORE/LOAD 的 payload 始终留在 AFDM/NVM 内部，只有控制描述经过 Guest。单独 csQueueComputeRequest 返回原始完成值，不自动处理恢复动作。
