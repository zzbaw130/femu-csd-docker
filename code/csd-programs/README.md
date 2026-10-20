# CSD 设备程序

本目录的 native `.so` 在容器内编译，在 FEMU CU 线程执行，逻辑上属于 CSD CPU。Guest 应用源码在 `code/apps`；Guest 中的 SNIA 库、设备适配插件与这里的设备算子是不同产物。

```text
csd-programs/
├── CMakeLists.txt       # 公共编译、workmem 链接和输出规则
├── examples/            # 原生 vadd/kernels、sort、avg
│   ├── CMakeLists.txt
│   └── tests/           # 排序、重入和归并测试
├── csd-workmem/
│   ├── cs_workmem.h/.c  # 共享工作内存接口与实现
│   ├── exports.map      # 限定公共导出符号
│   ├── CMakeLists.txt
│   └── tests/           # 分配、共享配额、设备客户端与探针
└── build/               # CMake 生成文件、所有 .so 和本地测试程序
```

## 编译

需要 C/C++ 编译器、CMake 3.16+、pthread 和 liblz4 开发包。在容器内构建全部程序：

```sh
docker compose exec -T femu-csd sh -c \
  'cmake -S /workspace/code/csd-programs -B /workspace/code/csd-programs/build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build /workspace/code/csd-programs/build -j2'
```

也可以在宿主编译；宿主产物用于本地测试，实际设备程序应在运行 FEMU 的容器内编译，避免系统库版本不一致：

```sh
cmake -S code/csd-programs -B /tmp/csd-programs-check -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/csd-programs-check -j2
ctest --test-dir /tmp/csd-programs-check --output-on-failure
```

所有 SO 直接输出到指定构建目录顶层。Compose 的 `CSD_PROGRAM_DIR` 为 `/workspace/code/csd-programs/build`；加载时只传 `cs-example-sort.so` 等文件名。可选 BPF 用 `cmake --build code/csd-programs/build --target csd-vadd-bpf`，需要 clang BPF 后端。

## 新增算子

每个程序使用自己的目录和 CMakeLists.txt，并在根 CMakeLists.txt 加入 `add_subdirectory(my-program)`。子目录只需：

```cmake
add_csd_program(my-operator operator.c)
```

根目录的 `add_csd_program` 统一设置原生参数头路径、共享分配器依赖、pthread 以及 `$ORIGIN` 搜索路径。产物为 `my-operator.so`，不会额外增加 lib 前缀。算子不自行复制分配器实现。

## 统一内存规范

**所有 native CSF SO 都动态链接同一个 libcs-workmem.so，即使当前没有动态工作区。** 公共构建函数保留此依赖；分配器自身和 BPF 对象不适用。

动态算法工作区包含 `cs_workmem.h`，使用 `cs_work_alloc/calloc/realloc/free`；为任务创建 context，可跨重入保留，结束或失败时调用 `cs_work_end`。Hash 表、排序缓冲和输入规模相关的大数组不得绕过分配器使用 malloc/new/mmap、VLA 或 alloca。C++ 大型 STL 容器需要接入相应分配器，不能默认向系统堆申请。

```c
#include "femu-csd-kernel.h"
#include "cs_workmem.h"
int64_t operator_main(struct femu_csd_args *args)
{
    (void)args;
    cs_work_context *ctx = cs_work_begin();
    if (!ctx) return -1;
    void *buffer = cs_work_alloc(ctx, 4096);
    if (!buffer) { cs_work_end(ctx); return -2; }
    /* 使用工作区，将输出写入约定的 AFDM。 */
    cs_work_end(ctx);
    return 0;
}
```

AFDM 保存输入、输出和交接数据；私有算法状态使用工作内存。全部 CU/不同 SO 在同一个 FEMU 进程内共享预算。禁止将 cs_workmem.c 或静态副本链接到各算子，也不要用独立 dlmopen namespace 加载多份分配器。

`CSD_WORKMEM_SIZE_MB=64` 与 `CSD_FDM_SIZE=64` 独立配置。链接不会拦截 malloc/new；固定 CU 栈、代码、少量有界临时变量，以及未接入的第三方运行库内部申请不计入工作池。原生 kernels 中的 thread/vector/LZ4 保留原有行为，不能据此宣称其全部内存已受配额限制。

## 示例与测试入口

- [原生和 SNIA 设备示例](examples/README.md)。
- [分配器使用规范](csd-workmem/README.md)。
- `bash tests/snia-cs-api-test.sh unit`：本地设备程序与 SNIA 测试。
- `bash tests/snia-cs-api-test.sh build`：容器设备程序与 Guest SDK。
- `bash tests/snia-cs-api-test.sh example`：Guest 256MiB 文件完整排序后分批计算平均值。
- `bash tests/csd-workmem-test.sh unit|build|live`：工作内存模拟或真实设备测试。
