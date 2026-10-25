#!/bin/bash

ROOT=$(dirname $(realpath $0))
DOCKER_PROGRAMS_DIR=/workspace/code/csd-programs
FEMU_PASSTHRU_DIR=/home/femu/apps/csd-passthru

# user build option to force build
FORCE_BUILD=0
[[ $1 == "build" ]] && FORCE_BUILD=1 && shift

if [ ${FORCE_BUILD} -eq 1 ]; then
    # compile csd-programs in docker
    docker compose exec femu-csd \
        sh -c "cmake -S ${DOCKER_PROGRAMS_DIR} -B ${DOCKER_PROGRAMS_DIR}/build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build ${DOCKER_PROGRAMS_DIR}/build -j2"

    # compile csd-passthru in guest
    docker compose exec femu-csd ./run-guest-ssh.sh \
        make -C ${FEMU_PASSTHRU_DIR}

    exit 0
fi

# run passthru
docker compose exec femu-csd ./run-guest-ssh.sh \
    sudo ${FEMU_PASSTHRU_DIR}/csd-passthru $@