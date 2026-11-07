#!/bin/bash
# 调用方式为 在宿主机运行 bash scripts/ssh-guest.sh，无位置参数。
# 通过当前 Compose 服务打开交互式 Guest SSH 会话。

docker compose exec femu-csd ./run-guest-ssh.sh
