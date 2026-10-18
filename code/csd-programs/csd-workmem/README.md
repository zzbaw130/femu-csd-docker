# CSD 共享工作内存分配器

这是设备侧 native CSF 的协作式分配器，实际在 FEMU 进程/CU 线程中运行。所有算子动态链接同一个 `libcs-workmem.so`，共享一个有限内存池；不修改 FEMU、NVMe 命令或 `femu_csd_args`。

## 内存与接口

- 默认 64 MiB；由容器环境变量 `CSD_WORKMEM_SIZE_MB` 配置，也可在第一次使用前调用 `cs_work_init(bytes)`。初始化后拒绝改变容量。
- 一次 mmap 建立有限 arena，块头与对齐消耗计入池内占用；后续分配只切分/合并 arena，不额外向系统申请工作区。
- 全局互斥锁保护块链和所有计数，限制所有 CU 的合计用量；最多 256 个并存 context。
- context 是不可解引用、带递增标识的 opaque token。支持可选的每 context 上限，但不预留独占份额。
- 支持 alloc/calloc/realloc/free/end；end 批量回收。无空间返回 NULL/ENOMEM，错误句柄/归属返回 EINVAL，context 数耗尽返回 ENOSPC。
- 返回地址按 `max_align_t` 对齐；需要更高对齐的 SIMD 缓冲暂不支持额外 aligned_alloc 接口。
- realloc 扩容保守地同时计入旧块与新块；失败保留旧块。缩小可以归还尾部。free 支持相邻空闲块合并。
- 查询全局/context 用量、请求字节、峰值、最大连续空闲 payload。`free_bytes` 包含空闲块头，不能直接等同于可申请的单块大小。

```c
#include "cs_workmem.h"
cs_work_context *ctx = cs_work_begin();
if (!ctx) { /* 处理错误 */ }
void *buffer = cs_work_alloc(ctx, bytes);
if (!buffer) { /* 缩小 chunk、分区、spill 或报告资源不足 */ }
/* 处理输入，输出写入 AFDM。 */
cs_work_end(ctx);
```

```bash
cc -shared -fPIC operator.c -Icsd-workmem/include -Lbuild \
   -lcs-workmem -Wl,-rpath,'$ORIGIN' -o build/operator.so
```

头文件只声明接口。库实现和静态内存池状态只编译进公共动态库；不要把 workmem.c/静态库复制链接到各个算子，也不要用独立 dlmopen namespace 分别加载。公共库和算子位于同一 build 目录，SONAME 为 libcs-workmem.so，算子 RUNPATH 为 `$ORIGIN`，不需要全局 LD_LIBRARY_PATH。

## 范围

FDM 独立限制输入输出数据；工作池限制经本接口申请的私有计算工作区。库的固定控制表、pthread/CU 栈、代码段、普通 malloc/new/mmap、第三方未接入的分配都不计入这个池；这不是任意 native 代码的完整进程内存硬限制或安全沙箱。算子必须处理失败并在退出路径结束 context。context 不允许在仍有线程使用其指针时结束。

同一 FEMU 进程中的全部使用者共享池。当前按单设备实验配置设计；多独立设备进程各自有自己的池，不提供跨进程共享。最后一个依赖库卸载时销毁池，之后重新加载将重新初始化。禁止留下仍需使用的工作区再卸载其全部依赖；CSF 间交接使用 AFDM。

## 构建与测试

```bash
make -C code/csd-programs/csd-workmem check
```

默认输出到 `code/csd-programs/build`。单元检查覆盖有限容量、对齐、元数据记账、分块合并、失败的 realloc 保留数据、context 自动回收和 8 线程压力。

两个独立的 `csd-workmem-probe-a.so/b.so` 都链接同一个公共库。RTLD_LOCAL 测试验证共享 16 MiB 总预算，两个 6 MiB 分配后第三个失败，跨库释放后重新申请成功。

真实 FEMU 验证客户端在 [apps/csd-workmem-test](../../apps/csd-workmem-test/README.md)。四个不同程序 ID 在四个 CU 上运行、覆盖两个 .so；每个申请池容量的 3/8，通过 AFDM 屏障保证同时持有，预期仅两个成功。执行两轮检查预算回收。
