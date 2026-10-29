#!/bin/sh
# Focused canonical SQL checks for scan output, CU scheduling and PG types.
set -eu
OUT=$(mktemp -d /home/femu/results/filter-only.XXXXXX)
chmod 755 "$OUT"
COLD=/home/femu/scripts/cold-query.sh
export PGOPTIONS="${PGOPTIONS:-} -c session_preload_libraries=auto_explain -c auto_explain.log_min_duration=0 -c auto_explain.log_level=notice -c auto_explain.log_analyze=off -c auto_explain.log_timing=off -c auto_explain.log_format=json"
printf 'query,mode,latency_ms,result_match\n' > "$OUT/timings.csv"
echo "Results: $OUT"
if [ "$#" -eq 0 ]; then set -- 6 1 9; fi
for query in "$@"; do
    for mode in host-only filter-only csd-without-hpcp; do
        sh "$COLD" "$mode" "/home/femu/tests/queries/$query.sql" > "$OUT/$query.$mode.log" 2> "$OUT/$query.$mode.err"
        sed -e '/^Time:/d' -e '/^Timing is /d' "$OUT/$query.$mode.log" > "$OUT/$query.$mode.rows"
        if [ "$mode" != host-only ]; then cmp "$OUT/$query.host-only.rows" "$OUT/$query.$mode.rows"; fi
        elapsed=$(awk '/^Time:/ {sum+=$2;n++} END {if(n) printf "%.3f",sum}' "$OUT/$query.$mode.log")
        printf '%s,%s,%s,yes\n' "$query" "$mode" "$elapsed" >> "$OUT/timings.csv"
        echo "Q$query $mode=$elapsed ms match=yes"
    done
    rm -f "$OUT/$query."*.rows
done
