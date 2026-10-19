#!/usr/bin/env python3
"""Guest 模拟 NVMe：80% ext4 数据区、20% 未格式化的 spill 区。"""

import argparse
import grp
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import sys
import tempfile
import time
import uuid


MIB = 1024 * 1024
DATA_LABEL = "femu-data"
SPILL_LABEL = "femu-spill"
DATA_TYPE = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
SPILL_TYPE = "8DA63339-0007-60C0-C436-083AC8230908"  # Linux reserved
FEMU_MODEL = "FEMU Computational Storage Controller"


class LayoutError(Exception):
    pass


def run(*args, input=None, check=True):
    result = subprocess.run(
        args, input=input, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if check and result.returncode:
        raise LayoutError(f"命令失败：{' '.join(args)}\n{result.stderr.strip()}")
    return result


def plan_layout(size_bytes, lba_bytes):
    """所有地址均为 namespace 逻辑块，不使用 sysfs 的固定 512B sector。"""
    if lba_bytes not in (512, 4096) or size_bytes % lba_bytes:
        raise LayoutError("仅支持容量按逻辑块对齐的 512B/4KiB namespace")
    alignment = MIB // lba_bytes
    total = size_bytes // lba_bytes
    # GPT 尾部保留分区表和备份 header，再向下按 MiB 对齐。
    table_blocks = (128 * 128 + lba_bytes - 1) // lba_bytes
    start = alignment
    end = (total - 1 - table_blocks) // alignment * alignment
    data_blocks = ((end - start) * 4 // 5) // alignment * alignment
    if data_blocks < alignment or end - start - data_blocks < alignment:
        raise LayoutError("设备太小，无法创建两个 MiB 对齐的分区")
    return {
        "data": {"start_lba": start, "num_blocks": data_blocks},
        "spill": {
            "start_lba": start + data_blocks,
            "num_blocks": end - start - data_blocks,
        },
    }


def partition_input(plan):
    lines = ["label: gpt", "unit: sectors", ""]
    for role, name, kind in (
        ("data", DATA_LABEL, DATA_TYPE), ("spill", SPILL_LABEL, SPILL_TYPE)
    ):
        extent = plan[role]
        lines.append(
            f'start={extent["start_lba"]}, size={extent["num_blocks"]}, '
            f'type={kind}, name="{name}"'
        )
    return "\n".join(lines) + "\n"


def validate_layout(table, device, lba_bytes, plan):
    if (table.get("label") != "gpt" or table.get("unit") != "sectors"
            # 较旧 sfdisk 不输出 sectorsize；逻辑块大小已由同设备 blockdev 获取。
            or table.get("sectorsize", lba_bytes) != lba_bytes
            or table.get("device") != device):
        raise LayoutError("不是目标 namespace 上的预期 GPT 布局")
    parts = table.get("partitions", [])
    if len(parts) != 2:
        raise LayoutError("需要且只能有 data/spill 两个分区")
    by_label = {part.get("name"): part for part in parts}
    roles = {}
    for role, label, kind in (
        ("data", DATA_LABEL, DATA_TYPE), ("spill", SPILL_LABEL, SPILL_TYPE)
    ):
        part = by_label.get(label)
        expected = plan[role]
        if (not part or part.get("type", "").upper() != kind
                or part.get("start") != expected["start_lba"]
                or part.get("size") != expected["num_blocks"]
                or not part.get("node", "").startswith(device + "p")):
            raise LayoutError(f"{label} 的标签、类型或 80/20 范围不匹配")
        try:
            uuid.UUID(part["uuid"])
        except (KeyError, ValueError, AttributeError) as error:
            raise LayoutError(f"{label} 缺少有效 PARTUUID") from error
        roles[role] = part
    if roles["data"]["uuid"].lower() == roles["spill"]["uuid"].lower():
        raise LayoutError("data/spill PARTUUID 不得相同")
    return roles


def filesystem_type(node):
    result = run("blkid", "-p", "-s", "TYPE", "-o", "value", node, check=False)
    if result.returncode not in (0, 2):
        raise LayoutError(f"无法探测 {node} 的文件系统：{result.stderr.strip()}")
    return result.stdout.strip()


def block_tree(device):
    result = run("lsblk", "--json", "--paths", "--output", "NAME,TYPE,MOUNTPOINTS", device)
    nodes = []

    def visit(node):
        nodes.append(node)
        for child in node.get("children", []):
            visit(child)

    for node in json.loads(result.stdout)["blockdevices"]:
        visit(node)
    return nodes


def require_unused(device):
    for node in block_tree(device):
        if any(node.get("mountpoints") or []):
            raise LayoutError(f'{node["name"]} 已挂载或用于 swap；请先停止应用并卸载')
        holders = Path("/sys/class/block") / Path(node["name"]).name / "holders"
        if holders.exists() and any(holders.iterdir()):
            raise LayoutError(f'{node["name"]} 正被其他块设备使用')


def wait_partition(part):
    node = part["node"]
    for _ in range(100):
        if Path(node).is_block_device():
            actual = run("blkid", "-p", "-s", "PART_ENTRY_UUID", "-o", "value", node,
                         check=False)
            if actual.returncode == 0 and actual.stdout.strip().lower() == part["uuid"].lower():
                return
        time.sleep(0.1)
    raise LayoutError(f"等待内核分区节点或 PARTUUID 更新超时：{node}")


def make_manifest(device, size_bytes, lba_bytes, table, roles, mountpoint):
    manifest = {
        "version": 1,
        "namespace": device,
        "namespace_bytes": size_bytes,
        "lba_bytes": lba_bytes,
        "partition_table_uuid": table["id"],
    }
    for role in ("data", "spill"):
        part = roles[role]
        manifest[role] = {
            "partition": part["node"],
            "partlabel": part["name"],
            "partuuid": part["uuid"],
            "start_lba": part["start"],
            "num_blocks": part["size"],
            "size_bytes": part["size"] * lba_bytes,
        }
    manifest["data"]["mountpoint"] = mountpoint
    return manifest


def write_manifest(path, manifest):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # 写入后原子替换，避免下次查询读到半份配置。
    fd, temporary = tempfile.mkstemp(prefix=".spill-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            os.fchmod(stream.fileno(), 0o644)
            json.dump(manifest, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def prepare(args):
    if os.geteuid() != 0:
        raise LayoutError("请通过 mount-nvme.sh 使用 sudo 运行")
    for command in ("sfdisk", "wipefs", "blockdev", "blkid", "lsblk", "udevadm",
                    "mkfs.ext4", "findmnt", "mount"):
        if not shutil.which(command):
            raise LayoutError(f"缺少 {command}；Ubuntu Guest 请安装 fdisk、util-linux、e2fsprogs、udev")
    device = os.path.realpath(args.device)
    if not Path(device).is_block_device() or not Path(device).name.startswith("nvme"):
        raise LayoutError("目标必须是 Guest 的 NVMe namespace 块设备")
    if (Path("/sys/class/block") / Path(device).name / "partition").exists():
        raise LayoutError("--device 必须指向整个 namespace，不能是分区")
    model = run("lsblk", "--nodeps", "--noheadings", "--output", "MODEL", device).stdout.strip()
    if model != FEMU_MODEL:
        raise LayoutError("只允许操作 FEMU Computational Storage Controller 模拟盘")
    # 在第一次写盘前校验 owner 和输出路径。
    owner = (pwd.getpwnam("femu").pw_uid, grp.getgrnam("femu").gr_gid)
    mountpoint = str(Path(args.mountpoint).resolve())
    config = Path(args.config).resolve()
    if config == Path(mountpoint) or Path(mountpoint) in config.parents:
        raise LayoutError("配置必须保存在 /data 之外的 Guest 系统盘中")
    size_bytes = int(run("blockdev", "--getsize64", device).stdout)
    lba_bytes = int(run("blockdev", "--getss", device).stdout)
    plan = plan_layout(size_bytes, lba_bytes)
    signatures = json.loads(run("wipefs", "--json", device).stdout)["signatures"]
    create = args.repartition or not signatures
    if create:
        require_unused(device)
        target = run("findmnt", "--noheadings", "--raw", "--mountpoint", mountpoint,
                     "--output", "MAJ:MIN", check=False)
        if target.returncode != 1:
            raise LayoutError(f"{mountpoint} 已被占用或无法检查；不会初始化设备")
        if signatures:
            print(f"重建 {device}：现有分区和文件系统将被覆盖。", flush=True)
        # sfdisk 同时清理旧整盘签名和新分区中残留的文件系统签名。
        run("sfdisk", "--wipe", "always", "--wipe-partitions", "always", device,
            input=partition_input(plan))
        run("blockdev", "--rereadpt", device)
        run("udevadm", "settle", "--timeout=10")

    result = run("sfdisk", "--json", device, check=False)
    try:
        if result.returncode:
            raise LayoutError("设备没有有效的 GPT 分区表")
        table = json.loads(result.stdout)["partitiontable"]
        roles = validate_layout(table, device, lba_bytes, plan)
    except LayoutError as error:
        raise LayoutError(
            f"{error}。已有数据不会自动迁移；备份并卸载后，显式使用 --repartition 重建。"
        ) from error

    for part in roles.values():
        wait_partition(part)
    data, spill = roles["data"]["node"], roles["spill"]["node"]
    # 已有布局也要拒绝被误挂载、格式化的 spill 区。
    require_unused(spill)
    if json.loads(run("wipefs", "--json", spill).stdout)["signatures"]:
        raise LayoutError("spill 分区存在存储签名；不会自动擦除")

    data_type = filesystem_type(data)
    if create:
        run("mkfs.ext4", "-F", "-L", DATA_LABEL, data)
    elif data_type != "ext4":
        raise LayoutError("已有 data 分区不是 ext4；不会自动格式化，请检查或显式重建")

    Path(mountpoint).mkdir(parents=True, exist_ok=True)
    mounted = run("findmnt", "--noheadings", "--raw", "--mountpoint", mountpoint,
                  "--output", "MAJ:MIN", check=False)
    if mounted.returncode == 0:
        devnum = os.stat(data).st_rdev
        if mounted.stdout.strip() != f"{os.major(devnum)}:{os.minor(devnum)}":
            raise LayoutError(f"{mountpoint} 已挂载其他设备，拒绝覆盖")
    elif mounted.returncode == 1:
        if any(any(node.get("mountpoints") or []) for node in block_tree(data)):
            raise LayoutError(f"{data} 已挂载到其他位置")
        run("mount", "-o", "noatime", data, mountpoint)
    else:
        raise LayoutError(f"无法检查挂载点 {mountpoint}")
    os.chown(mountpoint, *owner)
    manifest = make_manifest(device, size_bytes, lba_bytes, table, roles, mountpoint)
    write_manifest(config, manifest)
    print(f"数据区：{data} → {mountpoint}（{roles['data']['size'] * lba_bytes / MIB:.0f} MiB）")
    print(f"Spill 区：{spill}，不格式化、不挂载（{roles['spill']['size'] * lba_bytes / MIB:.0f} MiB）")
    print(f"SINA 分区配置：{config}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/nvme0n1", help="Guest namespace，默认 /dev/nvme0n1")
    parser.add_argument("--mountpoint", default="/data", help="数据分区挂载点，默认 /data")
    parser.add_argument("--config", default="/etc/sina-cs-api/spill.json", help="SINA 分区配置文件")
    parser.add_argument("--repartition", action="store_true",
                        help="清除现有布局，重新分区并格式化数据区；会丢失原有数据")
    args = parser.parse_args()
    try:
        prepare(args)
    except (LayoutError, OSError, KeyError, ValueError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
