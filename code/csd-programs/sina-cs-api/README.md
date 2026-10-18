# SNIA API 示例设备算子

`scale.c` 实现 scale、标量求和、128 位参数检查及延时探针。源码在容器内编译，产物为统一 `build/sina-cs-scale.so`，由 FEMU CU 执行。

```bash
make -C code/csd-programs/sina-cs-api
```

采用 Guest API 库提供的可选 `cs_kernel_decode`，从最后一个 AFDM 输入 MR 解码类型化参数；FEMU 原生 ABI 不变。Guest 应用与测试位于 `code/apps/sina-cs-api`。
