# SNIA API 示例设备算子

`scale.c` 实现 scale、标量求和、128 位参数检查及延时探针。源码在容器内编译，产物为统一 `build/sina-cs-scale.so`，由 FEMU CU 执行。

```bash
make -C code/csd-programs/sina-cs-api
```

直接读取原生 femu_csd_args 的 MR 和 cparam1/2，应用按各函数约定传参，没有额外参数解码。cs_spill_* 示例在入口用 csIsReentry 判断，内部 I/O 完成后从 csReentryValue 取回 spill_id，再更新私有状态。Guest 应用与测试位于 `code/apps/sina-cs-api`。

构建统一链接 `libcs-workmem.so` 并设置 `$ORIGIN`，即使当前算子只使用 AFDM 和固定栈变量。新增动态工作区必须使用 cs_work_* 接口。
