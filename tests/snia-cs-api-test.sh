#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case ${1:-unit} in
    unit)
        cmake -S "$ROOT/code/csd-programs" -B /tmp/csd-programs-check -DCMAKE_BUILD_TYPE=RelWithDebInfo
        cmake --build /tmp/csd-programs-check -j2
        ctest --test-dir /tmp/csd-programs-check --output-on-failure
        cmake -S "$ROOT/code/apps/snia-cs-api" -B /tmp/snia-cs-api-check \
            -DCSD_TEST_PROGRAM_DIR=/tmp/csd-programs-check -DCMAKE_BUILD_TYPE=RelWithDebInfo
        cmake --build /tmp/snia-cs-api-check -j2
        ctest --test-dir /tmp/snia-cs-api-check --output-on-failure
        ;;
    build)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd sh -c \
            'cmake -S /workspace/code/csd-programs -B /workspace/code/csd-programs/build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build /workspace/code/csd-programs/build -j2'
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'cmake -S /home/femu/apps/snia-cs-api -B /tmp/snia-cs-api-guest -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build /tmp/snia-cs-api-guest -j2'
        ;;
    smoke|example)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'sudo /tmp/snia-cs-api-guest/cs-batch-sort-limit /dev/nvme0n1 /data/cs-batch-input.bin'
        ;;
    *) echo '用法：snia-cs-api-test.sh [unit|build|smoke|example]；example 写入256MiB数据文件' >&2; exit 2 ;;
esac
