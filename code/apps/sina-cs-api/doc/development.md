# 开发与验证记录

各节记录当时阶段的状态；当前能力以 README 和兼容性说明为准。

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


## 2026-10-19：设备能力查询、64 位完成值与可恢复 batch

### 阶段一：设备通用能力（提交 557f05d9c）

- 新增 vendor admin GET_INFO（C0），查询版本、FDM 总量/已分配量、CU 数及内部 STORE/64 位结果功能位；命令能力表同步声明。
- E1 结果从截断/饱和的 32 位改为 CQE DW0/DW1 中完整 int64_t 位模式，原生 femu_csd_args 维持 52 字节。
- 新 QTest csd-info-result64 实际加载 SO，验证选择器、非法选择器、分配记账及 0x4353123456789abc 返回值。修复现有动态测试编译器在 Docker seccomp 下因 GLib close-range 被拒绝而跳过的问题。
- csd-info-result64、csd-exec-concurrency、512B/4KiB LBA 的 csd-nvm-copy 分别独立执行通过，没有把 SKIP 当作通过。

### 阶段二：Guest 库与设备程序

- 保留 Identify Controller 身份检查与相应测试，新增资源查询后删除环境变量配置依赖。FDMSize 来源于设备，CU 数限制软件 batch 工作线程；一个 CSE 的多个 CR 对应设备 CU，固定 CSEE 占用全部资源，NumAvailableCRs=0。
- E1 和资源查询使用 Linux 64 位 passthrough ioctl。标准 storage 同时支持 LOAD/D0 与 STORE/D1，数据不经 Guest 中转。
- 新增独立 cs_csf.h：标记、版本、DONE/NEED_STORE/NEED_LOAD/NEED_FREE completion，及 START/STORE_DONE/LOAD_DONE/FREE_DONE/ABORT 输入事件；不增加 CS_STATUS。
- batch 在请求私有副本上完成恢复循环，配置图不变；DONE 前后继始终等待。失败节点尝试 ABORT 让算子释放私有状态。
- 新增 Guest 临时池：按 GPT PARTLABEL=femu-spill 发现，512 字节 sysfs 单位换算 namespace LBA；按区段首次适配分配，40 位 opaque token，合作式进程独占锁，DSM/TRIM 后复用。成功批次保留 token，失败批次回收新增区段，设备最后参与句柄关闭时清理。
- 扩展示例 SO，演示恢复往返、跨 batch STORE/LOAD、失败、64 位返回及双分支汇合校验。继续统一链接 libcs-workmem.so。
- 中文 README 按项目、Guest 应用、设备算子组织，包含编译安装、动态链接、恢复参数与内存/临时资源生命周期；架构、传输及兼容性同步更新。

### 验证与部署

- 容器中 217 项 API 检查通过，另含共享 mock/live spill 场景断言；覆盖 8191 字节的非扇区整数长度、跨批 token、释放后拒绝旧 token、140 次失败回收、双分支恢复汇合、异步 event 以及同一图重复执行。
- 33 个标准入口、C++17 头文件及原生 52 字节 ABI 检查通过；ASan/UBSan/LeakSanitizer 同组检查通过。
- docker compose build femu-csd 完成，新镜像 ID 为 314bcc37df98；旧容器已替换，新容器 femu_docker-femu-csd-1 健康。镜像包含阶段一设备代码，应用/设备程序继续使用已有代码挂载。
- 新 Guest 的空白 16 GiB 模拟盘执行既有 sh 挂载脚本：p1 12.8 GiB ext4、PARTLABEL=femu-data，挂载 /data；p2 3.2 GiB、PARTLABEL=femu-spill，不格式化、不挂载。
- Guest 重新编译库和测试，无 SINA_CS_FEMU_DEVICES：api-smoke 输出 SNIA API batch PASS: 4 12 28 44；live-api 确认 4 个 native CSF 起止时间区间重叠，且 64 位结果、真实内部 STORE/LOAD、跨批 spill、汇合与失败回收全部通过。
- Guest 安装到 /tmp/sina-cs-api-install，用安装后的 cs.sh 和 -lsina-cs-api 动态链接应用；ldd 确认加载该 .so，实际执行示例通过。

### 当前边界

图调度在 Guest 库，设备仍无整图执行。临时池不提供跨进程共享 token 或崩溃持久化恢复；原生运行中不能抢占。CSF 算法私有状态与 ABORT 清理由算子负责，应用失败后需要销毁其查询状态；库不会理解或回滚排序器/hash 表。全局程序枚举、设备统计、文件 I/O、主机映射、动态 CSEE 和 native 沙箱仍不支持。
