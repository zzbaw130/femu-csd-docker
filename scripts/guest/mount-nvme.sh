#!/bin/bash
set -euo pipefail

# 只在 Guest 内运行；具体分区、身份校验和配置生成由同目录工具完成。
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [[ $EUID -ne 0 ]]; then
    exec sudo -- python3 "$SCRIPT_DIR/nvme-layout.py" "$@"
fi

exec python3 "$SCRIPT_DIR/nvme-layout.py" "$@"
