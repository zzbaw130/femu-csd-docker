# FEMU CSD + PostgreSQL 实验环境

在 Docker 里构建并运行 [FEMU](https://github.com/MoatLab/FEMU)（QEMU 10.1 基座）的 **CSD 模式**
（`femu_mode=4`）：PostgreSQL 12（`code/apps/postgres`，REL_12_STABLE）跑在 guest 里，
数据盘是 FEMU 模拟的 NVMe SSD —— NAND 时序、FTL、GC 全部是模拟的，用来做 CSD / 存储相关实验
（TPC-H 等）。算子（`.so`）在容器里编译，由宿主机的 FEMU 进程加载执行。

## 1. 构建并启动

```bash
./scripts/build.sh              # 重建容器镜像 + 启动 VM（保留已有 guest 镜像，不碰 postgres）
./scripts/build.sh -i           # 额外删掉 guest 镜像 u20s.qcow2，重新做一遍 provisioning
./scripts/build.sh -p           # 额外：挂载模拟盘 → 编译安装 PostgreSQL → 载数据启库
./scripts/build.sh -ip          # 从零一条龙（首次构建用这个）
```

| 步骤 | 默认 | `-i` | `-p` |
| --- | :-: | :-: | :-: |
| `docker compose down --rmi local` | ✓ | | |
| 删 `images/u20s.qcow2`（强制重做 guest 镜像） | | ✓ | |
| `docker compose build` 编译 FEMU | ✓ | | |
| `docker compose up -d --wait` 启动 CSD VM（等 healthcheck：guest SSH 可用） | ✓ | | |
| guest `mount-nvme.sh`：模拟盘 `/dev/nvme0n1` mkfs.ext4 并挂到 `/data` | | | ✓ |
| guest `install-postgre.sh`：从 `~/apps/postgres` 编译安装 PostgreSQL 到 `~/opt/postgres/release` | | | ✓ |
| guest `start-postgre.sh 0.01`：解压 `data/tpch_s0.01.tar` 到 `/data` 并启动 `tpch_s0.01` | | | ✓ |

`images/u20s.qcow2` 不存在时（首次或用了 `-i`）会自动制作 guest 镜像：下载 Ubuntu 24.04 cloud image +
cloud-init，并顺带安装 `build-essential,zlib1g-dev,libreadline-dev,bison,flex`（guest 里编译 PG 需要），
耗时几分钟。

前置条件：x86_64 + **可用的 `/dev/kvm`**、docker compose v2、首次构建/首次启动需要联网；
内存按第 5 节预留（默认约 32 GiB 模拟盘 + 8 GiB guest + FDM ≈ 45 GiB）。

## 2. 快速验证

```bash
# CSD passthru smoke test
# 编译passthru程序以及相关so算子
./tests/passthru-test.sh build
# 开启测试
./tests/passthru-test.sh build <nvme dev> <options>

# 基于postgrs的tpch测试，需要build.sh的时候指定了-p
./tests/test-guest.sh tpch-test.sh

# ssh到guest内部
./scripts/ssh-guest.sh
```

想换数据规模：`./scripts/sh-guest.sh guest/start-postgre.sh -f 1`（`-f` 强制重新解压，SF 见 `data/`）。

## 3. 目录结构

### 宿主机（本仓库）

```
.
├── Dockerfile / docker-compose.yml / .dockerignore
├── scripts/
│   ├── build.sh                     # 构建/启动（-i 重做 guest 镜像，-p 顺带装并启动 PostgreSQL）
│   ├── sh-guest.sh                  # 在 guest 里执行 scripts/guest/<name>
│   ├── ssh-guest.sh / scp-guest.sh  # 进 guest / 往 guest 传文件
│   ├── gen-pgdata.sh                # 宿主机侧生成 data/tpch_s<SF>.tar（需要本机 duckdb + pg）
│   ├── create-tables.sql / create-indexes.sql
│   └── guest/                       # 只在 guest 内跑的脚本
│       ├── mount-nvme.sh            # 格式化并挂载模拟盘到 /data
│       ├── install-postgre.sh       # 编译安装 PostgreSQL
│       └── start-postgre.sh         # 解压数据 + 启库
├── tests/
│   ├── test-guest.sh                # 在 guest 里执行 tests/guest/<name>
│   ├── passthru-test.sh             # CSD passthru smoke test
│   └── guest/
│       ├── tpch-test.sh             # TPC-H 22 条（guest 内 psql）
│       └── queries/                 # 1..22 的 <n>.sql 与 <n>.explain.sql
├── code/
│   ├── FEMU/                        # FEMU 源码（改这里，容器里编译）
│   ├── apps/
│   │   ├── postgres/                # PostgreSQL 12 源码（guest 里编译）
│   │   └── csd-passthru/            # guest 侧 CSD 测试程序
│   └── csd-programs/                # CSD 算子源码，容器里编成 .so 由 FEMU 加载
├── data/                            # TPC-H pgdata tar（tpch_s<SF>.tar，仓库只带 SF=0.01）
├── results/                         # 实验结果（共享给 guest，按数据库名分子目录）
└── images/                          # guest 镜像、SSH key、下载缓存（不入库）
```

### 容器内

```
/workspace/FEMU/                    # FEMU 源码 + build-femu/（qemu-system-x86_64、脚本副本）
/workspace/{code,data,results,scripts,tests}/   # ← 同名宿主机目录（只有 code/apps 导出给 guest）
/root/images/                       # ← ./images（guest 镜像 u20s.qcow2 + SSH key，落在宿主机）
```

### guest 内（`/home/femu`）

```
apps/     ← 9p: 宿主机 code/apps      （postgres 源码、csd-passthru）
data/     ← 9p: 宿主机 data           （tpch_s<SF>.tar）
results/  ← 9p: 宿主机 results        （实验结果直接落回宿主机）
scripts/  ← 9p: 宿主机 scripts/guest  （guest 内脚本）
tests/    ← 9p: 宿主机 tests/guest    （tpch-test.sh + queries/）
/data                               # 模拟 SSD（/dev/nvme0n1, ext4），PGDATA 在这里
```

## 4. 设计要点（相对上游 FEMU 的改动）

- **编译分层**：`Dockerfile` 把「基本不变的 FEMU 源码」和「频繁改的脚本」分成两层 ——
  FEMU 用主 context 编译，`hw/femu/scripts` 与 `femu-scripts` 通过 compose 的
  `additional_contexts` 单独 COPY（`.dockerignore` 里把这两个目录从主 context 排除）。
  改脚本只重跑最后两层，**不再重编 FEMU**。
- **`run-csd.sh`**：设备参数全部改成读环境变量（见第 5 节）；新增 5 条 `-virtfs`
  导出（`apps/data/results/scripts/tests`，`security_model=passthrough`）；
  `csd_program_dir` 指向容器内的 `code/csd-programs`（guest 看不到算子目录）。
- **`make-guest-image.sh`**：cloud-init 里预加载 `9pnet_virtio`（否则开机挂载会和
  virtio-9p 设备 probe 竞争，报 `no channels available`），`/etc/fstab` 追加 5 条 9p 挂载
  （`nofail,_netdev`），并建好/授权对应挂载点。
- **compose**：`healthcheck` 保证 `up -d --wait` 真的等到 guest 可用；`ulimits: memlock: -1`
  让 FEMU 能锁住设备内存。

## 5. 可调参数（docker-compose.yml 的 `environment`）

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `FEMU_SMP` | 8 | guest vCPU 数 |
| `FEMU_MEM` | 8G | guest 内存 |
| `CSD_NVM_SIZE` | 32768 | **暴露给 guest 的SSD容量**（MB）。NAND 原始容量固定 64 GB；由于需要有25% OP，因此size最大为49152MB |
| `CSD_FDM_SIZE` | 4096 | CSD 设备内存 FDM（MiB） |
| `CSD_NR_CU` | 8 | CSD 计算单元数（≤64） |
| `CSD_CSF_RUNTIME_SCALE` | 1 | 程序运行时间缩放 |
| `CSD_PROGRAM_DIR` | `/workspace/code/csd-programs` | 容器内算子目录（`csd_program_dir`） |

最大内存需求 ≈ `CSD_NVM_SIZE`（MB）+ `FEMU_MEM`（GB） + `CSD_FDM_SIZE`（MB）+ 约 1 GiB。

## 6. 容易踩的坑

1. **删除 `images/` 前需要先 `docker compose down`**：`./images` 是 bind mount，运行中删除会让
   provisioning 半途失败（`qemu-img: Could not open ...`），容器随后退出、8080 无人监听。
2. **改了 `make-guest-image.sh` 必须重建 guest 镜像**：`./scripts/build.sh -i`（= `docker compose build`
   后删掉 `images/u20s.qcow2` 再 `up -d`）；只改 `run-csd.sh` 的环境变量/QEMU 参数用默认的
   `./scripts/build.sh` 即可，guest 镜像会复用。
