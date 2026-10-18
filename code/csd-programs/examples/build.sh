#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
    make -C /workspace/code/csd-programs/examples "$@"
