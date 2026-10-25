#!/bin/bash
# Usage: build.sh [-i|--rebuild-image] [-p|--build-postgre]
#
#   默认   重建容器镜像并启动 VM（保留已有 guest 镜像，不碰 postgres）
#   -i     同时删掉 guest 镜像 u20s.qcow2，重新做一遍 provisioning
#   -p     再做 postgres 那几步：初始化分区与 SNIA → 编译安装 → 载数据启库

set -eu

ROOT=$(dirname $(realpath $0))

REBUILD_IMAGE=0
BUILD_POSTGRE=0
while [[ $# -gt 0 ]]; do
    case $1 in
        -i|--rebuild-image) REBUILD_IMAGE=1 ;;
        -p|--build-postgre) BUILD_POSTGRE=1 ;;
        -ip|-pi) REBUILD_IMAGE=1;BUILD_POSTGRE=1 ;;
        *) echo "usage: $0 [-i|--rebuild-image] [-p|--build-postgre]" >&2; exit 1 ;;
    esac
    shift
done

docker compose down --rmi local

if [[ ${REBUILD_IMAGE} -eq 1 ]]; then
    sudo rm -f ${ROOT}/../images/u20s.qcow2
fi

docker compose build
docker compose up -d --wait

if [[ ${BUILD_POSTGRE} -eq 1 ]]; then
    ${ROOT}/sh-guest.sh ${ROOT}/guest/init-csd-env.sh
    ${ROOT}/sh-guest.sh ${ROOT}/guest/install-postgre.sh
    ${ROOT}/sh-guest.sh ${ROOT}/guest/start-postgre.sh 0.01
fi
