# FEMU CSD Passthrough Tests

This directory contains lightweight guest-side tools for validating FEMU CSD
vendor commands without `linux-cemu`, FDMFS, or a fixed VM image.

Build inside a normal Linux guest:

```bash
make
```

Run a basic AFDM smoke test against a namespace device:

```bash
sudo ./csd-passthru /dev/nvme0n1 smoke
```

The smoke test sends AFDM commands through `NVME_IOCTL_IO_CMD` and uses the
original CEMU-style admin lifecycle commands through `NVME_IOCTL_ADMIN_CMD`:

- allocate AFDM
- write AFDM
- read AFDM
- load and activate a phantom CSF
- execute the phantom CSF
- deactivate and unload the phantom CSF
- deallocate AFDM

Build also produces `csd-vadd.so`, a minimal shared-library CSF used by the
shared-library smoke path. The program load payload follows the original CEMU
descriptor format: a PRP data buffer containing `path\0symbol\0`.

The QEMU process on the host loads the program, so the file must be on the
host, in the directory named by the `csd_program_dir` property. The `path` in
the descriptor is a file name in that directory: FEMU refuses a name that
contains `/` or resolves outside the directory, and refuses every shared-library
and uBPF load when `csd_program_dir` is unset. `run-csd.sh` does not set it.

Build the programs on the host and copy them into one directory
(`csd-original-kernels.so` needs the lz4 development package):

```bash
cd hw/femu/tests/csd                 # in the FEMU source tree on the host
make csd-vadd.so csd-original-kernels.so
mkdir -p ~/csd-programs
cp csd-vadd.so csd-original-kernels.so ~/csd-programs/
```

Start FEMU with `csd_program_dir` pointing at that directory. With
`run-csd.sh`, add this line after the other `FEMU_OPTIONS` lines:

```bash
FEMU_OPTIONS=${FEMU_OPTIONS}",csd_program_dir=$HOME/csd-programs"
```

Then pass the file name alone in the guest:

```bash
sudo ./csd-passthru /dev/nvme0n1 smoke-so csd-vadd.so
```

`make` also builds `csd-original-kernels.so`, which contains small
shared-library ports of the original CEMU `knn`, `sql`, `grep`, and `lz4`
kernels. These tests exercise the same CSD program lifecycle and inline memory
range interface as the vadd test:

```bash
sudo ./csd-passthru /dev/nvme0n1 smoke-so-all csd-original-kernels.so
```

FDMFS-free MRS is available through the original CEMU memory range set
management command layout (`0x21`). The passthrough helper creates an MRS from
AFDM-backed memory range descriptors and executes a CSF by `rsid`:

```bash
sudo ./csd-passthru /dev/nvme0n1 smoke-mrs csd-vadd.so
sudo ./csd-passthru /dev/nvme0n1 vadd-example csd-vadd.so
```

The migrated sync-breakdown check measures NVM-to-AFDM copy, CSF execution, and
AFDM read as separate stages:

```bash
sudo ./csd-passthru /dev/nvme0n1 sync-breakdown csd-vadd.so 4096 16
```

The indirect vadd smoke keeps the original indirect CSF ABI shape and uses an
AFDM-backed MRS instead of FDMFS files:

```bash
sudo ./csd-passthru /dev/nvme0n1 indirect-vadd csd-vadd.so
```

A compact benchmark entry covers vadd plus the original kernel smoke set:

```bash
sudo ./csd-passthru /dev/nvme0n1 benchmark-kernels csd-vadd.so csd-original-kernels.so 1
```

The shared-library CSF ABI is:

```c
int64_t kernel(struct femu_csd_args *args);
```

The execute command uses a CEMU-style program execute command body:
`pind`, `numr`, `dlen`, `cparam1`, `cparam2`, `group`, and `runtime` are sent
in the command. Because this lightweight test path intentionally avoids MRS and
FDMFS, it sends inline memory ranges in the PRP data buffer. In those test
ranges, `nsid=0` means AFDM, `sb` is the AFDM id, and `len=0` means the full
AFDM allocation. The CSF ABI then sees `args->mr_addr[0]` as the output AFDM
and `args->mr_addr[1]` as the input AFDM.

Other useful command-level checks:

```bash
sudo ./csd-passthru /dev/nvme0n1 alloc 4096
sudo ./csd-passthru /dev/nvme0n1 create-group 5 0 0
sudo ./csd-passthru /dev/nvme0n1 set-qos <group-id> 6 0 0
sudo ./csd-passthru /dev/nvme0n1 exec <pind> <in-afdm-id> <out-afdm-id> 0 <group-id> <cparam1> <cparam2>
sudo ./csd-passthru /dev/nvme0n1 delete-group <group-id>
sudo ./csd-passthru /dev/nvme0n1 nvm-to-afdm <afdm-id> 0 0 0
sudo ./csd-passthru /dev/nvme0n1 bench 4096 32
sudo ./csd-passthru /dev/nvme0n1 bench 65536 16
```

新增设备内部 AFDM→NVM 命令 `0xd1`，与 NVM→AFDM 的 `0xd0` 使用同一参数布局：

```bash
sudo ./csd-passthru /dev/nvme0n1 afdm-to-nvm <afdm-id> <afdm字节偏移> <namespace起始LBA> <零基块数>
sudo ./csd-passthru /dev/nvme0n1 nvm-to-afdm <afdm-id> <afdm字节偏移> <namespace起始LBA> <零基块数>
```

`nlb=0` 表示一个逻辑块；512B namespace 中 `nlb=7` 表示 4KiB。
CDW10/11 是起始 LBA，CDW12 低 16 位是零基块数，CDW13 是 AFDM ID，
CDW14/15 是 AFDM 字节偏移。请求不携带 Guest 数据缓冲区，成功完成值为复制字节数。
写入会覆盖指定 LBA，spill 使用时应填入 `femu-spill` 分区内的 namespace 绝对地址。

两个方向均在控制器 FTL 线程调用公共读写路径，计入 NAND、通道排队和 GC 时延。
`0xd1` 使用 FUA，完成前计入介质写入开销，内部写不计为主机传输的写入页数。
当前不支持携带 namespace metadata 的格式或 power-loss 模式。
本工具直接调用设备命令；SNIA STORE 映射和临时空间管理见 [SNIA API 库](../snia/README.md)。

The `bench` command reports wall-clock average latency for AFDM write, AFDM
read, and NVM-to-AFDM copy. It is intended as a regression check for the CSD
command path, not a final paper-level benchmark harness.

FEMU CSD also accepts the original CEMU program lifecycle admin command
layouts for load/unload (`0x22`) and activate/deactivate (`0x23`). The
lightweight passthrough helper sends those commands to the controller device
without the CEMU kernel driver:

```bash
sudo ./csd-passthru /dev/nvme0 admin-load-phantom 1 1000
sudo ./csd-passthru /dev/nvme0 admin-load-so 1 csd-vadd.so csd_vadd
sudo ./csd-passthru /dev/nvme0 admin-load-ubpf 1 csd-vadd.bpf.o csd_vadd_bpf 0
sudo ./csd-passthru /dev/nvme0 admin-activate 1
sudo ./csd-passthru /dev/nvme0 admin-deactivate 1
sudo ./csd-passthru /dev/nvme0 admin-unload 1
sudo ./csd-passthru /dev/nvme0 admin-create-mrs <out-afdm-id> <in-afdm-id>
sudo ./csd-passthru /dev/nvme0 admin-delete-mrs <rsid>
```

The tool assumes FEMU was started with CSD mode enabled, for example:

```bash
-device femu,femu_mode=4,fdm_size=64,csd_program_dir=/path/to/csd-programs
```

It intentionally does not depend on CEMU's modified kernel driver or FDMFS. CSD
mode still uses FEMU's device-side BBSSD FTL path for normal NVM read/write
requests; the passthrough commands validate the additional computational
storage interface.

Shared-library CSF support is enabled in the default FEMU build. uBPF support
is optional because it depends on an external `ubpf` library. If `ubpf` is
installed through pkg-config, build FEMU with:

```bash
./femu-compile.sh --enable-csd-ubpf
```

If you use the `ubpf-cemu` source tree directly, pass its path explicitly:

```bash
./femu-compile.sh --enable-csd-ubpf=/home/<user>/CEMU-FEMU/ubpf-cemu
```

The guest helper does not build BPF objects by default. Build the BPF test
program on the host with Clang BPF support and copy it into `csd_program_dir`:

```bash
make bpf                             # on the host
cp csd-vadd.bpf.o ~/csd-programs/
```

Then, in the guest:

```bash
sudo ./csd-passthru /dev/nvme0n1 smoke-ubpf csd-vadd.bpf.o 0
sudo ./csd-passthru /dev/nvme0n1 smoke-ubpf csd-vadd.bpf.o 1
```
