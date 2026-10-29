#!/bin/sh
# Targeted resource-model regression; all SQL comes from canonical queries.
set -eu
OUT=$(mktemp -d /home/femu/results/planner-cost.XXXXXX)
chmod 755 "$OUT"
COLD=/home/femu/scripts/cold-query.sh
QUERIES=/home/femu/tests/queries
if [ "$#" -eq 0 ]; then set -- 1 6 9 13; fi
for query in "$@"; do
    if [ "$query" = 13 ]; then export PGOPTIONS='-c pg_sina_csd.max_memory=65536'; fi
    for mode in host-only SQLerate; do
        sh "$COLD" "$mode" "$QUERIES/$query.sql" > "$OUT/$query.$mode.log" 2> "$OUT/$query.$mode.err"
        sed -e '/^Time:/d' -e '/^Timing is /d' "$OUT/$query.$mode.log" > "$OUT/$query.$mode.rows"
    done
    cmp "$OUT/$query.host-only.rows" "$OUT/$query.SQLerate.rows"
    # The legacy forced-device path need not invoke the cost planner.
    # SQLerate always exposes candidates, including rejected device regions.
    sh "$COLD" SQLerate "$QUERIES/$query.explain.sql" \
        > "$OUT/$query.plan" 2> "$OUT/$query.plan.err"
    unset PGOPTIONS
    grep -q 'Node Cost Estimates:.*serial=' "$OUT/$query.plan"
    if [ "$query" = 1 ]; then
        grep -q 'fused Scan/Filter/Aggregate' "$OUT/$query.plan"
        grep -q 'Estimated Spill I/O: 0 bytes' "$OUT/$query.plan"
    fi
    if [ "$query" = 9 ]; then grep -q 'rows=175 ' "$OUT/$query.plan"; fi
    if grep -q 'Estimated CSD Serial CPU:' "$OUT/$query.plan"; then
        grep -q 'Estimated CSD Scheduling:' "$OUT/$query.plan"
        grep -q 'CSD Stage Estimates:' "$OUT/$query.plan"
    fi
    if grep -Eiq '(cpu|state|resident|available|spill)=(-|nan|inf)' "$OUT/$query.plan"; then exit 1; fi
    echo "Q$query: results match; stage estimates present"
done
rm -f "$OUT/"*.rows
printf 'Planner-cost regression passed: %s\n' "$OUT"
