#!/bin/sh
# 独立缓存组：cold每次清空；warm先HOST预热一次；repeat从冷态连续执行。
set -eu
MODE=${1:?usage: cold-query.sh execution-mode query.sql}
QUERY=${2:?usage: cold-query.sh execution-mode query.sql}
case "$MODE" in
    host) MODE=host-only ;;
    csd) MODE=csd-without-hpcp ;;
    host-only|filter-only|csd-without-hpcp|csd-with-hpcp|SQLerate) ;;
    *) echo 'unknown execution mode' >&2; exit 2 ;;
esac
PROFILE=${PG_SINA_PROFILE:-off}
case "$PROFILE" in on|off) ;; *) echo 'PG_SINA_PROFILE must be on or off' >&2; exit 2 ;; esac
CACHE=${PG_SINA_CACHE:-cold}
case "$CACHE" in cold|warm|repeat) ;; *) echo 'PG_SINA_CACHE must be cold, warm or repeat' >&2; exit 2 ;; esac
RUNS=${PG_SINA_RUNS:-1}
if [ "$CACHE" = repeat ]; then RUNS=${PG_SINA_RUNS:-5}; fi
case "$RUNS" in ''|*[!0-9]*|0) echo 'PG_SINA_RUNS must be positive' >&2; exit 2 ;; esac
[ "$CACHE" != cold ] || [ "$RUNS" -eq 1 ] || { echo 'Each cold run needs its own reset' >&2; exit 2; }
marked=no
if [ -n "${PG_SINA_RUNS+x}" ] || [ "$RUNS" -gt 1 ]; then marked=yes; fi
export PATH=/home/femu/opt/postgres/release/bin:$PATH
export PGOPTIONS="${PGOPTIONS:-} -c pg_sina_csd.profile=$PROFILE"
PGDATA=${PGDATA:-/data/tpch_s1}
PGDATABASE=${PGDATABASE:-tpch_s1}
pg_ctl -D "$PGDATA" -m fast -w stop >/dev/null
sync
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
pg_ctl -D "$PGDATA" -l "$PGDATA/log" -o '-c autovacuum=off' -w start >/dev/null
# Only cache-enabled modes populate either destination. DCP is an independent
# GUC, so setting hpcp alone does not turn it off for the control modes.
population=off
case "$MODE" in csd-with-hpcp|SQLerate) population=on ;; esac
driver=$(mktemp /tmp/tpch-run.XXXXXX)
trap 'rm -f "$driver"' EXIT HUP INT TERM
{
    printf "SET pg_sina_csd.profile=%s; LOAD 'pg_sina_csd'; SET statement_timeout='300s';\n" "$PROFILE"
    if [ "$CACHE" = warm ]; then
        printf "SET pg_sina_csd.exe_path='host-only'; SET pg_sina_csd.hpcp=off; SET pg_sina_csd.dcp=off; SET pg_sina_csd.profile=off;\n"
        printf '\\o /dev/null\n\\i %s\n\\o\n\\timing off\n' "$QUERY"
    fi
    printf "SET pg_sina_csd.exe_path='%s'; SET pg_sina_csd.hpcp=%s; SET pg_sina_csd.dcp=%s; SET pg_sina_csd.profile=%s;\n" "$MODE" "$population" "$population" "$PROFILE"
    run=1
    while [ "$run" -le "$RUNS" ]; do
        [ "$marked" = no ] || printf '\\echo TPCH_BEGIN %s\n' "$run"
        printf '\\i %s\n' "$QUERY"
        [ "$marked" = no ] || printf '\\echo TPCH_END %s\n' "$run"
        run=$((run+1))
    done
} > "$driver"
echo "CACHE=$CACHE exe_path=$MODE runs=$RUNS: reset both caches; population=$population" >&2
psql -X -q -A -t -v ON_ERROR_STOP=1 -d "$PGDATABASE" -f "$driver"
