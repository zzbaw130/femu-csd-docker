# FEMU CSD 实验环境（Docker）

在 Docker 里构建并运行 [FEMU](https://github.com/MoatLab/FEMU)（QEMU 10.1 基座）的
**CSD（Computational Storage，`femu_mode=4`）** 模式：模拟盘、算子加载和程序执行都在
宿主机的 QEMU 进程里，guest 里只跑应用，通过 virtio-9p 与宿主机共享目录。

当前模拟设备（`code/FEMU/hw/femu/scripts/run-csd.sh` 顶部参数）：

| 项 | 值 | 说明 |
| --- | --- | --- |
| 暴露容量 `ssd_size` | 49152 MiB = 48 GiB | guest 里 `nvme list` 显示 51.54 GB |
| NAND 原始容量 | 64 GiB | `blks_per_pl=1024`，其余 16 GiB 作 OP（占原始 25%） |
| `fdm_size` | 4096 MiB | CSD 设备内存（guest 按需 Allocate） |
| `nr_cu` / `csf_runtime_scale` | 8 / 1 | 8 个计算单元 |
| guest 内存 | 8 GiB | `-m 8G` |

---

## 1. 环境要求

- x86_64 Linux，**必须有可用的 `/dev/kvm`**（FEMU 用 KVM 启动 guest）
- Docker + docker compose v2
- 内存：约 **60 GiB 空闲**（48 GiB 模拟盘 + 8 GiB guest + 最多 4 GiB FDM + 开销）
- 磁盘：镜像约 2.8 GB，guest 镜像约 2.8 GB（都在 `images/`）
- 网络：首次 `docker compose build` 需要联网（apt + QEMU configure 拉 subprojects）；
  首次启动做 guest 镜像时需要联网下载 Ubuntu cloud image

## 2. 从零构建并启动

```bash
# 1) 构建镜像（编译 FEMU，首次约 3~5 分钟）
docker compose build

# 2) 启动：首次会自动制作 guest 镜像（下载 Ubuntu 24.04 cloud image + cloud-init
#    provisioning，约 2~4 分钟），之后启动 CSD 模式的 VM
docker compose up -d

# 3) 观察启动过程 / 进串口控制台（Ctrl-C 只退出日志，Ctrl-A X 退出 QEMU）
docker compose logs -f femu-csd

# 4) 连接 guest
./scripts/run-guest-ssh.sh
```

**完全从零重建**（换参数、想重做 guest 镜像时）：

```bash
docker compose down
docker rmi femu-csd:latest
rm -rf images            # 若报 Permission denied（目录属主是容器 root），改用下面这条
# docker run --rm -v "$PWD:/w" alpine:3.20 rm -rf /w/images
docker compose build     # 想连编译缓存一起清掉：docker compose build --no-cache
docker compose up -d     # 重新 provisioning + 启动
```

> 只改 `run-csd.sh` 里的 QEMU/设备参数 → 只需 `docker compose build` + 重启容器；
> 改了 `make-guest-image.sh`（guest 侧挂载/目录）→ 必须先删 `images/u20s.qcow2` 再启动，
> 否则新配置不会进 guest 镜像。

## 3. 目录结构

### 3.1 宿主机（本仓库）

```
.
├── Dockerfile                     # ubuntu:24.04 + 编译 FEMU + 启动时制作 guest 镜像
├── docker-compose.yml             # /dev/kvm、memlock、8080、四个共享目录挂载
├── code/
│   ├── FEMU/                      # FEMU 源码（无 .git，直接由本仓库跟踪；在此改代码）
│   ├── apps/csd-passthru/         # 运行在 guest 上的应用源码
│   └── csd-programs/              # CSD 算子源码 + build.sh，在容器里编译成 .so
├── scripts/run-guest-ssh.sh       # 快捷登录 guest
├── tests/passthru-test.sh         # 快捷跑 smoke test
├── results/                       # 实验输出（共享给 guest）
└── images/                        # guest 镜像 u20s.qcow2、SSH 密钥、下载缓存（gitignore）
```

### 3.2 容器内部

```
/root/femu/                        # FEMU 源码及构建目录（COPY 进镜像）
└── build-femu/                    # qemu-system-x86_64、run-csd.sh、make-guest-image.sh、log
/root/workspace/                   # 共享目录的挂载点（来自 compose volumes）
├── code/                          #   ← 宿主机 ./code
│   ├── FEMU/                      #      只给容器编译用，不导出给 guest
│   ├── apps/                      #      以 mount_tag=apps 导出
│   └── csd-programs/              #      csd_program_dir：容器内 make 出 .so，guest 看不到
├── scripts/                       #   ← 宿主机 ./scripts（同名 tag 导出）
├── tests/                         #   ← 宿主机 ./tests
└── results/                       #   ← 宿主机 ./results
/root/images/                      #   ← 宿主机 ./images（guest 镜像 + SSH 密钥）
```

工作目录（`WORKDIR`）是 `/root/femu/build-femu`，`CMD` 会用相对路径执行
`./make-guest-image.sh` 和 `./run-csd.sh`。

### 3.3 guest 内部

```
/home/femu/
├── apps/        ← 9p: 宿主机 ./code/apps   （guest 侧应用：csd-passthru 等）
├── scripts/     ← 9p: 宿主机 ./scripts
├── tests/       ← 9p: 宿主机 ./tests
└── results/     ← 9p: 宿主机 ./results
```

guest 里**没有** FEMU 源码，也**没有** `csd-programs`：算子 `.so` 由宿主机上的 QEMU
进程从 `csd_program_dir` 加载（guest 只按文件名下发加载命令，不能给路径）。

## 4. 相对上游 FEMU 的脚本改动

### 4.1 `code/FEMU/hw/femu/scripts/run-csd.sh`（启动脚本）

| 项 | 上游默认 | 现在 | 说明 |
| --- | --- | --- | --- |
| `blks_per_pl` | 256 | **1024** | NAND 原始容量 16 GiB → 64 GiB |
| `ssd_size`（`devsz_mb`） | 4096 | **49152** | 暴露 48 GiB，余 16 GiB 作 OP |
| `fdm_size` | 64 | **4096** | CSD 设备内存 4 GiB |
| `nr_cu` | 4 | **8** | 计算单元数 |
| `csf_runtime_scale` | 3 | **1** | 程序运行时间缩放 |
| guest 内存 | `-m 4G` | **`-m 8G`** | |
| `csd_program_dir` | 不设置 | **`/root/workspace/code/csd-programs`** | 容器侧目录，**不共享**给 guest |
| 9p 导出 | 无 | 新增 **4 条 `-virtfs`** | `apps`/`results`/`scripts`/`tests`，`security_model=mapped-xattr` |

上游脚本没有任何 9p 导出，`csd_program_dir` 也未设置（只能跑内置 phantom 程序）；
现在 4 个目录各自独立导出，guest 按 `mount_tag` 挂到 `/home/femu/<tag>`。

### 4.2 `code/FEMU/hw/femu/scripts/make-guest-image.sh`（guest 镜像制作）

cloud-init 的 user-data 里新增了三块内容：

1. **`/etc/modules-load.d/femu-9p.conf`**：开机预加载 `9pnet_virtio`。
   不加载的话，systemd 处理 fstab 时 virtio-9p 设备还没 probe 完，挂载会失败并打印
   `9pnet_virtio: no channels available for device X`（有 `nofail`，是静默失败）。
2. **`/etc/fstab` 追加 4 行 9p 挂载**（`nofail,_netdev,msize=1048576`）：
   `apps`/`results`/`scripts`/`tests` → `/home/femu/<同名>`，开机自动挂载。
   `nofail` 保证不带 `-virtfs` 启动时也能正常开机，`_netdev` 再推迟到设备就绪之后。
3. **`runcmd` 里 `mkdir -p` + `chown femu:femu`** 这 4 个挂载点
   （用显式路径列表，因为 Ubuntu 的 `/bin/sh` 是 dash，不支持 `{a,b}` 花括号展开）。

### 4.3 `Dockerfile`

- 依赖增加 `liblz4-dev`（编 `csd-original-kernels.so` 需要 `-llz4`）
- `COPY code/FEMU /root/femu`，在 `/root/femu/build-femu` 里执行 `femu-compile.sh`
- `CMD`：启动时若 `/root/images/u20s.qcow2` 不存在，先用
  `make-guest-image.sh --packages build-essential` 制作（guest 里带 gcc 便于编 csd-passthru），
  然后 `exec ./run-csd.sh`

> 为什么不在 `docker build` 阶段做 guest 镜像：`RUN` 步骤**无法传入 `/dev/kvm`**，
> 只能用 `--no-kvm`（TCG，10~30 分钟）；而且构建期做出的镜像会落在镜像的
> `/root/images/`，运行时被 `./images` 挂载遮住。所以放在容器启动时做（KVM，约 2 分钟）。

## 5. 两个快捷脚本

### `scripts/run-guest-ssh.sh` — 快捷登录 guest

```bash
./scripts/run-guest-ssh.sh          # 进入 guest 的交互 shell（用户 femu，自带密钥）
```

内容就是 `docker compose exec femu-csd ./run-guest-ssh.sh`。
要执行单条命令（脚本本身不转发参数）用：

```bash
docker compose exec femu-csd ./run-guest-ssh.sh "uname -a"
# 或从宿主机直连：
ssh -i images/femu-guest-key -p 8080 femu@localhost "sudo nvme list"
```

### `tests/passthru-test.sh` — 快捷跑 smoke test

```bash
# 共享库算子：vadd
./tests/passthru-test.sh /dev/nvme0n1 smoke-so csd-vadd.so
# 原始 CEMU 算子集合：knn / sql / grep / lz4
./tests/passthru-test.sh /dev/nvme0n1 smoke-so-all csd-original-kernels.so
# 内置 phantom 程序（不需要 .so）
./tests/passthru-test.sh /dev/nvme0n1 smoke
```

它依次做三件事，参数（设备 + 子命令 + 参数）原样透传给 `csd-passthru`：

1. 容器里 `make -C /root/workspace/code/csd-programs`（编译算子 `.so`）
2. guest 里 `make -C /home/femu/apps/csd-passthru`（编译应用）
3. guest 里 `sudo /home/femu/apps/csd-passthru/csd-passthru <你的参数>`

子命令与算子 ABI 见 `code/csd-programs/README.md`（FEMU 自带的 CSD 测试说明）。

## 6. 常用操作

```bash
# 改算子源码后重新编译（产物在宿主机 code/csd-programs/*.so）
./code/csd-programs/build.sh

# 改 FEMU 源码后重新编译（容器内增量编译）
docker compose exec femu-csd bash -c 'cd /root/femu/build-femu && make -j$(nproc)'

# 改了脚本/依赖后重建镜像
docker compose build

# 停止（模拟盘在内存里，没有数据需要保存）
docker compose down
```

退出 VM：guest 内 `sudo poweroff`，或在串口控制台按 `Ctrl-A X`。

## 7. 注意事项

1. **`/dev/kvm` 必需**：compose 里已声明 `devices: /dev/kvm`；没有 KVM 时 FEMU 起不来，
   guest 镜像也无法制作（`docker build` 阶段同样拿不到 KVM）。
2. **删 `images/` 前先 `docker compose down`**：`./images` 是 bind mount，运行中删除会把
   目录从容器脚下抽走，provisioning 会以 `qemu-img: Could not open ... No such file or directory`
   失败，容器随后退出（表现就是「ssh 连不上」，因为 8080 没人监听）。
3. **改 `make-guest-image.sh` 必须重做 guest 镜像**：`docker compose build` →
   删 `images/u20s.qcow2` → `docker compose up -d`。
4. **guest 里写共享目录**：9p 用 `security_model=mapped-xattr`，宿主机文件属主/权限不变，
   guest 里的属主记在 xattr 上。若 `femu` 用户写不进去（目录在宿主机属于 root），
   一次性执行 `sudo chown -R femu:femu /home/femu/apps`（只影响 guest 看到的属主）。
5. **宿主机直连 ssh 的私钥**：`images/femu-guest-key` 由容器 root 生成（0600），宿主机上
   可能读不了（`Load key ... Permission denied`）。一次性修：
   `docker run --rm -v "$PWD/images:/x" alpine:3.20 chown "$(id -u):$(id -g)" /x/femu-guest-key`；
   不想管就用 `./scripts/run-guest-ssh.sh`。
6. **内存与 mlock**：48 GiB 盘 + 8 GiB guest，运行前确认空闲内存约 60 GiB；
   compose 的 `ulimits: memlock: -1` 让 FEMU 能把设备内存 `mlock` 住（否则会打印
   `cannot pin the ... memory backend`，时延会抖动）。
7. **挂载自检**：guest 里 `mount | grep 9p` 应该有 4 行；没有的话看
   `sudo dmesg | grep 9p`（fstab 用 `nofail`，失败不会阻塞开机）。
8. **算子安全性**：`.so` 会在宿主机的 QEMU 进程内以 root 执行，`csd_programs` 目录里
   只放自己编译/信任的程序。

## 8. 验证过的状态

```
[FEMU] Log: vCSD0,CSD mode initialized: fdm=4096MB, nr_cu=8, nr_thread=4
guest: sudo nvme list → /dev/nvme0n1  vCSD0  FEMU Computational Storage Controller  51.54 GB
guest: /home/femu → apps  results  scripts  tests（4 个 9p 挂载齐备）
./tests/passthru-test.sh /dev/nvme0n1 smoke-so csd-vadd.so
    → loaded shared-library CSF id=1 / shared-library smoke passed
./tests/passthru-test.sh /dev/nvme0n1 smoke-so-all csd-original-kernels.so
    → knn / sql / grep / lz4 shared-library smoke passed
```
