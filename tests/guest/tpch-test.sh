#!/bin/sh
# TPC-H 验收唯一入口。查询正文只来自相邻的 queries/。
set -eu
export PATH=/home/femu/opt/postgres/release/bin:$PATH
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
QUERIES=$ROOT/queries
COLD=/home/femu/scripts/cold-query.sh
RESULTS=$(mktemp -d /home/femu/results/tpch.XXXXXX)
chmod 755 "$RESULTS"
echo "Results: $RESULTS"
printf 'query,host_ms,csd_ms,result_match,csd_offloaded,performance\n' > "$RESULTS/timings.csv"
if [ "$#" -eq 0 ]; then set -- 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22; fi
failed=0
for query in "$@"; do
    case "$query" in ''|*[!0-9]*) echo "Invalid query: $query" >&2; exit 2 ;; esac
    [ -f "$QUERIES/$query.sql" ] || { echo "No query: $query" >&2; exit 2; }
    ok=1
    for mode in host csd; do
        if sh "$COLD" "$mode" "$QUERIES/$query.sql" > "$RESULTS/$query.$mode.log" 2> "$RESULTS/$query.$mode.err"; then
            sed -e '/^Time:/d' -e '/^Timing is /d' "$RESULTS/$query.$mode.log" > "$RESULTS/$query.$mode.rows"
        else
            ok=0; failed=1
            echo "FAIL Q$query $mode"
            cat "$RESULTS/$query.$mode.err" >&2
        fi
    done
    match=no; offloaded=no; performance=error
    host=$(awk '/^Time:/ {sum+=$2; n++} END {if(n) printf "%.3f",sum}' "$RESULTS/$query.host.log")
    csd=$(awk '/^Time:/ {sum+=$2; n++} END {if(n) printf "%.3f",sum}' "$RESULTS/$query.csd.log")
    if [ "$ok" -eq 1 ]; then
        if cmp -s "$RESULTS/$query.host.rows" "$RESULTS/$query.csd.rows"; then match=yes; else failed=1; fi
        if psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
            -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path=csd" \
            -f "$QUERIES/$query.explain.sql" > "$RESULTS/$query.plan.log" 2> "$RESULTS/$query.plan.err" &&
            grep -q 'Custom Scan (SniaCsd' "$RESULTS/$query.plan.log"; then offloaded=yes; else failed=1; fi
        performance=not_slower
        if awk -v h="$host" -v c="$csd" 'BEGIN {exit !(c>h)}'; then
            # Spill alone never proves physical memory exhaustion. A slower
            # sample remains unqualified until its cause has been established.
            performance=needs_analysis; failed=1
            sed -E 's/^([[:space:]]*)explain[[:space:]]+/\1EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) /I' "$QUERIES/$query.explain.sql" |
                PG_SINA_PROFILE=on sh "$COLD" csd - > "$RESULTS/$query.profile.log" 2> "$RESULTS/$query.profile.err" || failed=1
        fi
    fi
    printf '%s,%s,%s,%s,%s,%s\n' "$query" "$host" "$csd" "$match" "$offloaded" "$performance" >> "$RESULTS/timings.csv"
    echo "Q$query host=$host ms csd=$csd ms match=$match offloaded=$offloaded performance=$performance"
done
cat "$RESULTS/timings.csv"
echo "Slower CSD samples require memory/spill/CPU analysis; correctness alone is not acceptance."
exit "$failed"
