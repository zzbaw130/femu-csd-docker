#!/bin/bash
# 调用方式为 在 Guest 运行 bash /home/femu/scripts/start-postgre.sh [-f] <SF>。
# SF 选择 tpch_s<SF>.tar（如 0.01、4、8），-f 强制重新解包。需先初始化 NVMe 并安装 PG。
set -eu
export PATH=/home/femu/opt/postgres/release/bin:$PATH

FORCE_RELOAD=0
while getopts ":f" opt; do
    case ${opt} in
        f) FORCE_RELOAD=1 ;;
        *) echo "usage: $0 [-f] <SF>" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

SF=${1:?usage: $0 [-f] <SF>}

SRC_TAR=/home/femu/data/tpch_s${SF}.tar
DEST_DIR=/data
PGDATA=${DEST_DIR}/tpch_s${SF}

if ! command -v pg_ctl >/dev/null; then
    echo "PostgreSQL is missing. Run install-postgre.sh first." >&2
    exit 1
fi

# Stop the running instance before editing its configuration or restoring data.
running_data=$(env -u PGOPTIONS psql -XqAt -d postgres -c 'SHOW data_directory' 2>/dev/null || true)
if [ -n "$running_data" ]; then
    pg_ctl -D "$running_data" -m fast -w stop >/dev/null
elif pg_ctl -D "$PGDATA" status >/dev/null 2>&1; then
    pg_ctl -D "$PGDATA" -m fast -w stop >/dev/null
fi

if [ ! -e "$PGDATA" ] || [ "$FORCE_RELOAD" -eq 1 ]; then
    tar -xf "$SRC_TAR" -C "$DEST_DIR"
fi

# 写入 TPC-H 实例的默认参数，测试重启时直接读取 postgresql.conf。
conf=$PGDATA/postgresql.conf
while IFS='=' read -r name value; do
    if grep -Eq "^[#[:space:]]*$name[[:space:]]*=" "$conf"; then
        sed -i -E "s/^[#[:space:]]*$name[[:space:]]*=.*/$name = $value/" "$conf"
    else
        printf '%s = %s\n' "$name" "$value" >> "$conf"
    fi
done <<'SETTINGS'
shared_buffers='2GB'
work_mem='1GB'
max_parallel_workers_per_gather=8
max_parallel_workers=16
max_worker_processes=16
enable_hashagg=off
autovacuum=off
SETTINGS

pg_ctl -D "$PGDATA" -l "$PGDATA/log" -w start
