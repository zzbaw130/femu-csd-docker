#!/bin/bash
set -eu

src_dir=/home/femu/apps/postgres
build_dir=/home/femu/opt/postgres/build
install_dir=/home/femu/opt/postgres/release

if [ ! -x "${install_dir}/bin/postgres" ]; then
    mkdir -p "${build_dir}" && cd "${build_dir}"
    "${src_dir}/configure" --prefix="${install_dir}"
    make -j"$(nproc)"
    make install

    # install contrib extension
    extensions=(pg_visibility)
    for extension in "${extensions[@]}"; do
        cd "${build_dir}/contrib/${extension}"
        make -j"$(nproc)"
        make install
    done
    echo PATH="${install_dir}/bin:\$PATH" >> /home/femu/.profile
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
    if grep -qx 'PARTNAME=femu-spill' "$partition/uevent"; then
        sudo chown femu "/dev/${partition##*/}"
        sudo chmod 0660 "/dev/${partition##*/}"
    fi
done
sudo setcap cap_sys_admin=ep "${install_dir}/bin/postgres"
echo "PostgreSQL installed; direct CSD access configured (restart PG to apply)."
