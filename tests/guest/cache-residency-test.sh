#!/bin/sh
# 调用方式为 在 Guest 运行 SF=8 sh cache-residency-test.sh，无位置参数。
# SF 默认 8，PGDATA/PGDATABASE 可显式覆盖。需缓存驱动、CSD 映射和 pg-sina-cache-test 模块。
# 验证 Q6 驻留位图和缓冲替换，会重启所选 PG 并清 Guest 文件缓存。
# Canonical Q6 only; 64MiB PG buffers force replacement, not device workmem.
set -eu
PGDATA=${PGDATA:-/data/${PGDATABASE:-tpch_s${SF:-8}}}
export PGDATA PGDATABASE=${PGDATABASE:-${PGDATA##*/}}
export PATH=/home/femu/opt/postgres/release/bin:$PATH
OUT=$(mktemp -d /home/femu/results/cache-residency.XXXXXX)
chmod 755 "$OUT"
QUERY=/home/femu/tests/queries/template/6.sql
MODULE=/home/femu/opt/pg_sina_csd/build/pg-sina-cache-test
export PGOPTIONS='-c session_preload_libraries=pg_sina_csd -c pg_sina_csd.profile=on'
restore() {
    pg_ctl -D "$PGDATA" -m fast -w stop >/dev/null
    pg_ctl -D "$PGDATA" -l "$PGDATA/log" -o '-c autovacuum=off' -w start >/dev/null
}
trap restore EXIT
for buffers in 2GB 64MB; do
    for policy in dcp hpcp; do
        pg_ctl -D "$PGDATA" -m fast -w stop >/dev/null
        sync; sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
        pg_ctl -D "$PGDATA" -l "$PGDATA/log" -o "-c autovacuum=off -c shared_buffers=$buffers" -w start >/dev/null
        mode=csd-without-hpcp; hpcp=off; dcp=on
        if [ "$policy" = hpcp ]; then mode=csd-with-hpcp; hpcp=on; dcp=off; fi
        psql -X -q -A -t -v ON_ERROR_STOP=1 -d "$PGDATABASE" \
            -c "CREATE FUNCTION pg_temp.cache_check(regclass) RETURNS text AS '$MODULE','pg_sina_cache_check' LANGUAGE C STRICT;
                SET pg_sina_csd.exe_path='$mode'; SET pg_sina_csd.hpcp=$hpcp; SET pg_sina_csd.dcp=$dcp" \
            -c "SELECT pg_temp.cache_check('lineitem')" -f /home/femu/tests/tpch-parameters.psql -f "$QUERY" \
            -c "SET pg_sina_csd.exe_path='host-only'" -f "$QUERY" \
            -c "SET pg_sina_csd.exe_path='SQLerate'" -f "$QUERY" \
            > "$OUT/$buffers.$policy.log" 2> "$OUT/$buffers.$policy.err"
        grep -E '^[-0-9.]+$' "$OUT/$buffers.$policy.log" > "$OUT/current.rows"
        [ "$(wc -l < "$OUT/current.rows")" -eq 3 ]
        [ "$(sort -u "$OUT/current.rows" | wc -l)" -eq 1 ]
        grep -Eq 'SQLerate relation cache .*generation=4' "$OUT/$buffers.$policy.err"
        if [ ! -f "$OUT/reference.rows" ]; then head -n 1 "$OUT/current.rows" > "$OUT/reference.rows"; fi
        [ "$(head -n 1 "$OUT/current.rows")" = "$(cat "$OUT/reference.rows")" ]
    done
done
grep -Eq 'dcp_evictions=[1-9][0-9]*' "$OUT/64MB.dcp.err"
# No correctness-observer module here: consecutive statements must not wait
# for the file worker. Also cover coalescing, abort and normal connection exit.
restore
burst=''
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do burst="$burst SELECT 1;"; done
timeout -k 5s 30s psql -X -q -A -t -d "$PGDATABASE" \
    -c "SET pg_sina_csd.exe_path='csd-with-hpcp'; SET pg_sina_csd.hpcp=on; SET pg_sina_csd.dcp=off" \
    -f /home/femu/tests/tpch-parameters.psql -f "$QUERY" -c "$burst" -c "SET pg_sina_csd.exe_path='SQLerate'" -f "$QUERY" \
    -c "SET statement_timeout='2ms'" -c "SELECT pg_sleep(0.1)" \
    -c "SET statement_timeout=0; SET pg_sina_csd.exe_path='csd-with-hpcp'" \
    -f "$QUERY" -c "SELECT pg_sleep(0.05)" > "$OUT/async.log" 2> "$OUT/async.err"
[ "$(grep -c 'ERROR:' "$OUT/async.err")" -eq 1 ]
grep -q 'canceling statement due to statement timeout' "$OUT/async.err"
[ "$(grep -Fxc "$(cat "$OUT/reference.rows")" "$OUT/async.log")" -eq 3 ]
grep -Eq 'coalesced=[1-9][0-9]*' "$OUT/async.err"
awk '
 /cache ledger refresh/ {
   for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="generation")g=a[2]; if(a[1]=="returned_ns")r=a[2]}
   returned[g]=r
 }
 /cache file publish/ {
   for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="generation")g=a[2]; if(a[1]=="finished_ns")f=a[2]}
   if(returned[g] && f>returned[g]) deferred++
 }
 END {if(!deferred)exit 1}
' "$OUT/async.err"
rm "$OUT/current.rows" "$OUT/reference.rows"
echo "Cache residency page checks passed: $OUT"
