#!/bin/bash
set -e

sudo mkdir -p /data

if ! sudo blkid /dev/nvme0n1 > /dev/null 2>&1; then
    sudo mkfs.ext4 /dev/nvme0n1
else
    echo "already mkfs"
fi

if ! mountpoint -q /data; then
    sudo mount -o noatime /dev/nvme0n1 /data
else
    echo "already mount"
fi

sudo chown femu:femu /data
