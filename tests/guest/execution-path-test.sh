#!/bin/sh
# Five-mode and placement regression; SQL bodies come only from queries/.
set -eu
OUT=$(mktemp -d /home/femu/results/execution-path.XXXXXX)
chmod 755 "$OUT"
COLD=/home/femu/scripts/cold-query.sh
PG=/home/femu/opt/postgres/release/bin/psql
QUERIES=/home/femu/tests/queries
run() {
    query=$1; mode=$2; label=$3
    sh "$COLD" "$mode" "$QUERIES/$query.sql" > "$OUT/$query.$label.log" 2> "$OUT/$query.$label.err"
    sed -e '/^Time:/d' -e '/^Timing is /d' "$OUT/$query.$label.log" > "$OUT/current.rows"
    if [ "$mode" = host-only ]; then cp "$OUT/current.rows" "$OUT/$query.host.rows";
    else cmp "$OUT/current.rows" "$OUT/$query.host.rows"; fi
    echo "Q$query $label: result matches"
}
plan() {
    query=$1; mode=$2; label=$3
    "$PG" -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
        -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path='$mode'" \
        -f "$QUERIES/$query.explain.sql" > "$OUT/$query.$label.plan" 2> "$OUT/$query.$label.plan.err"
}
for query in 6 13; do
    run "$query" host-only host
    for mode in filter-only csd-without-hpcp csd-with-hpcp SQLerate; do
        run "$query" "$mode" "$mode"
        plan "$query" "$mode" "$mode"
        grep -q "Execution Mode: $mode" "$OUT/$query.$mode.plan"
    done
done
# The scan-only baseline must retain actual native aggregate/join nodes.
grep -q 'Hash Right Join' "$OUT/13.filter-only.plan"
grep -q 'Custom Scan (SniaCsdPlan)' "$OUT/13.filter-only.plan"
grep -q 'HPCP: disabled' "$OUT/6.csd-without-hpcp.plan"
grep -q 'HPCP: enabled' "$OUT/6.csd-with-hpcp.plan"
# Structural tests change model inputs, never the canonical query or parameters.
# An expensive device must leave the SQL native; cheap delivery exposes islands
# underneath the native RIGHT JOIN (unsupported by the device compiler).
export PGOPTIONS='-c pg_sina_csd.csd_cpu_ratio=1000'
run 6 SQLerate expensive-device
plan 6 SQLerate expensive-device
if grep -q 'Custom Scan (SniaCsd' "$OUT/6.expensive-device.plan"; then exit 1; fi
export PGOPTIONS='-c max_parallel_workers_per_gather=0 -c pg_sina_csd.result_batch_ms=0 -c pg_sina_csd.csd_cpu_ratio=0.001 -c pg_sina_csd.cache_mbps=1'
run 13 SQLerate mixed
plan 13 SQLerate mixed
grep -q 'Hash Right Join' "$OUT/13.mixed.plan"
grep -q 'Custom Scan (SniaCsdPlan)' "$OUT/13.mixed.plan"
export PGOPTIONS='-c max_parallel_workers_per_gather=0 -c enable_hashjoin=off -c pg_sina_csd.result_batch_ms=0 -c pg_sina_csd.host_cost_ms=1 -c pg_sina_csd.csd_cpu_ratio=4 -c pg_sina_csd.cache_mbps=1'
run 13 SQLerate merge-streams
plan 13 SQLerate merge-streams
grep -Eq '^[[:space:]]*->[[:space:]]+Merge.*Join' "$OUT/13.merge-streams.plan"
[ "$(grep -c 'Custom Scan (SniaCsdPlan)' "$OUT/13.merge-streams.plan")" -ge 2 ]
unset PGOPTIONS
# Preserve the aggregate fast path and scalar/decorrelation semantics.
for query in 1 11 17; do
    run "$query" host-only host
    run "$query" SQLerate SQLerate
    plan "$query" SQLerate SQLerate
done
run 11 csd-without-hpcp scalar-initplan
rm -f "$OUT/current.rows" "$OUT/"*.host.rows
printf 'Execution-path regression passed: %s\n' "$OUT"
