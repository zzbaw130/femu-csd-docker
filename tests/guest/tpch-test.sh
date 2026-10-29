#!/bin/sh
# 正式 22 条查询，每次测量前独立冷启动；默认验证五种路径。
# auto_explain 只记录实际执行树，不开启逐节点 ANALYZE/TIMING 插桩。
set -eu
export PATH=/home/femu/opt/postgres/release/bin:$PATH
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
QUERIES=$ROOT/queries
COLD=/home/femu/scripts/cold-query.sh
MODES=${TPCH_MODES:-host-only filter-only csd-without-hpcp csd-with-hpcp SQLerate}
RESULTS=$(mktemp -d /home/femu/results/tpch.XXXXXX)
chmod 755 "$RESULTS"
echo "Results: $RESULTS"
printf 'query,mode,latency_ms,result_match,execution_ok\n' > "$RESULTS/timings.csv"
# Identical plan logging for every mode. JSON includes the actual SQLerate
# CustomScan boundaries, device plan and runtime workmem/spill counters.
export PGOPTIONS="${PGOPTIONS:-} -c session_preload_libraries=auto_explain -c auto_explain.log_min_duration=0 -c auto_explain.log_level=notice -c auto_explain.log_analyze=off -c auto_explain.log_timing=off -c auto_explain.log_format=json"
printf '%s\n' "$PGOPTIONS" > "$RESULTS/pgoptions.txt"
if [ "$#" -eq 0 ]; then set -- 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22; fi
failed=0
for query in "$@"; do
    case "$query" in ''|*[!0-9]*) echo "Invalid query: $query" >&2; exit 2 ;; esac
    [ -f "$QUERIES/$query.sql" ] || { echo "No query: $query" >&2; exit 2; }
    host_ok=0
    for mode in $MODES; do
        ok=no; match=no; elapsed=
        if sh "$COLD" "$mode" "$QUERIES/$query.sql" > "$RESULTS/$query.$mode.log" 2> "$RESULTS/$query.$mode.err"; then
            ok=yes
            # 冷启动后记录实际配置，避免读取到尚未重启应用的旧值。
            if [ ! -f "$RESULTS/environment.log" ]; then
                psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
                    -c "SELECT version()" -c "SHOW shared_buffers" -c "SHOW work_mem" \
                    -c "SHOW max_parallel_workers_per_gather" > "$RESULTS/environment.log" 2> "$RESULTS/environment.err"
            fi
            elapsed=$(awk '/^Time:/ {sum+=$2; n++} END {if(n) printf "%.3f",sum}' "$RESULTS/$query.$mode.log")
            sed -e '/^Time:/d' -e '/^Timing is /d' "$RESULTS/$query.$mode.log" > "$RESULTS/$query.$mode.rows"
            if [ "$mode" = host-only ]; then host_ok=1; match=yes;
            elif [ "$host_ok" -eq 1 ] && cmp -s "$RESULTS/$query.host-only.rows" "$RESULTS/$query.$mode.rows"; then match=yes;
            else failed=1; fi
        else
            failed=1
            echo "FAIL Q$query $mode; see $RESULTS/$query.$mode.err" >&2
        fi
        printf '%s,%s,%s,%s,%s\n' "$query" "$mode" "$elapsed" "$match" "$ok" >> "$RESULTS/timings.csv"
        echo "Q$query $mode=$elapsed ms match=$match ok=$ok"
    done
    rm -f "$RESULTS/$query."*.rows
done
echo "Results: $RESULTS"
echo 'Correctness and performance must be assessed separately; a completed run is not performance acceptance.'
exit "$failed"
