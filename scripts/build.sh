#!/bin/bash
# 在仓库根目录运行 ./scripts/build.sh [-i|-p|-ip]。
# 默认重建 FEMU 容器并启动 Guest，复用 Guest 系统盘。
# -i/--rebuild-image 重建 Guest 系统盘，-p/--build-postgre 初始化并启动数据库。
# SF 指定 -p 加载的规模（默认 0.01），例如 SF=8 ./scripts/build.sh -p。

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
    ${ROOT}/sh-guest.sh ${ROOT}/guest/start-postgre.sh "${SF:-0.01}"
fi
