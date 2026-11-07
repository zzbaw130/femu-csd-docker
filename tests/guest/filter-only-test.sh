#!/bin/sh
# 调用方式为 在 Guest 运行 SF=8 sh filter-only-test.sh [查询编号...]。
# SF 默认 8，PGDATA/PGDATABASE 可显式覆盖。查询编号默认 6 1 9，另用固定 Q17 检查取消。
# Focused canonical SQL checks for scan output, CU scheduling and PG types.
set -eu
PGDATA=${PGDATA:-/data/${PGDATABASE:-tpch_s${SF:-8}}}
export PGDATA PGDATABASE=${PGDATABASE:-${PGDATA##*/}}
export PATH=/home/femu/opt/postgres/release/bin:$PATH
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp -d /home/femu/results/filter-only.XXXXXX)
chmod 755 "$OUT"
COLD=/home/femu/scripts/cold-query.sh
export PGOPTIONS="${PGOPTIONS:-} -c session_preload_libraries=pg_sina_csd,auto_explain -c auto_explain.log_min_duration=0 -c auto_explain.log_level=notice -c auto_explain.log_analyze=off -c auto_explain.log_timing=off -c auto_explain.log_format=json"
printf 'query,mode,latency_ms,result_match\n' > "$OUT/timings.csv"
echo "Results: $OUT"
if [ "$#" -eq 0 ]; then set -- 6 1 9; fi
for query in "$@"; do
    for mode in host-only filter-only csd-without-hpcp; do
        sh "$COLD" "$mode" "/home/femu/tests/queries/template/$query.sql" > "$OUT/$query.$mode.log" 2> "$OUT/$query.$mode.err"
        sed -e '/^Time:/d' -e '/^Timing is /d' "$OUT/$query.$mode.log" > "$OUT/$query.$mode.rows"
        if [ "$mode" != host-only ]; then cmp "$OUT/$query.host-only.rows" "$OUT/$query.$mode.rows"; fi
        elapsed=$(awk '/^Time:/ {sum+=$2;n++} END {if(n) printf "%.3f",sum}' "$OUT/$query.$mode.log")
        printf '%s,%s,%s,yes\n' "$query" "$mode" "$elapsed" >> "$OUT/timings.csv"
        echo "Q$query $mode=$elapsed ms match=yes"
    done
    rm -f "$OUT/$query."*.rows
done
# ERROR can detach DSM before extension ResourceOwner cleanup. Cover both
# initialization and an executing stream, without adding another query copy.
for timeout in 100ms 1s; do
    log=$OUT/cancel-$timeout
    psql -X -q -A -t -v ON_ERROR_STOP=off -d "$PGDATABASE" \
        -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path='filter-only'; SET statement_timeout='$timeout'" \
        -f "$ROOT/tpch-parameters.psql" -f "$ROOT/queries/template/17.sql" \
        -c "SET statement_timeout=0; SELECT 'connection-alive'" > "$log.log" 2> "$log.err"
    grep -q 'canceling statement due to statement timeout' "$log.err"
    grep -qx 'connection-alive' "$log.log"
    echo "Cancellation $timeout: connection alive"
done
