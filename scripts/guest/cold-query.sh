#!/bin/sh
# 每条语句独立重启；默认冷缓存，PG_SINA_CACHE=warm 预热后测量。
set -e
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
case "$CACHE" in cold|warm) ;; *) echo 'PG_SINA_CACHE must be cold or warm' >&2; exit 2 ;; esac
export PATH=/home/femu/opt/postgres/release/bin:$PATH
# Make profiling visible when the library is loaded by session_preload_libraries.
export PGOPTIONS="${PGOPTIONS:-} -c pg_sina_csd.profile=$PROFILE"
PGDATA=/data/tpch_s1
pg_ctl -D "$PGDATA" -m fast -w stop >/dev/null
sync
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
pg_ctl -D "$PGDATA" -l "$PGDATA/log" -o '-c autovacuum=off' -w start >/dev/null
if [ "$CACHE" = warm ]; then
    # 先顺序读入文件页缓存，再填充 shared_buffers，避免逐页预热产生
    # 大量同步设备读。全部准备及缓存核验均在正式 SQL 计时之外。
    warm_options='-c session_preload_libraries=pg_sina_csd -c pg_sina_csd.profile=off -c pg_sina_csd.exe_path=host-only'
    paths=$(PGOPTIONS="$warm_options" psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 -c \
        "SELECT pg_relation_filepath(oid) FROM pg_class WHERE relnamespace='public'::regnamespace AND relkind IN ('r','i') ORDER BY oid")
    files=$(printf '%s\n' "$paths" | while IFS= read -r relation; do
        for file in "$PGDATA/$relation" "$PGDATA/$relation".* "$PGDATA/${relation}_fsm"* "$PGDATA/${relation}_vm"*; do
            [ -f "$file" ] || continue
            printf '%s\n' "$file"
        done
    done)
    printf '%s\n' "$files" | xargs -d '\n' cat > /dev/null
    PGOPTIONS="$warm_options" psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 -c \
        "SELECT sum(pg_prewarm(c.oid,'buffer',f.fork)) FROM pg_class c
         CROSS JOIN (VALUES ('main'),('fsm'),('vm')) f(fork)
         WHERE c.relnamespace='public'::regnamespace AND c.relkind IN ('r','i')
           AND pg_relation_size(c.oid,f.fork)>0" > /dev/null
    buffers=$(PGOPTIONS="$warm_options" psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 -c \
        "WITH rels AS (SELECT oid,relfilenode FROM pg_class WHERE relnamespace='public'::regnamespace AND relkind IN ('r','i'))
         SELECT (SELECT count(*) FROM pg_buffercache b JOIN rels r USING(relfilenode)
                 WHERE b.reldatabase=(SELECT oid FROM pg_database WHERE datname=current_database()) AND b.relforknumber IN (0,1,2)),
                (SELECT sum(pg_relation_size(r.oid,f.fork)/8192) FROM rels r CROSS JOIN (VALUES ('main'),('fsm'),('vm')) f(fork))")
    cached=${buffers%|*}; expected=${buffers#*|}
    file_cache=$(printf '%s\n' "$files" | xargs -d '\n' fincore -b -n -o RES,SIZE | \
        awk '{cached+=$1; bytes+=$2} END {printf "%.0f|%.0f",cached,bytes}')
    file_cached=${file_cache%|*}; file_bytes=${file_cache#*|}
    echo "WARM exe_path=$MODE: shared_cached_pages=$cached shared_expected_pages=$expected file_cached_bytes=$file_cached file_bytes=$file_bytes" >&2
    [ "$cached" = "$expected" ] || { echo 'Warm buffers are incomplete' >&2; exit 1; }
    [ "$file_cached" = "$file_bytes" ] || { echo 'Warm file cache is incomplete' >&2; exit 1; }
else
    echo "COLD exe_path=$MODE: PG shared buffers reset; Guest page cache dropped" >&2
fi
psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
    -c "SET pg_sina_csd.profile=$PROFILE; LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path='$MODE'; SET statement_timeout='120s'" -f "$QUERY"
