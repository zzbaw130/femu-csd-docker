#!/bin/sh
set -e

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
