# Guest SSD 分区与挂载

`mount-nvme.sh` 只在 Guest 中运行，调用同目录的 `nvme-layout.py`，不依赖额外 Python 包。
仅接受型号为 `FEMU Computational Storage Controller` 的 NVMe namespace。

```bash
# 在宿主机调用；脚本由 sh-guest.sh 转交到 Guest。
./scripts/sh-guest.sh scripts/guest/mount-nvme.sh
```

空白盘自动创建 GPT，并将扣除 GPT 元数据后的可用范围按约 80%/20% 分割：

| 分区 | GPT PARTLABEL | GPT 类型 | 用途 |
| --- | --- | --- | --- |
| `/dev/nvme0n1p1` | `femu-data` | Linux filesystem | 创建 ext4，挂载 `/data`，归属 `femu:femu` |
| `/dev/nvme0n1p2` | `femu-spill` | Linux reserved | 不格式化、不挂载，供临时存储管理器使用 |

边界按 MiB 对齐，因此比例会有小于一个 MiB 的舍入。512B 和 4KiB 逻辑块均受支持。
重复执行会校验并复用正确的分区和 ext4，不重新格式化，也不会清空 spill 数据。
脚本不修改 `/etc/fstab`，Guest 重新启动后可再次运行。

## 旧整盘文件系统

旧脚本直接在 `/dev/nvme0n1` 上创建 ext4。新脚本遇到此布局或其他不匹配布局时默认退出，
不会迁移、缩小或覆盖已有 PostgreSQL 数据。若确实要重建，先备份数据、停止数据库、卸载该盘，
再显式执行：

```bash
# 会覆盖整个模拟盘的现有分区和数据文件系统。
./scripts/sh-guest.sh scripts/guest/mount-nvme.sh --repartition
```

即使传入 `--repartition`，脚本也拒绝操作仍被挂载、用作 swap 或被其他块设备使用的目标。
Guest 需安装 `python3`、`fdisk`、`util-linux`、`e2fsprogs` 和 `udev`。

## SINA 如何识别 spill 区

角色由 **GPT PARTLABEL + PARTUUID + 范围** 标识，不能只根据 p1/p2 序号猜测。
脚本成功挂载后，在 Guest 系统盘原子写入 `/etc/sina-cs-api/spill.json`，包括：

```json
{
  "version": 1,
  "namespace": "/dev/nvme0n1",
  "namespace_bytes": 17179869184,
  "lba_bytes": 512,
  "partition_table_uuid": "实际的 GPT UUID",
  "data": {
    "partition": "/dev/nvme0n1p1",
    "partlabel": "femu-data",
    "partuuid": "实际的 PARTUUID",
    "start_lba": 2048,
    "num_blocks": 26839040,
    "size_bytes": 13741588480,
    "mountpoint": "/data"
  },
  "spill": {
    "partition": "/dev/nvme0n1p2",
    "partlabel": "femu-spill",
    "partuuid": "实际的 PARTUUID",
    "start_lba": 26841088,
    "num_blocks": 6711296,
    "size_bytes": 3436183552
  }
}
```

以上数值仅示意，实际配置由当前设备容量计算并从写入后的 GPT 读取。
`start_lba` 是 **namespace 内的绝对 LBA**，`num_blocks` 以 `lba_bytes` 为单位。
不要与 Linux sysfs 中固定使用 512B 单位的 `start`/`size` 字段直接混用。

后续 SINA spill 管理器应读取 `spill` 项，重新校验设备容量、逻辑块大小、GPT/PARTUUID、
PARTLABEL 和分区边界，确认未格式化、未挂载后才允许分配，并将所有 I/O 限制在该范围。
设备重建后应重新运行挂载脚本生成配置，不能继续使用旧 token 或旧配置缓存。

**当前提交只准备布局和配置契约。现有 SINA 库尚未读取该文件，也尚未实现 STORE、临时
extent 分配或可恢复 CSF 调度；写出此配置不代表这些功能已经启用。**

可选参数：`--device /dev/nvme0n1`、`--mountpoint /data`、
`--config /etc/sina-cs-api/spill.json`。配置文件必须位于数据挂载点之外。

## 验证

在宿主机运行，不访问真实块设备：

```bash
python3 -m unittest discover -s tests -p 'test_nvme_layout.py' -v
bash -n scripts/guest/mount-nvme.sh
```
