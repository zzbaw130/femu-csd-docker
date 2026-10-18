# 原生 FEMU CSD 算子样例

本目录保存原生 vadd、original-kernels（knn/sql/grep/lz4）、BPF 源码及未扩展的 `femu-csd-kernel.h`。这些程序在容器内编译、FEMU CU 线程中执行；Guest 的调用工具位于 `code/apps/csd-passthru`。

## 构建与加载

```bash
make -C code/csd-programs/examples
# 或
./code/csd-programs/examples/build.sh
```

产物统一为 `code/csd-programs/build/csd-vadd.so` 和 `csd-original-kernels.so`。C++ kernels 依赖 liblz4 开发包。Compose 已将设备加载目录指向该 build 目录，Guest 只传文件名：

```bash
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 smoke
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 smoke-so csd-vadd.so
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 smoke-so-all csd-original-kernels.so
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 smoke-mrs csd-vadd.so
sudo /home/femu/apps/csd-passthru/csd-passthru /dev/nvme0n1 vadd-example csd-vadd.so
```

## 原生 ABI

```c
int64_t csf(struct femu_csd_args *args);
```

头文件保留原来的 numr、mr_addr、mr_len、两个 cparam、data_buffer 和 buffer_len，不增加 SNIA 或 SQL 字段。样例的 MR[0] 是输出，MR[1] 是输入；nsid=0 的 descriptor 以 sb 保存 AFDM ID，len=0 表示完整分配。

命令仍为原生 B0/C0/D0/E1/F2/F5，以及 admin 22/25 加载和 23 激活。MRS 使用 admin 21。共享库载荷为 `文件名\0入口符号\0`，文件必须位于 csd_program_dir。

## 可选 BPF

```bash
make -C code/csd-programs bpf
```

输出 `build/csd-vadd.bpf.o`，需要 clang BPF 后端和启用 uBPF 的 FEMU。默认 all 不构建 BPF。

更多 Guest 命令参数可运行 `csd-passthru` 查看帮助；批量复制、执行、读取的示例用于功能回归，不作为精确带宽基准。
