#!/bin/sh
# 调用方式为 在 Guest 运行 SF=8 sh database-cache-test.sh，无位置参数。
# SF 默认 8，PGDATA/PGDATABASE 可显式覆盖。需缓存驱动、CSD 映射和 pg_buffercache。
# 用 Q6 检查文件页缓存、共享缓冲区和取消恢复。
# DCP/HPCP switches and cached-page correctness; canonical Q6 only by default.
set -eu
PGDATA=${PGDATA:-/data/${PGDATABASE:-tpch_s${SF:-8}}}
export PGDATA PGDATABASE=${PGDATABASE:-${PGDATA##*/}}
export PATH=/home/femu/opt/postgres/release/bin:$PATH
OUT=$(mktemp -d /home/femu/results/database-cache.XXXXXX)
chmod 755 "$OUT"
QUERY=/home/femu/tests/queries/template/6.sql
COLD=/home/femu/scripts/cold-query.sh
printf 'policy,shared_pages,file_cached_bytes,dirty_pages\n' > "$OUT/cache.csv"
PGOPTIONS='-c pg_sina_csd.dcp=off -c pg_sina_csd.hpcp=off' sh "$COLD" host-only "$QUERY" > "$OUT/host.log" 2> "$OUT/host.err"
sed '/^Time:/d; /^Timing is /d' "$OUT/host.log" > "$OUT/reference.rows"
for policy in none hpcp dcp both; do
    hpcp=off; dcp=off
    case "$policy" in hpcp) hpcp=on;; dcp) dcp=on;; both) hpcp=on; dcp=on;; esac
    PGOPTIONS="-c pg_sina_csd.dcp=$dcp -c pg_sina_csd.hpcp=$hpcp" PG_SINA_PROFILE=on \
        sh "$COLD" csd-with-hpcp "$QUERY" > "$OUT/$policy.log" 2> "$OUT/$policy.err"
    sed '/^Time:/d; /^Timing is /d' "$OUT/$policy.log" > "$OUT/current.rows"
    cmp "$OUT/reference.rows" "$OUT/current.rows"
    grep -q 'SQLerate scan callbacks continuations=[1-9]' "$OUT/$policy.err"
    state=$(psql -X -q -A -t -v ON_ERROR_STOP=1 -d "$PGDATABASE" -c \
        "SELECT count(*),count(*) FILTER (WHERE isdirty) FROM pg_buffercache
         WHERE relfilenode=pg_relation_filenode('public.lineitem') AND relforknumber=0
           AND reldatabase=(SELECT oid FROM pg_database WHERE datname=current_database())")
    shared=${state%|*}; dirty=${state#*|}
    path=$(psql -X -q -A -t -v ON_ERROR_STOP=1 -d "$PGDATABASE" -c "SELECT pg_relation_filepath('public.lineitem')")
    file=$(fincore -b -n -o RES "$PGDATA/$path" | tr -d ' ')
    [ "$dirty" -eq 0 ]
    if [ "$dcp" = on ]; then
        [ "$shared" -gt 0 ]; grep -q 'invalid_pages=0' "$OUT/$policy.err"
        # Read the canonical SQL through native PG, without clearing DCP pages.
        psql -X -q -A -t -v ON_ERROR_STOP=1 -d "$PGDATABASE" \
            -c "SET pg_sina_csd.exe_path='host-only'" -f /home/femu/tests/tpch-parameters.psql -f "$QUERY" > "$OUT/$policy.host.log" 2> "$OUT/$policy.host.err"
        sed '/^Time:/d; /^Timing is /d' "$OUT/$policy.host.log" > "$OUT/current.rows"
        cmp "$OUT/reference.rows" "$OUT/current.rows"
    else [ "$shared" -eq 0 ]; fi
    if [ "$hpcp" = on ]; then [ "$file" -gt 0 ]; else [ "$file" -eq 0 ]; fi
    printf '%s,%s,%s,%s\n' "$policy" "$shared" "$file" "$dirty" >> "$OUT/cache.csv"
done
# Cancel and reuse the same backend: the receiver/session must be reclaimed.
timeout -k 5s 30s psql -X -q -A -t -d "$PGDATABASE" \
    -c "SET pg_sina_csd.exe_path='csd-without-hpcp'; SET pg_sina_csd.dcp=on; SET statement_timeout='50ms'" \
    -f /home/femu/tests/tpch-parameters.psql -f "$QUERY" -c "SET statement_timeout=0" -f "$QUERY" > "$OUT/recovery.log" 2> "$OUT/recovery.err"
grep -q 'canceling statement due to statement timeout' "$OUT/recovery.err"
[ "$(grep -c 'ERROR:' "$OUT/recovery.err")" -eq 1 ]
sed '/^Time:/d; /^Timing is /d' "$OUT/recovery.log" > "$OUT/current.rows"
cmp "$OUT/reference.rows" "$OUT/current.rows"
rm "$OUT/current.rows" "$OUT/reference.rows"
printf 'Database-cache regression passed: %s\n' "$OUT"
