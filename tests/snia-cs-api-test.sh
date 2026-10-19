#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case ${1:-unit} in
    unit)
        make -C "$ROOT/code/csd-programs" snia-cs-scale.so
        cmake -S "$ROOT/code/apps/snia-cs-api" -B /tmp/snia-cs-api-check -DCMAKE_BUILD_TYPE=Debug
        cmake --build /tmp/snia-cs-api-check -j2
        ctest --test-dir /tmp/snia-cs-api-check --output-on-failure
        ;;
    build)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            make -C /workspace/code/csd-programs snia-cs-scale.so
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'cmake -S /home/femu/apps/snia-cs-api -B /tmp/snia-cs-api-guest -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build /tmp/snia-cs-api-guest -j2'
        ;;
    smoke)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'sudo /tmp/snia-cs-api-guest/api-smoke /dev/nvme0n1 && sudo /tmp/snia-cs-api-guest/live-api /dev/nvme0n1'
        ;;
    *) echo '用法：snia-cs-api-test.sh [unit|build|smoke]；SSH_PORT 选择测试 Guest' >&2; exit 2 ;;
esac
