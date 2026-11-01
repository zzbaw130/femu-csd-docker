#!/bin/sh
# 唯一正式TPC-H入口；cold/warm默认3次，repeat默认连续5次。
# auto_explain保留实际计划，不开启逐节点ANALYZE/TIMING。
set -eu
export PATH=/home/femu/opt/postgres/release/bin:$PATH
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
QUERIES=$ROOT/queries
PGDATABASE=${PGDATABASE:-tpch_s1}
COLD=/home/femu/scripts/cold-query.sh
MODES=${TPCH_MODES:-host-only filter-only csd-without-hpcp csd-with-hpcp SQLerate}
CACHES=${TPCH_CACHE_MODES:-${PG_SINA_CACHE:-cold}}
COLD_RUNS=${TPCH_COLD_RUNS:-3}
WARM_RUNS=${TPCH_WARM_RUNS:-3}
REPEAT_RUNS=${TPCH_REPEAT_RUNS:-5}
for count in "$COLD_RUNS" "$WARM_RUNS" "$REPEAT_RUNS"; do
    case "$count" in ''|*[!0-9]*|0) echo 'Run counts must be positive' >&2; exit 2 ;; esac
done
for cache in $CACHES; do
    case "$cache" in cold|warm|repeat) ;; *) echo "Invalid cache mode: $cache" >&2; exit 2 ;; esac
done
RESULTS=${TPCH_RESULTS:-$(mktemp -d /home/femu/results/tpch.XXXXXX)}
mkdir -p "$RESULTS"
chmod 755 "$RESULTS"
echo "Results: $RESULTS"
printf 'query,mode,latency_ms,result_match,execution_ok,cache,round,fdm_clean\n' > "$RESULTS/timings.csv"
printf 'cache,query,mode,runs,median_ms,total_ms,all_valid\n' > "$RESULTS/groups.csv"
export PG_SINA_PROFILE=${PG_SINA_PROFILE:-on}
export PGOPTIONS="${PGOPTIONS:-} -c session_preload_libraries=pg_sina_csd,auto_explain -c auto_explain.log_min_duration=0 -c auto_explain.log_level=notice -c auto_explain.log_analyze=off -c auto_explain.log_timing=off -c auto_explain.log_format=json"
printf '%s\n' "$PGOPTIONS" > "$RESULTS/pgoptions.txt"
printf '%s\n' "$CACHES" > "$RESULTS/cache-mode.txt"
printf 'cold=%s warm=%s repeat=%s profile=%s\n' "$COLD_RUNS" "$WARM_RUNS" "$REPEAT_RUNS" "$PG_SINA_PROFILE" > "$RESULTS/protocol.txt"
psql -X -q -A -t -v ON_ERROR_STOP=1 -d "$PGDATABASE" \
    -c "SELECT version()" -c "SHOW shared_buffers" -c "SHOW work_mem" \
    -c "SHOW max_parallel_workers_per_gather" > "$RESULTS/environment.log" 2> "$RESULTS/environment.err"
if [ "$#" -eq 0 ]; then set -- 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22; fi
for query in "$@"; do
    case "$query" in ''|*[!0-9]*) echo "Invalid query: $query" >&2; exit 2 ;; esac
    [ -f "$QUERIES/$query.sql" ] || { echo "No query: $query" >&2; exit 2; }
    sha256sum "$QUERIES/$query.sql"
done > "$RESULTS/queries.sha256"
failed=0
for cache in $CACHES; do
    case "$cache" in cold) runs=$COLD_RUNS ;; warm) runs=$WARM_RUNS ;; repeat) runs=$REPEAT_RUNS ;; esac
    for query in "$@"; do
        for mode in $MODES; do
            dir=$RESULTS/$cache/$query/$mode
            mkdir -p "$dir"
            mode_valid=yes
            groups=1; per_group=$runs
            if [ "$cache" = cold ]; then groups=$runs; per_group=1; fi
            group=1
            while [ "$group" -le "$groups" ]; do
                prefix=$dir/group$group
                sudo nvme admin-passthru /dev/nvme0 --opcode=0xc0 --cdw10=2 > "$prefix.fdm.before" 2>&1
                sudo cat /sys/kernel/debug/femu-cache/profile > "$prefix.cache.before"
                status=0
                PG_SINA_CACHE=$cache PG_SINA_RUNS=$per_group sh "$COLD" "$mode" "$QUERIES/$query.sql" > "$prefix.log" 2> "$prefix.err" || status=$?
                sudo cat /sys/kernel/debug/femu-cache/profile > "$prefix.cache.after"
                sudo nvme admin-passthru /dev/nvme0 --opcode=0xc0 --cdw10=2 > "$prefix.fdm.after" 2>&1
                if [ "$status" -ne 0 ]; then
                    failed=1; mode_valid=no
                    echo "FAIL $cache Q$query $mode group=$group status=$status; see $prefix.err" >&2
                fi
                clean=yes
                if ! grep -q 'result: 0x00000000' "$prefix.fdm.before" || ! grep -q 'result: 0x00000000' "$prefix.fdm.after"; then clean=no; failed=1; fi
                # Markers are psql output only. No SQL, cache sampling, waits or
                # settings are inserted between executions within a warm/repeat group.
                awk -v prefix="$prefix" '
                    /^TPCH_BEGIN / {run=$2; file=prefix "." run ".log"; printf "%s", "" > file; close(file); next}
                    /^TPCH_END / {file=prefix "." $2 ".done"; printf "%s", "" > file; close(file); run=""; next}
                    run!="" {print >> (prefix "." run ".log")}
                ' "$prefix.log"
                run=1
                while [ "$run" -le "$per_group" ]; do
                    round=$run; [ "$cache" != cold ] || round=$group
                    log=$prefix.$run.log
                    ok=no; match=no; elapsed=
                    if [ -f "$prefix.$run.done" ]; then
                        elapsed=$(awk '/^Time:/ {v+=$2; n++} END {if(n) printf "%.3f",v}' "$log")
                        [ -z "$elapsed" ] || ok=yes
                        sed '/^Time:/d; /^Timing is /d' "$log" > "$dir/$round.rows"
                        reference=$RESULTS/$query.reference.rows
                        if [ "$mode" = host-only ] && [ ! -f "$reference" ]; then cp "$dir/$round.rows" "$reference"; fi
                        if [ -f "$reference" ] && cmp -s "$reference" "$dir/$round.rows"; then match=yes; fi
                    fi
                    if [ "$ok" != yes ] || [ "$match" != yes ] || [ "$clean" != yes ]; then failed=1; mode_valid=no; fi
                    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$query" "$mode" "$elapsed" "$match" "$ok" "$cache" "$round" "$clean" >> "$RESULTS/timings.csv"
                    echo "$cache Q$query $mode run=$round ms=$elapsed match=$match ok=$ok fdm=$clean"
                    run=$((run+1))
                done
                group=$((group+1))
            done
            awk -F, -v cache="$cache" -v q="$query" -v mode="$mode" '$1==q && $2==mode && $6==cache && $5=="yes" {print $3}' "$RESULTS/timings.csv" | sort -n > "$dir/latencies.txt"
            awk -v cache="$cache" -v q="$query" -v mode="$mode" -v valid="$mode_valid" ' {a[++n]=$1; sum+=$1} END {if(n) printf "%s,%s,%s,%d,%.3f,%.3f,%s\n",cache,q,mode,n,n%2?a[(n+1)/2]:(a[n/2]+a[n/2+1])/2,sum,valid}' "$dir/latencies.txt" >> "$RESULTS/groups.csv"
        done
    done
done
echo "Results: $RESULTS"
echo 'Correctness, selected paths and performance targets must be assessed separately.'
exit "$failed"
