#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
guest() {
    docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
        env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh "$1"
}
case ${1:-build} in
    build)
        bash "$ROOT/tests/snia-test.sh" build
        guest 'cmake -S /home/femu/apps/pg_sina_csd -B /home/femu/opt/pg_sina_csd/build -DPG_CONFIG=/home/femu/opt/postgres/release/bin/pg_config -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build /home/femu/opt/pg_sina_csd/build -j2 && ctest --test-dir /home/femu/opt/pg_sina_csd/build --output-on-failure && cmake --install /home/femu/opt/pg_sina_csd/build'
        ;;
    prepare)
        guest '/home/femu/opt/postgres/release/bin/psql -X -v ON_ERROR_STOP=1 -d tpch_s1 -c "CREATE EXTENSION IF NOT EXISTS pg_sina_csd" -c "SELECT sina_csd.prepare(c.oid) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname= '\''public'\'' AND c.relkind='\''r'\'' ORDER BY c.relname"'
        ;;
    check)
        guest 'set -e; for table in region nation supplier customer part partsupp orders lineitem; do sudo /home/femu/opt/pg_sina_csd/build/pg-sina-csd-check "dbname=tpch_s1 user=femu" "public.$table"; done'
        ;;
    sql)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh '/home/femu/opt/postgres/release/bin/psql -X -v ON_ERROR_STOP=1 -d tpch_s1' \
            < "$ROOT/code/apps/pg_sina_csd/tests/prepare.sql"
        guest "sudo /home/femu/opt/pg_sina_csd/build/pg-sina-csd-check 'dbname=tpch_s1 user=femu' public.pg_sina_fixture 3 && sudo /home/femu/opt/pg_sina_csd/build/pg-sina-csd-check 'dbname=tpch_s1 user=femu' public.pg_sina_empty 3"
        guest "/home/femu/opt/postgres/release/bin/psql -X -v ON_ERROR_STOP=1 -d tpch_s1 -c \"DELETE FROM sina_csd.relations WHERE relid IN ('public.pg_sina_fixture'::regclass,'public.pg_sina_empty'::regclass); DROP TABLE public.pg_sina_fixture,public.pg_sina_empty\""
        ;;
    query|join|memory|profile|analyze|bench|order-bench|aggregate-bench|join-bench)
        guest "bash /home/femu/scripts/pg-sina-query-test.sh $1"
        ;;
    *) echo '用法：pg-sina-csd-test.sh [build|prepare|check|sql|query|join|memory|profile|analyze|bench|order-bench|aggregate-bench|join-bench]' >&2; exit 2 ;;
esac
