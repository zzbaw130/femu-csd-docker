#!/bin/sh
set -e

# Guest 初始化：准备数据/spill 分区，并安装应用使用的 SNIA 库。
# 宿主物理 NUMA 绑定由 run-csd.sh 的 numactl 完成，覆盖 Guest RAM、
# FDM、workmem 和执行线程；NVM 后端单独放置，仍有跨节点访问。
# Guest 内设置策略不能约束宿主物理页，因此这里只检查单虚拟节点。
if [ "$(cat /sys/devices/system/node/online)" != 0 ]; then
    echo '实验要求 Guest 仅暴露一个 NUMA 节点；请检查 FEMU 启动配置。' >&2
    exit 1
fi
echo 'Guest NUMA=node0；物理节点由宿主 run-csd.sh 绑定，NVM 后端单独放置。'

DEVICE=/dev/nvme0n1
DATA=${DEVICE}p1
SPILL=${DEVICE}p2

# 首次运行或分区标签不匹配时重建布局，会清除原有数据。
if [ "$(sudo blkid -s PARTLABEL -o value "$DATA" 2>/dev/null || true)" != femu-data ] ||
   [ "$(sudo blkid -s PARTLABEL -o value "$SPILL" 2>/dev/null || true)" != femu-spill ]; then
    sudo parted -s -a optimal "$DEVICE" \
        mklabel gpt \
        mkpart femu-data ext4 1MiB 80% \
        mkpart femu-spill 80% 100%
    sudo partprobe "$DEVICE"
    sudo udevadm settle
    sudo mkfs.ext4 -F -L femu-data "$DATA"
    sudo wipefs -a "$SPILL"
fi

sudo mkdir -p /data
if ! mountpoint -q /data; then
    sudo mount -o noatime "$DATA" /data
fi
sudo chown femu:femu /data

echo "$DATA：femu-data，挂载到 /data"
echo "$SPILL：femu-spill，不格式化、不挂载"

cmake -S /home/femu/apps/snia -B /tmp/snia-guest \
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/snia-guest -j2
sudo cmake --install /tmp/snia-guest --prefix /usr/local
sudo ldconfig

echo 'SNIA 已安装：应用包含 cs.h，使用 cc application.c -lsnia -o application 编译。'
