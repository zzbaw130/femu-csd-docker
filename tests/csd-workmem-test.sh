#!/usr/bin/env bash
# 调用方式为 在宿主机运行 bash tests/csd-workmem-test.sh [unit|build|live]。
# unit（默认）运行工作内存单测，build 构建 Guest 客户端，live 在真实 FEMU 上验证。
# live 需先 build，Guest 需有 NVMe、4 个 CU 和可用的工作内存。SSH_PORT 默认 8080。
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case ${1:-unit} in
    unit)
        cmake -S "$ROOT/code/csd-programs" -B /tmp/csd-programs-check -DCMAKE_BUILD_TYPE=RelWithDebInfo
        cmake --build /tmp/csd-programs-check -j2
        ctest --test-dir /tmp/csd-programs-check -R workmem --output-on-failure
        ;;
    build)
        bash "$ROOT/tests/snia-test.sh" build
        # Only apps/ is shared with the Guest; copy these two test files over SSH.
        tar -C "$ROOT/code/csd-programs/csd-workmem/tests" -cf - workmem_device_client.c workmem_probe.h | \
            docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'mkdir -p /tmp/workmem-device-test && tar -xf - -C /tmp/workmem-device-test && cc -O2 -Wall -Wextra -Werror -I/home/femu/apps/snia/include /tmp/workmem-device-test/workmem_device_client.c -L/tmp/snia-guest -lsnia -Wl,-rpath,/tmp/snia-guest -o /tmp/workmem-device-test/workmem-device-client'
        ;;
    live)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'sudo /tmp/workmem-device-test/workmem-device-client /dev/nvme0n1'
        ;;
    *) echo '用法：csd-workmem-test.sh [unit|build|live]；live 需4 CU' >&2; exit 2 ;;
esac
