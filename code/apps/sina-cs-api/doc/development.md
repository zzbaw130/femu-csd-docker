# 开发与验证记录

## 2026-10-18：基于原生 FEMU 的 Guest API 与软件 batch

分支：`sina_csd_dev`。基线：`femu-csd-with-original-postgre`，解引用提交 `f69c3bb49`。当前范围是 Guest SDK 和示例 CSF，不移植 PostgreSQL 扩展或 SQL runtime。

### 最终修改

1. 提供 `include/src/tests/examples/doc` 结构、全部 33 个 v1.1 API 入口、C/C++ 头文件；应用包含 `cs.h` 或 `cs.sh` 并链接库。
2. Guest 进程内实现 opaque 句柄、引用保护、同步/异步请求、callback、event，以及最多 64 节点的 SERIAL/PARALLEL/HYBRID 软件 batch。
3. 使用原生 B0/C0/F2/F5/D0/E1 和加载/激活命令，完成 AFDM 管理、传输、block LOAD 和 CSF 执行。
4. 撤回初版的 `cs-api-wire.h`、`cs-api.inc`、C1/B1/E3 命令和 `femu_csd_args` 扩展。FEMU CSD 源码及原有设备程序 ABI 与基线一致。
5. 类型化参数转为 CSF 的 4 KiB AFDM 输入缓冲；`.so` 使用可选解码器在私有栈上解析。设备只执行原生 MR 调用；每次完成后释放参数缓冲。
6. 清零/填充改为 Guest F5 写入；AFDM→AFDM 改为分块 F2/F5 fallback，正确处理重叠，文档明确其外部传输成本。
7. 原生设备没有能力查询：显式实验配置指定 namespace/FDM/CU；CSF 发现采用进程内目录。实时 CU 属性和全局设备统计返回不支持。
8. 中文文档说明实际执行位置、主机编排与设备执行边界、参数 ABI、能力来源及局限。SDK 可以独立构建安装，无需 Guest 挂载 FEMU 源码。

### 验证

- 本地静态/动态库、示例、集成测试程序、C++17 头文件构建通过，启用 `-Wall -Wextra -Werror`。
- 本地 mock transport 仅接受原生 FEMU 命令；161 项检查通过，覆盖参数/offset、失败路径释放参数 AFDM、配置缺失/错误、同进程多 handle 目录、图环回滚和重配置、并行分支、callback/event、引用保护、多分块正向/反向重叠复制。
- 33 个 API 声明/实现检查和 52 字节原生 native ABI 检查通过。
- 独立临时目录重编译恢复后的 FEMU，未替换现有运行中的 QEMU。

- 隔离测试 Guest：复制系统盘到临时路径，snapshot 模式，1536 MiB RAM、256 MiB FEMU namespace、64 MiB FDM、4 CU、独立 SSH 2223；原有 Guest 未重启。
- Guest 从源码构建库与示例，原生 NVMe 软件 batch 输出 `SNIA API batch PASS: 4 12 28 44`。
- 真实 NVMe live 测试通过：64 参数、纯标量 CSF、128 位高低位、CSF 负返回值转错误、FDM 填充/软件复制、D0 block LOAD、资源回收。四个不同程序 ID 的 CSF 在输出 AFDM 写入起止时间戳，确认四个执行区间重叠；没有使用新增能力查询。
- AddressSanitizer、UndefinedBehaviorSanitizer 和 LeakSanitizer：161 项 mock 检查通过。
- `git diff HEAD -- code/FEMU/hw/femu/csd code/csd-programs/femu-csd-kernel.h` 为空；未修改原生 FEMU CSD 或原有 native ABI。
- 临时 Guest 验证后关闭并移除临时盘；原有 Guest 保持运行。
- `git diff --check` 与 shell 语法检查通过。

### 当前边界

硬件 batch flag=0，无设备整图提交。设备全局统计、实时 CU 查询、跨进程程序发现、STORE、主机映射、动态 CSEE、管理通知、运行中抢占、私有工作内存配额和通用 native 沙箱尚不支持。参数传输每次增加一次 B0/F5/C0，以及一个 E1 MR；不能把它的开销归为原生 CSF 计算性能。完整范围见 [兼容性说明](compatibility.md)。
