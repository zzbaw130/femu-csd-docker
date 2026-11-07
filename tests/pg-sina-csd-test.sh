#!/usr/bin/env bash
# 调用方式为 在宿主机运行 bash tests/pg-sina-csd-test.sh [build|prepare|check|tpch|paths] [SF]。
# build（默认）构建并安装扩展，prepare 重建八表映射，check 校验映射读取。
# tpch 运行正式冷启动，paths 运行执行路径回归。SF 可作第二参数或环境变量，默认 8。
# Guest 须已启动所选规模的数据库；SSH_PORT 默认 8080。
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
sf=${2:-${SF:-8}}
if [[ $# -gt 2 || ! $sf =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo '用法：pg-sina-csd-test.sh [build|prepare|check|tpch|paths] [SF]' >&2
    exit 2
fi
guest() {
    docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
        env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh "export SF=$sf; $1"
}
case ${1:-build} in
    build)
        bash "$ROOT/tests/snia-test.sh" build
        guest 'cmake -S /home/femu/apps/pg_sina_csd -B /home/femu/opt/pg_sina_csd/build -DPG_CONFIG=/home/femu/opt/postgres/release/bin/pg_config -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build /home/femu/opt/pg_sina_csd/build -j2 && ctest --test-dir /home/femu/opt/pg_sina_csd/build --output-on-failure && cmake --install /home/femu/opt/pg_sina_csd/build'
        ;;
    prepare)
        guest '/home/femu/opt/postgres/release/bin/psql -X -v ON_ERROR_STOP=1 -d "tpch_s$SF" -c "CREATE EXTENSION IF NOT EXISTS pg_sina_csd" -c "SELECT sina_csd.prepare(c.oid) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname= '\''public'\'' AND c.relkind='\''r'\'' ORDER BY c.relname"'
        ;;
    check)
        guest 'set -e; for table in region nation supplier customer part partsupp orders lineitem; do sudo /home/femu/opt/pg_sina_csd/build/pg-sina-csd-check "dbname=tpch_s$SF user=femu" "public.$table"; done'
        ;;
    tpch)
        guest 'sh /home/femu/tests/tpch-cold.sh'
        ;;
    paths)
        guest 'sh /home/femu/tests/execution-path-test.sh'
        ;;
    *) echo '用法：pg-sina-csd-test.sh [build|prepare|check|tpch|paths] [SF]' >&2; exit 2 ;;
esac
