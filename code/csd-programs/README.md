# CSD 设备程序

这里的 native `.so` 在容器内编译，由 FEMU CU 线程执行，逻辑上运行在 CSD CPU。Guest 应用源码位于 `code/apps`，两者通过 NVMe 交互。

```text
csd-programs/
├── Makefile          # 自动汇总各程序目录的构建规则
├── examples/         # 原生 vadd、original-kernels、BPF、原生 ABI 头及独立样例规则
├── sina-cs-api/      # SNIA API 的示例 CSF
├── csd-workmem/      # 共享工作内存库、探针、测试和文档
└── build/           # 所有 .so、可选 BPF 及本地测试程序，不提交 git
```

每个独立程序/组件一个目录，保存自己的源码、Makefile 和说明；根 Makefile 自动发现一级子目录中的 Makefile。各目录接受 `BUILD_DIR`，默认统一输出到 `build`。

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

迁移时保留的旧分支编译产物位于本机 `build/legacy`，不参与构建或默认加载。`make clean` 只清理当前项目产物，不删除该归档。
