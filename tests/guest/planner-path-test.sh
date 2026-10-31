#!/bin/sh
# Only planning structure: canonical SQL is never edited or executed here.
set -eu
PG=/home/femu/opt/postgres/release/bin/psql
QUERIES=/home/femu/tests/queries
OUT=$(mktemp -d /home/femu/results/planner-paths.XXXXXX)
chmod 755 "$OUT"
plan() {
    query=$1; mode=$2; label=$3; settings=${4:-}
    "$PG" -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
        -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path='$mode'; $settings" \
        -f "$QUERIES/$query.explain.sql" > "$OUT/$label.plan" 2> "$OUT/$label.err"
}
plan 14 SQLerate expensive 'SET pg_sina_csd.csd_cpu_ratio=1000'
if grep -q 'Custom Scan (SniaCsd' "$OUT/expensive.plan"; then exit 1; fi
plan 14 SQLerate cheap 'SET pg_sina_csd.csd_cpu_ratio=0.001'
grep -q 'Custom Scan (SniaCsd' "$OUT/cheap.plan"
grep -q 'original HOST=.*original CSD=.*shared search=' "$OUT/cheap.plan"
plan 9 SQLerate joins
grep -q 'CSD Stage Estimates:.*Join' "$OUT/joins.plan"
# A common rewrite may also produce a better HOST candidate. It must not
# silently replace the original native-PG baseline or require a CSD operator.
plan 17.original host-only original
grep -q 'SubPlan' "$OUT/original.plan"
plan 17.original SQLerate rewritten-host 'SET pg_sina_csd.csd_cpu_ratio=1000'
if grep -q 'Custom Scan (SniaCsd\|SubPlan' "$OUT/rewritten-host.plan"; then exit 1; fi
grep -q 'query=shared PG Path search' "$OUT/rewritten-host.plan"
for query in 17.original 20.original; do
    plan "$query" SQLerate "$query"
    grep -q 'Custom Scan (SniaCsd' "$OUT/$query.plan"
    grep -q 'original HOST=' "$OUT/$query.plan"
done
echo "Planner Path regression passed: $OUT"
