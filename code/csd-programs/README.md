# CSD 设备程序

这里的 native `.so` 在容器内编译，由 FEMU CU 线程执行，逻辑上运行在 CSD CPU。Guest 应用源码位于 `code/apps`，两者通过 NVMe 交互。

```text
csd-programs/
├── Makefile          # 汇总构建并检查统一动态库依赖
├── mk/               # native CSF 公共构建规则及 ELF 检查
├── examples/         # 原生 vadd、original-kernels、BPF、原生 ABI 头及独立样例规则
├── sina-cs-api/      # SNIA API 的示例 CSF
├── csd-workmem/      # 共享工作内存库、探针、测试和文档
└── build/           # 所有 .so、可选 BPF 及本地测试程序，不提交 git
```

每个独立程序/组件一个目录，保存自己的源码、Makefile 和说明；根 Makefile 自动发现一级子目录中的 Makefile。各目录接受 `BUILD_DIR`，默认统一输出到 `build`。

## 所有 native 算子的统一内存规范

**所有 native CSF `.so` 均必须动态链接同一个 `libcs-workmem.so`，即使当前没有动态工作区。** 分配器库本身不依赖自己；可选 BPF `.o` 不属于 native 动态库。

动态工作内存必须包含 `cs_workmem.h`，通过 `cs_work_alloc/calloc/realloc/free` 申请和释放。每次执行创建 context，所有成功、失败和提前退出路径都必须调用 `cs_work_end`；处理内存不足时可缩小 chunk、分区、spill 或返回错误。

**链接库不会自动拦截 malloc/new。** Hash 表、排序缓冲、解码数组等显式动态工作区不得绕过分配器使用 malloc/calloc/realloc/new/mmap。C++ STL 的大工作数组需要接入自定义 allocator，不能直接使用默认 std::allocator。输入规模决定的大工作区也不应转移到栈/VLA/alloca。

允许少量有固定上限的栈上临时变量。CU 栈、代码、库固定控制表，以及原生样例中 std::thread/vector、LZ4 等运行库内部的分配仍不属于工作池；原生样例保留兼容行为，统一链接不意味着这些内部申请已经完成接管。第三方组件的大工作区必须接入分配接口，无法接入的部分需明确记录，不能宣称已受池配额限制。

FDM 只用于约定的输入、输出和交接数据；CSF 私有工作区由本库管理，不以 FDM 代替。多个 CU/不同 .so 在同一个 FEMU 进程中共享一份预算；不要把 workmem.c 或静态库复制链接进各个算子，也不要通过不同 dlmopen namespace 加载多份分配器。

```c
#include "cs_kernel.h"   /* 原生 femu_csd_args 定义及可选参数解码器 */
#include "cs_workmem.h"

int64_t operator_main(struct femu_csd_args *args)
{
    (void)args;
    cs_work_context *ctx = cs_work_begin();
    if (!ctx)
        return -1;
    void *buffer = cs_work_alloc(ctx, 4096);
    if (!buffer) {
        cs_work_end(ctx);
        return -2;
    }
    /* 使用 buffer 计算，将最终输出写入约定的 AFDM。 */
    cs_work_end(ctx);  /* 批量释放该 context 的全部工作区 */
    return 0;
}
```

不要在每个算子中重新配置全局池容量；容量由 `CSD_WORKMEM_SIZE_MB` 统一指定。可用 cs_work_begin_limit 设置单 context 上限，但仍与其他 context 共用同一个总池。

## 新增算子的构建方式

每个组件 Makefile 包含 `../mk/csf.mk`，使用其中的头文件路径、库依赖和链接参数。例如：

```make
CC ?= cc
include ../mk/csf.mk
.PHONY: all clean
all: $(OUT)/my-operator.so
$(OUT):
	mkdir -p $@
$(OUT)/my-operator.so: src/operator.c $(CSF_BUILD_DEPS) | $(OUT)
	$(CC) $(CSF_CPPFLAGS) -I../../apps/sina-cs-api/include -O2 -fPIC \
	    -shared $< $(CSF_LDLIBS) -o $@.tmp
	mv -f $@.tmp $@
clean:
	rm -f $(OUT)/my-operator.so
```

公共规则保证：

- 独立构建组件时，缺失的分配器库会先构建；根目录并行构建时先完成公共库。
- `DT_NEEDED` 中保留 libcs-workmem.so，即使算子没有调用其接口。通过 push-state/no-as-needed/pop-state 防止链接器删除未引用依赖，也不改变其余库的链接状态。
- 统一使用 `$ORIGIN` 查找同目录的公共库，不把宿主绝对构建路径写入算子。
- 先生成临时 .so，再原子替换文件，避免原地截断可能仍被 FEMU 映射的旧文件。
- 根目录 `make all` 与 `make check-links` 检查 build 顶层每个 native 算子的实际 ELF 依赖及搜索路径；缺失则构建失败。

## 构建

```bash
# 宿主或容器内均可；original-kernels 需要 liblz4 开发包。
make -C code/csd-programs -j4
make -C code/csd-programs check

# 从宿主在容器编译；无需重编译 FEMU。
docker compose exec -T femu-csd make -C /workspace/code/csd-programs -j4
```

也支持 `make -C code/csd-programs examples`、`csd-workmem`、`sina-cs-api`，以及原有 `csd-vadd.so` / `sina-cs-scale.so` 目标别名。无论入口在哪，产物都进入 build。`make bpf` 为可选目标，依赖带 BPF 后端的 clang。

Compose 将 `CSD_PROGRAM_DIR` 设置为 `/workspace/code/csd-programs/build`。加载时仍只传文件名，例如 `csd-vadd.so`，不传宿主/容器完整路径。公共库与依赖它的算子放在同一 build 目录，算子以 `$ORIGIN` 查找共享库。

Compose 使用挂载的 `scripts/container/start-csd.sh` 和当前源码的 run-csd.sh；后者通过 `sudo env` 显式传入工作池参数，避免 sudo 丢弃环境变量。无需因本次设备程序改动重建 FEMU 镜像。

`CSD_WORKMEM_SIZE_MB=64` 控制所有协作式 CSF 共享的工作池，与 `CSD_FDM_SIZE=64` 独立。详细接口和限制见 [分配器说明](csd-workmem/README.md)。普通 malloc/new、栈和未接入的第三方分配不在工作池限制内。

## 测试

- 原生命令、vadd 和 kernels：[examples](examples/README.md)。
- Guest SNIA batch：`tests/sina-cs-api-test.sh [unit|build|smoke]`。
- 工作内存：`tests/csd-workmem-test.sh [unit|build|live]`，live 需要 4 CU；以两个独立 .so 验证共享总预算、超额拒绝与恢复。

## legacy 与 exports.map

`build/legacy/` 是迁移时为了保留旧实验二进制而建立的本机归档，包含旧 PG 分支和整理前的 .so。它不是项目必需目录，干净构建不会创建它；其中的旧文件不参与构建、依赖检查或默认加载，也不保证符合当前 ABI/链接规范。当前 make clean 不删除该归档。

`csd-workmem/src/exports.map` 是 GNU 链接器的符号版本脚本，构建公共库时通过 `--version-script` 使用：

```text
CSD_WORKMEM_1.0 {
    global: cs_work_*;
    local: *;
};
```

它将 cs_work_* 作为公共 API 导出，其他符号限制为库内可见，并为导出接口标记 CSD_WORKMEM_1.0 ABI 版本。它不负责分配或限制内存，不是另一个设备算子，也不会生成额外运行进程。
