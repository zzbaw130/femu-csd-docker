#!/bin/sh
# Guest: each measured query starts with empty PG buffers and Guest page cache.
set -eu
export PATH=/home/femu/opt/postgres/release/bin:$PATH
PROJECT=/home/femu/apps/pg_sina_csd
COLD=/home/femu/scripts/cold-query.sh
RESULTS=$(mktemp -d /tmp/pg-sina-plan-bench.XXXXXX)
echo "Results: $RESULTS"
printf 'query,host_ms,csd_ms,reference_ms,host_timed_out\n' > "$RESULTS/timings.csv"
if [ "$#" -eq 0 ]; then set -- 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22; fi
for query in "$@"; do
    psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
        -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path=csd" \
        -c "EXPLAIN (COSTS OFF) $(cat "$PROJECT/tests/q$query.sql")" > "$RESULTS/q$query.plan"
    grep -q 'Custom Scan (SniaCsd' "$RESULTS/q$query.plan"
    printf "\\\\timing on\nSET statement_timeout='120s';\n" > "$RESULTS/current.sql"
    cat "$PROJECT/tests/q$query.sql" >> "$RESULTS/current.sql"
    reference=; timed_out=0
    for mode in host csd; do
        if ! sh "$COLD" "$mode" "$RESULTS/current.sql" > "$RESULTS/q$query.$mode" 2> "$RESULTS/q$query.$mode.err"; then
            cat "$RESULTS/q$query.$mode.err" >&2
            # A reference query is a correctness oracle only. Its latency
            # must never replace the original host query's timeout.
            if [ "$mode" != host ] || [ ! -f "$PROJECT/tests/reference/q$query.sql" ] ||
               ! grep -q 'canceling statement due to statement timeout' "$RESULTS/q$query.$mode.err"; then exit 1; fi
            timed_out=1
            printf "\\\\timing on\nSET statement_timeout='120s';\n" > "$RESULTS/reference.sql"
            cat "$PROJECT/tests/reference/q$query.sql" >> "$RESULTS/reference.sql"
            sh "$COLD" host "$RESULTS/reference.sql" > "$RESULTS/q$query.reference"
            sed '/^Time:/d' "$RESULTS/q$query.reference" > "$RESULTS/q$query.host.rows"
            reference=$(awk '/^Time:/ {time=$2} END {print time}' "$RESULTS/q$query.reference")
            continue
        fi
        sed '/^Time:/d' "$RESULTS/q$query.$mode" > "$RESULTS/q$query.$mode.rows"
    done
    cmp "$RESULTS/q$query.host.rows" "$RESULTS/q$query.csd.rows"
    host=$(awk '/^Time:/ {time=$2} END {print time}' "$RESULTS/q$query.host")
    if [ "$timed_out" -eq 1 ]; then host=; fi
    csd=$(awk '/^Time:/ {time=$2} END {print time}' "$RESULTS/q$query.csd")
    printf '%s,%s,%s,%s,%s\n' "$query" "$host" "$csd" "$reference" "$timed_out" >> "$RESULTS/timings.csv"
    if [ "$timed_out" -eq 1 ]; then
        echo "PASS Q$query csd=$csd ms; original host >120s; result matched PG reference ($reference ms)"
    else
        echo "PASS Q$query host=$host ms csd=$csd ms"
    fi
done
