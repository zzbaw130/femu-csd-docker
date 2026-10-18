#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case ${1:-unit} in
    unit)
        make -C "$ROOT/code/csd-programs" sina-cs-scale.so
        make -C "$ROOT/code/apps/sina-cs-api" check
        ;;
    build)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            make -C /workspace/code/csd-programs sina-cs-scale.so
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'make -C /home/femu/apps/sina-cs-api BUILD_DIR=/tmp/sina-cs-api-guest all examples live -j2'
        ;;
    smoke)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'sudo env SINA_CS_FEMU_DEVICES=/dev/nvme0n1:67108864:4 /tmp/sina-cs-api-guest/api-smoke /dev/nvme0n1 && sudo env SINA_CS_FEMU_DEVICES=/dev/nvme0n1:67108864:4 /tmp/sina-cs-api-guest/live-api /dev/nvme0n1'
        ;;
    *) echo '用法：sina-cs-api-test.sh [unit|build|smoke]；SSH_PORT 选择测试 Guest' >&2; exit 2 ;;
esac
