#!/bin/bash
# 在 Guest 运行 bash /home/femu/scripts/install-postgre.sh [--rebuild]。
# 安装或复用 PostgreSQL，并为实验 Guest 配置 NVMe 直连权限。
set -eu

src_dir=/home/femu/apps/postgres
build_dir=/home/femu/opt/postgres/build
install_dir=/home/femu/opt/postgres/release

rebuild=0
case "${1:-}" in
    --rebuild) rebuild=1; shift ;;
esac
if [ "$#" -ne 0 ]; then
    echo "usage: $0 [--rebuild]" >&2
    exit 2
fi

fresh=0
if [ ! -x "${install_dir}/bin/postgres" ]; then
    fresh=1
fi
if [ "$fresh" -eq 1 ] || [ "$rebuild" -eq 1 ]; then
    mkdir -p "${build_dir}" && cd "${build_dir}"
    if [ ! -f config.status ]; then
        "${src_dir}/configure" --prefix="${install_dir}"
    fi
    make -j"$(nproc)"
    make install
fi

if [ "$fresh" -eq 1 ]; then
    make -C "${build_dir}/contrib/pg_visibility" -j"$(nproc)"
    make -C "${build_dir}/contrib/pg_visibility" install
    echo PATH="${install_dir}/bin:\$PATH" >> /home/femu/.profile
fi

# Upstream pgfincore 1.4.0 (bf7e1bb702b2ec62e9ac5ca94500307ca6a2e3d4)
# uses PGXS. Keep generated objects in the existing out-of-tree build.
if [ "$rebuild" -eq 1 ] || [ ! -f "${install_dir}/lib/pgfincore.so" ] ||
    [ ! -f "${install_dir}/share/extension/pgfincore.control" ]; then
    pgfincore_build="${build_dir}/contrib/pgfincore"
    mkdir -p "$pgfincore_build"
    pgfincore_make=(-C "$pgfincore_build" -f "${src_dir}/contrib/pgfincore/Makefile"
                   "PG_CONFIG=${install_dir}/bin/pg_config")
    make "${pgfincore_make[@]}" -j"$(nproc)"
    make "${pgfincore_make[@]}" install
fi

# 专用实验 Guest：PG 直接提交 vendor NVMe 命令，不使用提权辅助进程。
# 文件权限允许打开设备；CAP_SYS_ADMIN 满足内核 passthrough 检查。
if ! command -v setcap >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y libcap2-bin
fi
sudo tee /etc/udev/rules.d/99-pg-csd.rules >/dev/null <<'RULES'
SUBSYSTEM=="nvme", KERNEL=="nvme0", OWNER="femu", MODE="0660"
SUBSYSTEM=="block", KERNEL=="nvme0n1", OWNER="femu", MODE="0660"
SUBSYSTEM=="block", KERNEL=="nvme0n1p*", ENV{PARTNAME}=="femu-spill", OWNER="femu", MODE="0660"
RULES
sudo udevadm control --reload-rules
sudo chown femu /dev/nvme0 /dev/nvme0n1
sudo chmod 0660 /dev/nvme0 /dev/nvme0n1
# SNIA 根据 GPT 标签查找并独占 spill 分区，不假定 p1/p2 的角色。
for partition in /sys/class/block/nvme0n1p*; do
    if [ -f "$partition/uevent" ] && grep -qx 'PARTNAME=femu-spill' "$partition/uevent"; then
        sudo chown femu "/dev/${partition##*/}"
        sudo chmod 0660 "/dev/${partition##*/}"
    fi
done
sudo setcap cap_sys_admin=ep "${install_dir}/bin/postgres"
echo "PostgreSQL installed; direct CSD access configured (restart PG to apply)."
