#!/bin/bash
# 调用方式为 bash scripts/scp-guest.sh <host_path> <guest_path>。
# host_path 是宿主机文件或目录，guest_path 是 Guest 中的目标路径。需已启动 FEMU SSH。

KEY=$(dirname $(realpath $0))/../images/femu-guest-key
HOST_PATH=${1:?usage: $0 <host_path> <guest_path>}
GUEST_PATH=${2:?usage: $0 <host_path> <guest_path>}

# change ssh key owner
if [ ! -r ${KEY} ]; then
    sudo chown $USER:$USER ${KEY}
fi

scp -P 8080 -i ${KEY} -r ${HOST_PATH} femu@localhost:${GUEST_PATH}
