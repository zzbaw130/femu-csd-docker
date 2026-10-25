# CSD 共享工作内存

这是设备侧 native CSF 的协作式分配器，实际运行在 FEMU 进程/CU 线程中。不同算子动态链接同一个 `libcs-workmem.so`，共享有限的私有工作内存；FDM 单独用于输入和输出。

目录直接保存 `cs_workmem.h`、`cs_workmem.c`、CMakeLists.txt 和 exports.map；tests/ 保存模拟分配、跨SO共享配额、设备探针及 Guest 客户端。exports.map 仅将 cs_work_* 作为公共接口导出并标记 CSD_WORKMEM_1.0，不参与内存计量。

## 使用

```c
#include "cs_workmem.h"
cs_work_context *ctx = cs_work_begin();
if (!ctx) { /* 处理失败 */ }
void *buffer = cs_work_alloc(ctx, bytes);
if (!buffer) { /* 缩小 chunk、分区、spill 或返回资源不足 */ }
/* 计算，将输出写入 AFDM。 */
cs_work_end(ctx);
```

- 默认64MiB，由容器环境变量 `CSD_WORKMEM_SIZE_MB` 配置；首次使用前也可调用 cs_work_init(bytes)，初始化后不能改变容量。
- 一次 mmap 建立有限 arena，块头和对齐计入配额；互斥锁保护分配链及全部 CU 的总计数。
- 一次完整任务创建 context，可跨多次 CSF 入口保留；cs_work_end 批量释放。context 是不可解引用的 opaque token，最多256个。
- cs_work_begin_limit 可限制单 context，但不为它预留独占内存。
- cs_work_begin_child(parent, limit) 创建查询内的子 context；每次分配同时计入自身、父级和全局池的限额，供多个排序或索引节点共享一个查询预算。先结束子 context；仍有子级时结束父级返回 EBUSY。
- alloc/calloc/realloc 无空间返回 NULL/ENOMEM；错误句柄或归属返回 EINVAL，context 数量耗尽返回 ENOSPC。
- 地址按 max_align_t 对齐；realloc 优先利用紧邻的空闲块原地扩容，只计入新增空间，仍检查当前及父 context 的上限。无法原地扩容时才同时计入旧块和新块，失败保留旧块；free 合并相邻空闲块。
- cs_work_get_stats / cs_work_get_context_stats 查询容量、占用、峰值与最大连续空闲块。free_bytes 含空闲块头，不等于可申请的单块大小。

## 编译与链接

在 csd-programs 根目录构建，公共规则自动为所有算子添加头文件、共享库和 `$ORIGIN`：

```sh
cmake -S code/csd-programs -B code/csd-programs/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build code/csd-programs/build -j2
ctest --test-dir code/csd-programs/build -R workmem --output-on-failure
```

新增算子使用根目录的 `add_csd_program(name source.c)`；具体方式见[设备程序说明](../README.md)。不要把 cs_workmem.c 或静态库分别链接到每个 SO，不要用独立 dlmopen namespace 加载多份库。库的 SONAME 是 libcs-workmem.so，算子和库放在同一构建目录。

## 使用规范与范围

所有显式动态算法工作区必须使用本接口。普通 malloc/new/mmap、栈、代码、固定控制表和未接入的第三方分配不受此配额约束；链接不会自动拦截系统分配器，因此这是协作式配额。

context 结束前，使用其指针的线程必须全部停止；不要在仍需后续使用工作区时卸载全部依赖库。跨 CSF 保留的算法状态应由明确的任务所有者负责销毁；输入输出与交接数据使用 AFDM。

同一 FEMU 进程内共享池，独立设备进程各自拥有池；不提供跨进程共享。最后一个库依赖卸载时销毁池，重新加载会重新初始化。不要在单个算子入口反复配置全局容量。
