#!/bin/bash
# Guest: correctness and timings use the same cold-start path for every query.
set -euo pipefail
export PATH=/home/femu/opt/postgres/release/bin:$PATH
PROJECT=/home/femu/apps/pg_sina_csd
COLD=/home/femu/scripts/cold-query.sh
RESULTS=$(mktemp -d /tmp/pg-sina-queries.XXXXXX)
echo "Results: $RESULTS"
run_cold() {
    local mode=$1 sql=$2 log=$3
    printf '\\timing on\n%s\n' "$sql" > "$RESULTS/current.sql"
    sh "$COLD" "$mode" "$RESULTS/current.sql" > "$log"
    sed '/^Time:/d' "$log" > "$log.rows"
}
case ${1:-query} in
    query)
        psql -X -q -v ON_ERROR_STOP=1 -d tpch_s1 -f "$PROJECT/tests/query_setup.sql"
        while IFS=$'\t' read -r name sql; do
            [[ -n $name ]] || continue
            psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
                -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path=csd" \
                -c "EXPLAIN (COSTS OFF) $sql" > "$RESULTS/$name.plan"
            case $name in
                unsupported_*) ! grep -q 'SniaCsdAggregate' "$RESULTS/$name.plan" ;;
                zero_limit) ;; # PG may eliminate the whole query with LIMIT 0.
                *) grep -q 'SniaCsdAggregate' "$RESULTS/$name.plan" ;;
            esac
            run_cold host "$sql" "$RESULTS/$name.host"
            run_cold csd "$sql" "$RESULTS/$name.csd"
            cmp "$RESULTS/$name.host.rows" "$RESULTS/$name.csd.rows"
            echo "PASS $name: cold host/csd exact results and expected plan"
        done < "$PROJECT/tests/query_cases.tsv"
        for name in q1 q6; do
            sql=$(cat "$PROJECT/tests/$name.sql")
            psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
                -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path=csd" \
                -c "EXPLAIN (COSTS OFF) $sql" > "$RESULTS/$name.plan"
            grep -q 'SniaCsdAggregate' "$RESULTS/$name.plan"
            run_cold host "$sql" "$RESULTS/$name.host"
            run_cold csd "$sql" "$RESULTS/$name.csd"
            cmp "$RESULTS/$name.host.rows" "$RESULTS/$name.csd.rows"
            echo "PASS $name: all outputs equal after separate cold starts"
        done
        sql="$(cat "$PROJECT/tests/q1.sql" | sed 's/^;$/LIMIT 1;/')"
        run_cold host "$sql" "$RESULTS/q1-limit.host"
        run_cold csd "$sql" "$RESULTS/q1-limit.csd"
        cmp "$RESULTS/q1-limit.host.rows" "$RESULTS/q1-limit.csd.rows"
        echo 'PASS Q1 device sort and LIMIT 1'
        # GUC changes invalidate prepared plans; EXPLAIN does not execute them.
        psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
            -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path=csd; PREPARE path_test AS SELECT sum(l_quantity) FROM lineitem" \
            -c 'EXPLAIN (COSTS OFF) EXECUTE path_test' -c 'SET pg_sina_csd.exe_path=host' \
            -c 'EXPLAIN (COSTS OFF) EXECUTE path_test' -c 'SET pg_sina_csd.exe_path=csd' \
            -c 'EXPLAIN (COSTS OFF) EXECUTE path_test' > "$RESULTS/prepared.plan"
        [[ $(grep -c 'Custom Scan (SniaCsdAggregate)' "$RESULTS/prepared.plan") == 2 ]]
        grep -q 'Seq Scan' "$RESULTS/prepared.plan"
        echo 'PASS exe_path switches invalidate cached prepared plans'
        # Keep the same backend alive after cancellation: SNIA handles must
        # be reusable without relying on a worker or backend exit.
        {
            printf "\\set ON_ERROR_STOP off\nSET statement_timeout='100ms';\n"
            cat "$PROJECT/tests/q1.sql"
            printf '\\set ON_ERROR_STOP on\nSET statement_timeout=0;\n'
            printf 'EXPLAIN (COSTS OFF) SELECT count(*) FROM snia_query_fixture;\n'
            printf 'SELECT count(*) FROM snia_query_fixture;\nSELECT count(*) FROM snia_query_fixture;\n'
        } > "$RESULTS/timeout.sql"
        sh "$COLD" csd "$RESULTS/timeout.sql" > "$RESULTS/timeout.log" 2>&1
        grep -q 'canceling statement due to statement timeout' "$RESULTS/timeout.log"
        grep -q 'Custom Scan (SniaCsdAggregate)' "$RESULTS/timeout.log"
        [[ $(grep -c '^8$' "$RESULTS/timeout.log") == 2 ]]
        echo 'PASS cancellation and repeated device queries in the same PG backend'
        run_cold csd "$(cat "$PROJECT/tests/q1.sql")" "$RESULTS/after-timeout.csd"
        cmp "$RESULTS/q1.host.rows" "$RESULTS/after-timeout.csd.rows"
        echo 'PASS cancellation cleanup and full device reuse'
        psql -X -q -v ON_ERROR_STOP=1 -d tpch_s1 -c "DELETE FROM sina_csd.relations WHERE relid IN ('snia_query_fixture'::regclass,'snia_query_empty'::regclass); DROP TABLE snia_query_fixture,snia_query_empty"
        ;;
    bench)
        printf 'query\tmode\tround\tms\n' > "$RESULTS/timings.tsv"
        for round in 1 2 3; do
            if ((round%2)); then modes=(host csd); else modes=(csd host); fi
            for name in q1 q6; do
                for mode in "${modes[@]}"; do
                    log="$RESULTS/$name.$mode.$round"
                    run_cold "$mode" "$(cat "$PROJECT/tests/$name.sql")" "$log"
                    if [[ -f $RESULTS/$name.reference ]]; then cmp "$RESULTS/$name.reference" "$log.rows";
                    else cp "$log.rows" "$RESULTS/$name.reference"; fi
                    ms=$(awk '/^Time:/ {print $2}' "$log")
                    [[ -n $ms ]]
                    printf '%s\t%s\t%d\t%s\n' "$name" "$mode" "$round" "$ms" >> "$RESULTS/timings.tsv"
                    echo "$name $mode round=$round ${ms}ms"
                done
            done
        done
        awk 'NR>1 {key=$1" "$2; sum[key]+=$4; n[key]++} END {for(key in n) printf "%s mean=%.6fs\n",key,sum[key]/n[key]/1000}' "$RESULTS/timings.tsv"
        ;;
    *) echo 'usage: pg-sina-query-test.sh query|bench' >&2; exit 2 ;;
esac
