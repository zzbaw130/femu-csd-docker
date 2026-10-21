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
                unsupported_*) ! grep -q 'Custom Scan (SniaCsd' "$RESULTS/$name.plan" ;;
                order_*) grep -q 'SniaCsdSort' "$RESULTS/$name.plan"
                         grep -q 'Sort.sink; Sort.source' "$RESULTS/$name.plan" ;;
                zero_limit) ;; # PG may eliminate the whole query with LIMIT 0.
                *) grep -q 'SniaCsdAggregate' "$RESULTS/$name.plan"
                   grep -q 'CSD Scheduling: independent serial batches' "$RESULTS/$name.plan" ;;
            esac
            if [[ $name == late_* ]]; then
                grep -Eq 'CSD Deferred Columns: [1-9]' "$RESULTS/$name.plan"
            fi
            run_cold host "$sql" "$RESULTS/$name.host"
            run_cold csd "$sql" "$RESULTS/$name.csd"
            cmp "$RESULTS/$name.host.rows" "$RESULTS/$name.csd.rows"
            echo "PASS $name: cold host/csd exact results and expected plan"
            if [[ $name == order_spill ]]; then
                printf 'SET pg_sina_csd.profile=on;\nEXPLAIN (ANALYZE,COSTS OFF,TIMING OFF,FORMAT JSON)\n%s\n' "$sql" > "$RESULTS/current.sql"
                sh "$COLD" csd "$RESULTS/current.sql" > "$RESULTS/$name.json"
                check=$(psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 -c "
                    WITH doc AS (SELECT pg_read_file('$RESULTS/$name.json')::jsonb->0->'Plan' AS p)
                    SELECT (p->>'CSD Spilled Runs')::bigint>0
                        AND (p->>'CSD Spill Write')::bigint>0
                        AND (p->>'CSD Spill Read')::bigint>0
                        AND (p->>'CSD Workmem Peak')::bigint<=67108864
                        AND (p->'CSD Profile'->'Sort.source'->>'Calls')::bigint>1
                    FROM doc")
                [[ $check == t ]]
                echo 'PASS external row sort actually spills, resumes and stays within 64 MiB workmem'
            fi
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
            printf "SET pg_sina_csd.profile=on;\n\\set ON_ERROR_STOP off\nSET statement_timeout='100ms';\n"
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
        # Scan fails in one slot while other batches may still be in flight.
        # Drain them before the same backend starts the following full scan.
        {
            printf '\\set ON_ERROR_STOP off\nSELECT sum(bad) FROM snia_query_parallel;\n'
            printf '\\set ON_ERROR_STOP on\nSELECT count(*) FROM snia_query_parallel;\nSELECT count(*) FROM snia_query_parallel;\n'
        } > "$RESULTS/error.sql"
        sh "$COLD" csd "$RESULTS/error.sql" > "$RESULTS/error.log" 2>&1
        grep -q 'CSD execution failed' "$RESULTS/error.log"
        [[ $(grep -c '^450000$' "$RESULTS/error.log") == 2 ]]
        echo 'PASS device error drains outstanding batches before same-backend reuse'
        # Consume one chunk, then close a spilled sort before its source EOF.
        psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
            -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path=csd; BEGIN" \
            -c 'DECLARE sort_cursor CURSOR FOR SELECT k,a,a*1.00,a*2.00,a*3.00,a*4.00,a*5.00,a*6.00 FROM snia_query_parallel ORDER BY a,k' \
            -c 'FETCH 1 FROM sort_cursor; CLOSE sort_cursor; COMMIT' \
            -c 'SELECT count(*) FROM snia_query_parallel' > "$RESULTS/sort-close.log"
        grep -q '^450000$' "$RESULTS/sort-close.log"
        echo 'PASS closing a partially consumed sort releases workmem and spill before same-backend reuse'
        psql -X -q -v ON_ERROR_STOP=1 -d tpch_s1 -c "DELETE FROM sina_csd.relations WHERE relid IN ('snia_query_fixture'::regclass,'snia_query_empty'::regclass,'snia_query_parallel'::regclass); DROP TABLE snia_query_fixture,snia_query_empty,snia_query_parallel"
        ;;
    memory)
        # Each measured execution has its own cold start, including host oracles.
        for name in q1 q6 order_spill; do
            sql=$(cat "$PROJECT/tests/$name.sql")
            run_cold host "$sql" "$RESULTS/$name.host"
            for memory in 4 16 64 128; do
                run_cold csd "SET pg_sina_csd.max_memory='${memory}MB'; $sql" "$RESULTS/$name.$memory"
                cmp "$RESULTS/$name.host.rows" "$RESULTS/$name.$memory.rows"
                echo "PASS $name with max_memory=${memory}MB: exact cold results"
            done
        done
        # A cached plan reads the setting at execution, not at PREPARE time.
        printf "PREPARE memory_test AS SELECT count(*) FROM lineitem;\nSET pg_sina_csd.max_memory='4MB';\nEXPLAIN (ANALYZE,COSTS OFF,TIMING OFF) EXECUTE memory_test;\nSET pg_sina_csd.max_memory='64MB';\nEXPLAIN (ANALYZE,COSTS OFF,TIMING OFF) EXECUTE memory_test;\n" > "$RESULTS/current.sql"
        sh "$COLD" csd "$RESULTS/current.sql" > "$RESULTS/prepared-memory.log"
        grep -q 'CSD Query Memory Limit: 4194304 bytes' "$RESULTS/prepared-memory.log"
        grep -q 'CSD Query Memory Limit: 67108864 bytes' "$RESULTS/prepared-memory.log"
        echo 'PASS prepared execution uses the current query memory limit'
        for memory in 4 16 64 128; do
            printf "SET pg_sina_csd.max_memory='%sMB';\nEXPLAIN (ANALYZE,COSTS OFF,TIMING OFF,FORMAT JSON)\n%s\n" "$memory" "$(cat "$PROJECT/tests/order_spill.sql")" > "$RESULTS/current.sql"
            sh "$COLD" csd "$RESULTS/current.sql" > "$RESULTS/spill.$memory.json"
        done
        ;;
    profile)
        [[ $(psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 -c "LOAD 'pg_sina_csd'" -c 'SHOW pg_sina_csd.profile') == off ]]
        for name in q1 q6; do
            sql=$(cat "$PROJECT/tests/$name.sql")
            run_cold host "$sql" "$RESULTS/$name.host"
            run_cold csd "SET pg_sina_csd.profile=on; $sql" "$RESULTS/$name.profile"
            cmp "$RESULTS/$name.host.rows" "$RESULTS/$name.profile.rows"
            printf 'SET pg_sina_csd.profile=on;\nEXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, FORMAT JSON)\n%s\n' "$sql" > "$RESULTS/current.sql"
            sh "$COLD" csd "$RESULTS/current.sql" > "$RESULTS/$name.json"
            # PG's own JSON parser validates EXPLAIN and numeric counters.
            check=$(psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 -c "
                WITH doc AS (SELECT pg_read_file('$RESULTS/$name.json')::jsonb->0->'Plan' AS plan),
                     data AS (SELECT plan,plan->'CSD Profile' AS p FROM doc)
                SELECT (p->'Scan'->>'Calls')::bigint=(plan->>'CSD Chunks')::bigint
                   AND (p->'Filter'->>'Calls')::bigint=(plan->>'CSD Chunks')::bigint
                   AND (p->'Aggregate.sink'->>'Calls')::bigint=(plan->>'CSD Chunks')::bigint
                   AND (p->'Aggregate.source'->>'Calls')::bigint=1
                   AND (p->'Scan'->>'CPU Sum')::numeric>0
                   AND (p->'Filter'->>'Wall Sum')::numeric>0
                   AND abs((p->>'Total Wall')::numeric-(p->>'Setup Wall')::numeric
                       -(p->>'Pipeline 1 Wall')::numeric-(p->>'Pipeline 2 Wall')::numeric
                       -(p->>'Cleanup Wall')::numeric)<0.01
                   AND CASE WHEN '$name'='q1' THEN (p->'Sort'->>'Calls')::bigint=1
                            ELSE (p->'Limit'->>'Calls')::bigint=1 END
                FROM data")
            [[ $check == t ]]
            echo "PASS $name: profiled cold result, structured EXPLAIN, per-operator calls and timings"
        done
        printf 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, FORMAT JSON)\n%s\n' "$(cat "$PROJECT/tests/q6.sql")" > "$RESULTS/current.sql"
        sh "$COLD" csd "$RESULTS/current.sql" > "$RESULTS/q6.off.json"
        check=$(psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 -c "
            WITH doc AS (SELECT pg_read_file('$RESULTS/q6.json')::jsonb->0->'Plan' AS enabled,
                                pg_read_file('$RESULTS/q6.off.json')::jsonb->0->'Plan' AS disabled)
            SELECT NOT (disabled ? 'CSD Profile')
               AND (enabled->>'CSD AFDM')::bigint-(disabled->>'CSD AFDM')::bigint
                   =4096*(enabled->>'CSD Workers')::bigint FROM doc")
        [[ $check == t ]]
        echo 'PASS default off: no profile output or counter AFDM allocation'
        ;;
    analyze)
        for round in 1 2 3; do
            if ((round%2)); then modes=(host csd profiled); else modes=(profiled csd host); fi
            for name in q1 q6; do
                for mode in "${modes[@]}"; do
                    path=csd; profile=off; transport=0
                    [[ $mode != host ]] || path=host
                    if [[ $mode == profiled ]]; then profile=on; transport=1; fi
                    log_start=$(( $(wc -l < /data/tpch_s1/log)+1 ))
                    {
                        printf 'SET track_io_timing=on;\nSET pg_sina_csd.profile=%s;\n' "$profile"
                        printf 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, FORMAT JSON)\n'
                        cat "$PROJECT/tests/$name.sql"
                    } > "$RESULTS/current.sql"
                    SNIA_FEMU_PROFILE=$transport sh "$COLD" "$path" "$RESULTS/current.sql" > "$RESULTS/$name.$mode.$round.json"
                    tail -n +"$log_start" /data/tpch_s1/log > "$RESULTS/$name.$mode.$round.log"
                    echo "DONE $name $mode round=$round"
                done
            done
        done
        # Environment profiling is only for these samples; restore normal PG.
        unset SNIA_FEMU_PROFILE
        pg_ctl -D /data/tpch_s1 -l /data/tpch_s1/log -o '-c autovacuum=off' -w restart >/dev/null
        ;;
    bench|order-bench)
        queries=(q1 q6)
        if [[ $1 == order-bench ]]; then queries=(order_topn order_spill); fi
        for name in "${queries[@]}"; do
            psql -X -q -A -t -v ON_ERROR_STOP=1 -d tpch_s1 \
                -c "LOAD 'pg_sina_csd'; SET pg_sina_csd.exe_path=csd" \
                -c "EXPLAIN (COSTS OFF) $(cat "$PROJECT/tests/$name.sql")" > "$RESULTS/$name.plan"
            grep -q 'Custom Scan (SniaCsd' "$RESULTS/$name.plan"
        done
        printf 'query\tmode\tround\tms\n' > "$RESULTS/timings.tsv"
        for round in 1 2 3; do
            if ((round%2)); then modes=(host csd); else modes=(csd host); fi
            for name in "${queries[@]}"; do
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
        if [[ $1 == order-bench ]]; then
            for name in "${queries[@]}"; do
                printf 'SET pg_sina_csd.profile=on;\nEXPLAIN (ANALYZE,COSTS OFF,TIMING OFF,FORMAT JSON)\n%s\n' "$(cat "$PROJECT/tests/$name.sql")" > "$RESULTS/current.sql"
                sh "$COLD" csd "$RESULTS/current.sql" > "$RESULTS/$name.json"
            done
        fi
        ;;
    *) echo 'usage: pg-sina-query-test.sh query|memory|profile|analyze|bench|order-bench' >&2; exit 2 ;;
esac
