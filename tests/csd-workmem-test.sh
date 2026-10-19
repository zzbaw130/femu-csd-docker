#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case ${1:-unit} in
    unit)
        make -C "$ROOT/code/csd-programs/csd-workmem" check
        ;;
    build)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            make -C /workspace/code/csd-programs all
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'make -C /home/femu/apps/csd-workmem-test BUILD_DIR=/tmp/csd-workmem-test SDK_BUILD_DIR=/tmp/snia-cs-api-guest'
        ;;
    live)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'sudo /tmp/csd-workmem-test/csd-workmem-test /dev/nvme0n1'
        ;;
    *) echo '用法：csd-workmem-test.sh [unit|build|live]；live 需 4 CU 和已编译程序' >&2; exit 2 ;;
esac
