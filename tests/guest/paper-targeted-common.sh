# Shared setup for the targeted paper experiments. Source from a Guest entry point.
export PATH=/home/femu/opt/postgres/release/bin:$PATH
export LC_ALL=C
export PGDATA=/data/tpch_s8 PGDATABASE=tpch_s8
export PGOPTIONS="-c session_preload_libraries=${TPCH_PG_MODULE:-pg_sina_csd},auto_explain"
queries=$root/queries/sf8_sel0.1
case "$out" in
    /*) ;;
    *) echo "Use an absolute experiment output path." >&2; exit 2;;
esac
case "$out:$queries" in
    *[!a-zA-Z0-9_./:-]*) echo "Experiment paths must use shell-safe characters." >&2; exit 2;;
esac

cold_start() {
    pg_ctl -D "$PGDATA" -m fast -w stop >/dev/null
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    pg_ctl -D "$PGDATA" -l "$PGDATA/log" -w start \
        -o '-c autovacuum=off -c shared_buffers=2GB -c work_mem=16MB -c max_parallel_workers_per_gather=4 -c max_parallel_workers=8 -c max_worker_processes=8' >/dev/null
}

settings() {
    printf "SET pg_sina_csd.exe_path='%s'; SET pg_sina_csd.hpcp=%s; SET pg_sina_csd.dcp=%s; SET pg_sina_csd.max_memory='%sMB';\n" "$@"
}

# Each phase reads the same official SQL, in the same connection. Counter snapshots
# surround the SQL, with no diagnostic SQL executed between phases.
sample() {
    label=$1 query=$2 run=$3
    shift 3
    prefix=$out/logs/Q$query.$label.$run
    cold_start
    {
        echo "SET statement_timeout='300s'; SET pg_sina_csd.profile=on;"
        echo 'SET auto_explain.log_min_duration=0; SET auto_explain.log_level=notice; SET auto_explain.log_analyze=off;'
        phase=0
        while [ "$#" -gt 0 ]; do
            phase=$((phase + 1))
            settings "$1" "$2" "$3" "$4"
            shift 4
            printf '\\! cat /sys/block/nvme0n1/stat > %s.%s.nvme.before\n' "$prefix" "$phase"
            printf '\\! sudo cat /sys/kernel/debug/femu-cache/profile > %s.%s.cache.before\n' "$prefix" "$phase"
            printf '\\! echo PAPER_BEGIN %s >&2\n\\echo PAPER_BEGIN %s\n' "$phase" "$phase"
            phase_query=$query
            if [ "$phase" -eq 2 ]; then phase_query=${PAPER_NEXT_QUERY:-$query}; fi
            printf '\\i %s/%s.sql\n\\timing off\n' "$queries" "$phase_query"
            printf '\\echo PAPER_END %s\n\\! echo PAPER_END %s >&2\n' "$phase" "$phase"
            printf '\\! cat /sys/block/nvme0n1/stat > %s.%s.nvme.after\n' "$prefix" "$phase"
            printf '\\! sudo cat /sys/kernel/debug/femu-cache/profile > %s.%s.cache.after\n' "$prefix" "$phase"
            [ "$#" -eq 0 ] || printf '\\! sleep 1\n'
        done
    } > "$prefix.sql"
    status=ok
    psql -X -qAt -v ON_ERROR_STOP=1 -f "$prefix.sql" > "$prefix.out" 2> "$prefix.err" || status=error
    sudo nvme admin-passthru /dev/nvme0 --opcode=0xc0 --cdw10=2 > "$prefix.fdm" 2>&1
    if ! grep -q 'result: 0x00000000' "$prefix.fdm"; then status=fdm_leak; fi
    echo "$status" > "$prefix.status"
    printf '%s Q%s run %s %s\n' "$label" "$query" "$run" "$status"
}

extract_samples() {
    printf '%s\n' 'query,policy,run,phase,total_ms,maintenance_ms,host_read_bytes,dma_bytes,shared_pct,file_pct,known_pct,workmem_peak_bytes,spill_read_bytes,spill_write_bytes,result_bytes,status' > "$out/$1"
    for status_file in "$out"/logs/*.status; do
        prefix=${status_file%.status}
        sample_id=${prefix##*/}
        query=${sample_id%%.*}
        policy_run=${sample_id#*.}
        policy=${policy_run%.*}
        run=${policy_run##*.}
        for before in "$prefix".*.nvme.before; do
            phase=${before%.nvme.before}
            phase=${phase##*.}
            after="$prefix.$phase.nvme.after"
            host_bytes=
            dma_bytes=
            if [ -s "$after" ]; then
                host_bytes=$(awk 'NR==1 {b=$3} NR==2 {printf "%.0f",($3-b)*512}' "$before" "$after")
                dma_bytes=$(awk '/^CACHE_DEVICE / {for(i=2;i<=NF;i++) if ($i~/^dma_bytes=/) {split($i,a,"="); if(!seen++) b=a[2]; else printf "%.0f",a[2]-b}}' "$prefix.$phase.cache.before" "$prefix.$phase.cache.after")
            fi
            awk -v q="$query" -v policy="$policy" -v run="$run" -v phase="$phase" \
                -v host="$host_bytes" -v dma="$dma_bytes" -v status="$(cat "$status_file")" '
                function value(key,   s) {s=$0; sub(".*" key "=","",s); sub(/[^0-9.].*/,"",s); return s}
                /^PAPER_BEGIN / {active=$2==phase; next}
                /^PAPER_END / {if (active && FILENAME==ARGV[1]) complete=1; active=0; next}
                active && /^Time: / {ms+=$2; timed++}
                active && /cache ledger refresh reason=executor_end/ {end+=value("wall_ms")}
                active && /SQLerate cache resident / {shared=value("shared_pct"); file=value("file_pct"); known=value("known_pct")}
                active && /CSD Workmem Peak:/ {saw_peak=1; if ($(NF-1)+0>peak) peak=$(NF-1)+0}
                active && /CSD Spill Read:/ {saw_read=1; read+=$(NF-1)}
                active && /CSD Spill Write:/ {saw_write=1; write+=$(NF-1)}
                active && /CSD Host Result Bytes:/ {saw_result=1; result+=$(NF-1)}
                END {
                    if (!complete || !timed) status="error";
                    printf "%s,%s,%s,%s,%s,%.3f,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                        q,policy,run,phase,timed?sprintf("%.3f",ms):"",end,host,dma,shared,file,known,
                        (status=="ok" || saw_peak)?sprintf("%.0f",peak):"",
                        (status=="ok" || saw_read)?sprintf("%.0f",read):"",
                        (status=="ok" || saw_write)?sprintf("%.0f",write):"",
                        (status=="ok" || saw_result)?sprintf("%.0f",result):"",status
                }' "$prefix.out" "$prefix.err" >> "$out/$1"
        done
    done
}

check_rows() {
    awk -v phase="$2" '/^PAPER_BEGIN /{a=$2==phase; next} /^PAPER_END /{a=0} a && !/^(Time:|Timing is )/' "$1"
}
