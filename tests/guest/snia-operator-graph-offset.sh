#!/usr/bin/env bash
# Run inside the SF8 Guest with the branch's extension and CSD programs built.
set -euo pipefail
out=${1:?usage: snia-operator-graph-offset.sh ABSOLUTE_RESULTS_DIR ABSOLUTE_PG_MODULE [MODE ...]}
module=${2:?provide the path to the experimental pg_sina_csd.so}
shift 2
if [[ $out != /* || $module != /* || ! -f $module ]]; then
    echo 'Provide an absolute results path and an existing extension module.' >&2
    exit 2
fi
if [[ $# -eq 0 ]]; then set -- pg-sina-plan snia-operator-graph host-reference; fi

export PATH=/home/femu/opt/postgres/release/bin:$PATH
export LC_ALL=C PGDATA=/data/tpch_s8 PGDATABASE=tpch_s8
export PGOPTIONS="-c session_preload_libraries=$module,auto_explain"
mkdir -p "$out"
printf '%s\n' 'mode,status,sum,planning_ms,execution_ms,client_ms,workers,scan_wall_ms,merge_wall_ms,spill_read_bytes,spill_write_bytes' > "$out/offset100.csv"
for mode; do
    placement=csd-without-hpcp
    graph=off
    case $mode in
        pg-sina-plan) ;;
        snia-operator-graph) graph=on ;;
        host-reference) placement=host-only ;;
        *) echo "Unknown mode $mode" >&2; exit 2 ;;
    esac

    pg_ctl -D "$PGDATA" -m fast -w stop >/dev/null
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    pg_ctl -D "$PGDATA" -l "$PGDATA/log" -w start \
        -o '-c autovacuum=off -c shared_buffers=2GB -c work_mem=16MB -c max_parallel_workers_per_gather=4 -c max_parallel_workers=8 -c max_worker_processes=8' >/dev/null

    echo "Running $mode" >&2
    status=ok
    psql -XqAt -v ON_ERROR_STOP=1 -v placement="$placement" -v graph="$graph" \
        > "$out/$mode.out" 2> "$out/$mode.log" <<'SQL' || status=error
SET statement_timeout = '300s';
SET pg_sina_csd.exe_path = :'placement';
SET pg_sina_csd.hpcp = off;
SET pg_sina_csd.dcp = off;
SET pg_sina_csd.max_memory = '128MB';
SET pg_sina_csd.profile = on;
SET pg_sina_csd.experimental_operator_graph = :'graph';
SET auto_explain.log_min_duration = 0;
SET auto_explain.log_analyze = on;
SET auto_explain.log_timing = off;
SET auto_explain.log_level = notice;
\timing on
SELECT SUM(l_extendedprice)
FROM (
    SELECT l_extendedprice
    FROM lineitem
    WHERE l_shipdate < DATE '1993-01-01'
    ORDER BY l_extendedprice DESC
    OFFSET 100
) AS sorted;
SQL
    log=$out/$mode.log
    case $mode in
        pg-sina-plan) expected='CSD Executor: pg_sina_plan' ;;
        snia-operator-graph) expected='CSD Executor: SNIA operator graph' ;;
        host-reference) expected='Execution Path: host' ;;
    esac
    if [[ $status == ok && $mode != host-reference ]] && ! grep -Fq "$expected" "$log"; then
        status=unexpected_path
    fi
    result=$(awk '/^-?[0-9]+(\.[0-9]+)?$/ {print; exit}' "$out/$mode.out")
    client=$(awk '/^Time:/ {print $2; exit}' "$out/$mode.out")
    execution=$(awk '
        /duration: .* ms +plan:/ {for (i=1;i<NF;++i) if ($i=="duration:") duration=$(i+1)}
        /^Query Text: SELECT SUM\(l_extendedprice\)/ {print duration; exit}
    ' "$log")
    planning=$(awk -F 'total_ms=' '/CSD planner total_ms=/ {print $2; exit}' "$log")
    metrics=$(awk -F ': *' '
        /CSD Workers:/ {workers=$2+0}
        /CSD Scan Phase Wall:/ {scan=$2+0}
        /CSD Merge Phase Wall:/ {merge=$2+0}
        /CSD Spill Read:/ {read=$2+0; have_read=1}
        /CSD Spill Write:/ {write=$2+0; have_write=1}
        END {printf "%s,%s,%s,%s,%s",workers,scan,merge,have_read?read:"",have_write?write:""}
    ' "$log")
    if [[ $status == ok && ( -z $result || -z $execution || -z $client ) ]]; then
        status=incomplete
    fi
    printf '%s,%s,%s,%s,%s,%s,%s\n' "$mode" "$status" "$result" "$planning" "$execution" "$client" "$metrics" | tee -a "$out/offset100.csv"
done

awk -F, '
    NR>1 {
        if ($2!="ok") failed=1
        if (NR==2) reference=$3
        else if ("value=" $3 != "value=" reference) {
            printf "SUM mismatch for %s: %s vs %s\n", $1, $3, reference > "/dev/stderr"
            failed=1
        }
    }
    END {exit failed}
' "$out/offset100.csv"
