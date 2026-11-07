#!/bin/sh
# 调用：sh /home/femu/scripts/init-csd-env.sh [--partitioned]。
# 默认整盘用于 /data；--partitioned 保留历史80%数据、20%spill分区布局。
# 首次格式化会清除设备数据；随后编译安装 SNIA。
set -e

DEVICE=/dev/nvme0n1
DATA=$DEVICE

case ${1:-} in
    --partitioned)
        DATA=${DEVICE}p1
        SPILL=${DEVICE}p2
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
        echo "$SPILL：历史spill分区，不格式化、不挂载；设备spill使用隐藏空间"
        ;;
    '')
        if [ "$(sudo blkid -s TYPE -o value "$DATA" 2>/dev/null || true)" != ext4 ]; then
            sudo wipefs -a "$DEVICE"
            sudo partprobe "$DEVICE"
            sudo udevadm settle
            sudo mkfs.ext4 -F -m 0 -L femu-data "$DATA"
        fi
        ;;
    *) echo "用法：$0 [--partitioned]" >&2; exit 2 ;;
esac

sudo mkdir -p /data
if ! mountpoint -q /data; then
    sudo mount -o noatime "$DATA" /data
fi
sudo chown femu:femu /data

echo "$DATA：femu-data，挂载到 /data"

cmake -S /home/femu/apps/snia -B /tmp/snia-guest \
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/snia-guest -j2
sudo cmake --install /tmp/snia-guest --prefix /usr/local
sudo ldconfig

echo 'SNIA 已安装：应用包含 cs.h，使用 cc application.c -lsnia -o application 编译。'
