#!/bin/sh
# Guest: compare the selected Plan's device execution with cold native PG.
set -eu
export PATH=/home/femu/opt/postgres/release/bin:$PATH
PROJECT=/home/femu/apps/pg_sina_csd
COLD=/home/femu/scripts/cold-query.sh
RESULTS=$(mktemp -d /tmp/pg-sina-plans.XXXXXX)
echo "Results: $RESULTS"
tab=$(printf '\t')
while IFS="$tab" read -r name sql; do
    [ -n "$name" ] || continue
    printf '%s\n' "$sql" > "$RESULTS/current.sql"
    sh "$COLD" host "$RESULTS/current.sql" > "$RESULTS/$name.host"
    sh "$COLD" csd "$RESULTS/current.sql" > "$RESULTS/$name.csd"
    cmp "$RESULTS/$name.host" "$RESULTS/$name.csd"
    psql -X -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
        -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path=csd" \
        -c "EXPLAIN (COSTS OFF) $sql" > "$RESULTS/$name.plan"
    case "$name" in
        fallback_*) ! grep -q 'Custom Scan (SniaCsd' "$RESULTS/$name.plan" ;;
        *) grep -q 'Custom Scan (SniaCsd' "$RESULTS/$name.plan" ;;
    esac
    echo "PASS $name"
done < "$PROJECT/tests/plan_cases.tsv"
# Exercise close after a real device request has allocated query state. The
# next statement uses the same backend and must produce the normal result.
printf "\\\\set ON_ERROR_STOP off\nSET statement_timeout='1s';\n" > "$RESULTS/cancel.sql"
cat "$PROJECT/tests/q9.sql" >> "$RESULTS/cancel.sql"
printf "\n\\\\set ON_ERROR_STOP on\nSET statement_timeout=0;\n" >> "$RESULTS/cancel.sql"
awk -F "$tab" '$1=="left_join" {print $2}' "$PROJECT/tests/plan_cases.tsv" >> "$RESULTS/cancel.sql"
sh "$COLD" csd "$RESULTS/cancel.sql" > "$RESULTS/cancel.rows" 2> "$RESULTS/cancel.err"
grep -q 'canceling statement due to statement timeout' "$RESULTS/cancel.err"
cmp "$RESULTS/left_join.host" "$RESULTS/cancel.rows"
echo 'PASS cancellation cleanup and query reuse in the same backend'
