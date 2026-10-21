#!/bin/bash
# Guest: compare complete results against native PG, with cold input every time.
set -euo pipefail
export PATH=/home/femu/opt/postgres/release/bin:$PATH
PROJECT=/home/femu/apps/pg_sina_csd
RESULTS=$(mktemp -d /tmp/pg-sina-index.XXXXXX)
echo "Results: $RESULTS"
run_cold() {
    printf '%s\n' "$2" > "$RESULTS/current.sql"
    sh /home/femu/scripts/cold-query.sh "$1" "$RESULTS/current.sql" > "$3"
}
psql -X -q -v ON_ERROR_STOP=1 -d tpch_s1 -f "$PROJECT/tests/index_setup.sql" > "$RESULTS/setup.log"
while IFS=$'\t' read -r name sql; do
    [[ -n $name ]] || continue
    # Force the native index-join candidate on the small correctness fixtures.
    prefix='SET enable_hashjoin=off; SET enable_mergejoin=off; SET enable_bitmapscan=off;'
    run_cold host "$prefix $sql" "$RESULTS/$name.host"
    for memory in 4 64; do
        run_cold csd "$prefix SET pg_sina_csd.max_memory='${memory}MB'; $sql" "$RESULTS/$name.$memory"
        cmp "$RESULTS/$name.host" "$RESULTS/$name.$memory"
    done
    run_cold csd "$prefix EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF) $sql" "$RESULTS/$name.plan"
    grep -q 'CSD Index:' "$RESULTS/$name.plan"
    echo "PASS $name: full output, 4/64 MiB budget and native index candidate"
done < "$PROJECT/tests/index_cases.tsv"
# Empty indexes and an absent leading-key mapping must retain a correct path.
sql='SELECT count(*),sum(r.v) FROM snia_index_keys k JOIN snia_index_empty r ON k.k=r.k;'
run_cold host "$sql" "$RESULTS/empty.host"
run_cold csd "$sql" "$RESULTS/empty.csd"
cmp "$RESULTS/empty.host" "$RESULTS/empty.csd"
echo 'PASS empty indexed relation'
# REINDEX changes the physical file; reject its old mapping and use hash scan.
psql -X -q -v ON_ERROR_STOP=1 -d tpch_s1 -c 'REINDEX INDEX snia_index_rows_kv'
sql='SELECT count(*),sum(r.v),avg(r.v) FROM snia_index_keys k JOIN snia_index_rows r ON k.k=r.k;'
run_cold csd "SET enable_hashjoin=off; SET enable_mergejoin=off; EXPLAIN (COSTS OFF) $sql" "$RESULTS/stale.plan"
! grep -q 'CSD Index:' "$RESULTS/stale.plan"
run_cold csd "$sql" "$RESULTS/stale.csd"
cmp "$RESULTS/index_duplicates.host" "$RESULTS/stale.csd"
psql -X -q -v ON_ERROR_STOP=1 -d tpch_s1 -c "SELECT sina_csd.prepare('snia_index_rows')" > /dev/null
echo 'PASS stale index mapping falls back after REINDEX'
# An error after index page continuations must drain and free both join states.
{
    echo '\set ON_ERROR_STOP off'
    echo 'SET enable_hashjoin=off; SET enable_mergejoin=off; SET enable_bitmapscan=off;'
    sql='SELECT sum(r.v*1000000000000000000*1000000000000000000) FROM snia_index_keys k JOIN snia_index_rows r ON k.k=r.k;'
    echo "EXPLAIN (COSTS OFF) $sql"
    echo "$sql"
    echo '\set ON_ERROR_STOP on'
    echo 'SELECT count(*) FROM customer; SELECT count(*) FROM customer;'
} > "$RESULTS/error.sql"
sh /home/femu/scripts/cold-query.sh csd "$RESULTS/error.sql" > "$RESULTS/error.log" 2>&1
grep -q 'CSD Index:' "$RESULTS/error.log"
grep -q 'CSD execution failed' "$RESULTS/error.log"
[[ $(grep -c '^150000$' "$RESULTS/error.log") == 2 ]]
echo 'PASS index aggregation error and same-backend reuse'
# Q3 uses the unmodified SQL and native planner settings.
sql=$(cat "$PROJECT/tests/q3.sql")
run_cold host "$sql" "$RESULTS/q3.host"
for memory in 4 16 64 128; do
    run_cold csd "SET pg_sina_csd.max_memory='${memory}MB'; $sql" "$RESULTS/q3.$memory"
    cmp "$RESULTS/q3.host" "$RESULTS/q3.$memory"
    echo "PASS Q3 exact full output with ${memory} MiB query budget"
done
run_cold csd "SET pg_sina_csd.profile=on; EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF,FORMAT JSON) $sql" "$RESULTS/q3.json"
# Cancel while index continuations are active, then reuse the same backend.
{
    echo '\set ON_ERROR_STOP off'
    echo "SET statement_timeout='500ms'; $sql"
    echo 'SET statement_timeout=0;'
    echo '\set ON_ERROR_STOP on'
    echo 'SELECT count(*) FROM customer; SELECT count(*) FROM customer;'
} > "$RESULTS/cancel.sql"
sh /home/femu/scripts/cold-query.sh csd "$RESULTS/cancel.sql" > "$RESULTS/cancel.log" 2>&1
grep -q 'canceling statement due to statement timeout' "$RESULTS/cancel.log"
[[ $(grep -c '^150000$' "$RESULTS/cancel.log") == 2 ]]
echo 'PASS index continuation cancellation and same-backend reuse'
# Only fixture mappings/tables are removed; frozen TPC-H inputs stay prepared.
psql -X -q -v ON_ERROR_STOP=1 -d tpch_s1 <<'SQL'
DELETE FROM sina_csd.relations WHERE relid IN (SELECT indexid FROM sina_csd.indexes WHERE heaprelid IN (SELECT oid FROM pg_class WHERE relname LIKE 'snia_index_%'));
DELETE FROM sina_csd.relations WHERE relid IN (SELECT oid FROM pg_class WHERE relname LIKE 'snia_index_%');
DROP TABLE snia_index_keys,snia_index_rows,snia_index_empty,snia_index_big_keys,snia_index_big_rows,snia_index_small_keys,snia_index_small_rows;
SQL
