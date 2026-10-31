#!/bin/sh
# Canonical Q6 only; 64MiB PG buffers force replacement, not device workmem.
set -eu
export PATH=/home/femu/opt/postgres/release/bin:$PATH
OUT=$(mktemp -d /home/femu/results/cache-residency.XXXXXX)
chmod 755 "$OUT"
PGDATA=/data/tpch_s1
QUERY=/home/femu/tests/queries/6.sql
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
        psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
            -c "CREATE FUNCTION pg_temp.cache_check(regclass) RETURNS text AS '$MODULE','pg_sina_cache_check' LANGUAGE C STRICT;
                SET pg_sina_csd.exe_path='$mode'; SET pg_sina_csd.hpcp=$hpcp; SET pg_sina_csd.dcp=$dcp" \
            -c "SELECT pg_temp.cache_check('lineitem')" -f "$QUERY" \
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
rm "$OUT/current.rows" "$OUT/reference.rows"
echo "Cache residency page checks passed: $OUT"
