# 开发与验证记录

## 2026-10-19

分支 `sina_csd_dev`。第一阶段提交 `b46d5923a`：共享 arena 分配器、context、统计、两个独立探针和 Guest 集成客户端。

第二阶段整理目录：原生 vadd、original-kernels、BPF 和原生 ABI 头移入 examples，内容与原始 tag 相同；每个组件有自己的 Makefile，根目录汇总，所有当前 .so 输出到 build。旧分支遗留二进制归档至本机 build/legacy，不加载也不清理掉。

### 配置

- `CSD_PROGRAM_DIR=/workspace/code/csd-programs/build`。
- `CSD_WORKMEM_SIZE_MB=64`：本进程所有协作式 CSF 共用一个池。
- `CSD_FDM_SIZE=64`：独立的 FDM 输入输出容量。
- `CSD_NR_CU=4`。
- Compose 从挂载的 `scripts/container/start-csd.sh` 启动；run-csd.sh 使用 sudo env 显式传递工作池变量。已检查真实 QEMU 进程环境与程序加载路径。

### 验证

1. 本地与新容器内均编译通过，启用 Wall/Wextra/Werror；C++ 头文件链接通过。
2. 有限容量、max_align_t 对齐、context 限额与归属校验、碎片合并、批量回收、calloc/realloc、过期句柄、256 context 上限、8 线程共 24000 次分配/释放检查通过。
3. 两个 RTLD_LOCAL 加载的独立 .so 共用 16 MiB 预算：两个 6 MiB 成功，第三个失败；跨库释放后重试成功。
4. AddressSanitizer/UndefinedBehaviorSanitizer 下的分配器及跨库测试通过；这是自定义 arena，sanitizer 不等同于逐块越界沙箱。
5. 已停止旧 femu-csd 容器并重新创建，最终仅一个 femu-csd 容器运行。原 RAM SSD 内容随重启失效，没有执行数据库镜像恢复。
6. 新 Guest 真实 NVMe：四个不同程序 ID 覆盖两个探针 .so，每个同时申请 24 MiB，64 MiB 池仅允许两个成功，另外两个报告 ENOMEM。峰值为 50331744 字节（包含块头/对齐），两轮完成后 used 均为 0。
7. Guest SNIA batch 输出 `4 12 28 44`；64 参数、128 位、原生错误、block LOAD、四个 CU 重叠执行检查通过。
8. 原生 phantom、vadd、knn/sql/grep/lz4 smoke 全部通过；均从统一 build 加载。
9. SDK 161 项 API 检查及已有 Identify 测试通过。原先未提交的 Identify 源码/文档修改保留，不纳入本阶段提交。

FEMU CSD 核心和 femu_csd_args 未修改。启动脚本只增加工作池配置透传。池限制不覆盖原生栈、代码、固定控制表以及未接入的 malloc/new/第三方库，不能称为全部设备 RAM 的硬限制。
