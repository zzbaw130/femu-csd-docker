#!/bin/sh
# 调用方式为 在 Guest 运行 SF=8 sh planner-path-test.sh，无位置参数。
# SF 默认 8，PGDATA/PGDATABASE 可显式覆盖。需已准备所选数据库的 CSD 映射。
# 仅规划指定模板，检查路径选择、设备计划编码和去相关。
# Planning only: canonical SQL plus a small MATERIALIZED CTE regression.
set -eu
PGDATA=${PGDATA:-/data/${PGDATABASE:-tpch_s${SF:-8}}}
export PGDATA PGDATABASE=${PGDATABASE:-${PGDATA##*/}}
PG=/home/femu/opt/postgres/release/bin/psql
TESTS=/home/femu/tests
QUERIES=$TESTS/queries/template
OUT=$(mktemp -d /home/femu/results/planner-paths.XXXXXX)
chmod 755 "$OUT"
plan() {
    query=$1; mode=$2; label=$3; settings=${4:-}
    case "$query" in
        *.original) sed -n '/^\/\* 原始查询$/,/^\*\/$/{ /^\/\* 原始查询$/d; /^\*\/$/d; p; }' "$QUERIES/${query%.original}.sql" ;;
        *) cat "$QUERIES/$query.sql" ;;
    esac | sed 's/^select$/explain select/' |
    "$PG" -X -q -A -t -v ON_ERROR_STOP=1 -d "$PGDATABASE" \
        -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path='$mode'; $settings" \
        -f "$TESTS/tpch-parameters.psql" -f - > "$OUT/$label.plan" 2> "$OUT/$label.err"
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
# Physical preparation is shared: grouping does not require a full ordered
# Heap walk through lineitem's non-covering index in either device mode.
for mode in csd-with-hpcp SQLerate; do
    plan 18 "$mode" "grouping.$mode" 'SET pg_sina_csd.csd_cpu_ratio=0.001'
    grep -q 'CSD Plan:.*Hash Aggregate.*Hash Aggregate' "$OUT/grouping.$mode.plan"
    if grep -q 'CSD Plan:.*index ordered' "$OUT/grouping.$mode.plan"; then exit 1; fi
done
# Selected index joins expose their probe and page estimates, rather than
# silently charging only generic tuple CPU. Other physical joins remain legal.
plan 21 SQLerate index-cost
if grep -q 'CSD Stage Estimates:.*IndexJoin' "$OUT/index-cost.plan"; then
    grep -Eq 'probes=[1-9][0-9]* index_pages=[1-9][0-9]* heap_pages=' "$OUT/index-cost.plan"
fi
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
"$PG" -X -q -A -t -v ON_ERROR_STOP=1 -d "$PGDATABASE" \
    -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path='SQLerate'" \
    -c 'EXPLAIN WITH x AS MATERIALIZED (SELECT p_partkey FROM part WHERE p_partkey <= 4) SELECT count(*) FROM x' \
    > "$OUT/cte.plan" 2> "$OUT/cte.err"
grep -q 'Custom Scan (SQLeratePlan)' "$OUT/cte.plan"
echo "Planner Path regression passed: $OUT"
