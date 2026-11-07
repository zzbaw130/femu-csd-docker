#!/bin/bash
# 调用方式为 在宿主机运行 SF=8 bash tests/test-guest.sh <guest_test_script> [args...]。
# 脚本取文件名并从 /home/femu/tests 加载，args 原样转发。SF 指定数据库规模，默认 8。
if [[ $# -eq 0 ]]; then
    echo "usage: $0 <guest_test_script> [args...]" >&2
    exit 1
fi

GUEST_TEST_PATH=/home/femu/tests/$(basename $1)
shift

printf -v guest_command '%q ' env "SF=${SF:-8}" bash -l "$GUEST_TEST_PATH" "$@"
docker compose exec -T femu-csd ./run-guest-ssh.sh "$guest_command"
