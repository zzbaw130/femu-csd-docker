#!/bin/sh
# 每条被测语句独立冷启动；本脚本在 Guest 中运行。
set -e
MODE=${1:?usage: cold-query.sh host|csd query.sql}
QUERY=${2:?usage: cold-query.sh host|csd query.sql}
case "$MODE" in host|csd) ;; *) echo 'exe_path must be host or csd' >&2; exit 2 ;; esac
PROFILE=${PG_SINA_PROFILE:-off}
case "$PROFILE" in on|off) ;; *) echo 'PG_SINA_PROFILE must be on or off' >&2; exit 2 ;; esac
export PATH=/home/femu/opt/postgres/release/bin:$PATH
PGDATA=/data/tpch_s1
pg_ctl -D "$PGDATA" -m fast -w stop >/dev/null
sync
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
pg_ctl -D "$PGDATA" -l "$PGDATA/log" -o '-c autovacuum=off' -w start >/dev/null
echo "COLD exe_path=$MODE: PG shared buffers reset; Guest page cache dropped" >&2
psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
    -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path=$MODE; SET pg_sina_csd.profile=$PROFILE; SET statement_timeout='120s'" -f "$QUERY"
