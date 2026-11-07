#!/bin/bash
# 调用方式为 在宿主机运行 SF=8 bash scripts/sh-guest.sh <guest_script> [args...]。
# guest_script 取文件名并从 /home/femu/scripts 加载，args 原样传给脚本。
# SF 是传入 Guest 的默认规模（默认 8）。已有位置参数的脚本仍使用其自身参数。
if [[ $# -eq 0 ]]; then
    echo "usage: $0 <guest_script> [args...]" >&2
    exit 1
fi

GUEST_SH_PATH=/home/femu/scripts/$(basename $1)
shift

printf -v guest_command '%q ' env "SF=${SF:-8}" bash -l "$GUEST_SH_PATH" "$@"
docker compose exec -T femu-csd ./run-guest-ssh.sh "$guest_command"
