#!/bin/sh
# Planning only: canonical SQL plus a small MATERIALIZED CTE regression.
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
if grep -q 'SQLerate planner total_ms=' "$OUT/joins.err"; then exit 1; fi
plan 9 SQLerate timed 'SET pg_sina_csd.profile=on'
grep -q 'SQLerate planner total_ms=.*compile_ms=.*estimate_ms=' "$OUT/timed.err"
# Hundreds of Q9 candidates inspect properties without encoding a program.
# Only the final device island may receive its executable record.
awk '/SQLerate planner total_ms=/ {
    for (i=1; i<=NF; ++i) {
        split($i, field, "=")
        if (field[1]=="accepted") accepted=field[2]+0
        if (field[1]=="property_nodes") properties=field[2]+0
        if (field[1]=="encoded_nodes") encoded=field[2]+0
        if (field[1]=="serialized_plans") serialized=field[2]+0
    }
    ok=(properties>encoded && encoded>0 && encoded<accepted && serialized==1)
} END {exit !ok}' "$OUT/timed.err"
plan 14 SQLerate timed-host 'SET pg_sina_csd.profile=on; SET pg_sina_csd.csd_cpu_ratio=1000'
grep -q 'encoded_nodes=0 serialized_plans=0 fast_records=0' "$OUT/timed-host.err"
# The fused algorithm must be recognized before costing, while its execution
# record is generated only for the selected statement.
for query in 1 6; do
    plan "$query" SQLerate "fused.$query" 'SET pg_sina_csd.profile=on; SET pg_sina_csd.csd_cpu_ratio=0.001'
    grep -q 'CSD Stage Estimates: fused Scan/Filter/Aggregate' "$OUT/fused.$query.plan"
    grep -q 'serialized_plans=0 fast_records=1' "$OUT/fused.$query.err"
done
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
"$PG" -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
    -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path='SQLerate'" \
    -c 'EXPLAIN WITH x AS MATERIALIZED (SELECT p_partkey FROM part WHERE p_partkey <= 4) SELECT count(*) FROM x' \
    > "$OUT/cte.plan" 2> "$OUT/cte.err"
grep -q 'Custom Scan (SQLeratePlan)' "$OUT/cte.plan"
echo "Planner Path regression passed: $OUT"
