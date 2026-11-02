#!/bin/sh
# 公共执行部分；全量测试从 tpch-cold/warm/repeat.sh 进入。
set -eu
export PATH=/home/femu/opt/postgres/release/bin:$PATH
kind=${TPCH_CACHE_MODE:?use tpch-cold.sh, tpch-warm.sh or tpch-repeat.sh}
n=${TPCH_RUNS:-3}; buffers=2GB; workers=4; memory=16MB
PGDATA=${PGDATA:-/data/tpch_s1}
out=${TPCH_RESULTS:-}
while getopts 'n:b:w:m:D:d:o:' option; do
    case $option in
        n) n=$OPTARG ;; b) buffers=$OPTARG ;; w) workers=$OPTARG ;;
        m) memory=$OPTARG ;; D) PGDATA=$OPTARG ;; d) PGDATABASE=$OPTARG ;;
        o) out=$OPTARG ;; *) exit 2 ;;
    esac
done
shift $((OPTIND-1))
[ "$n" -gt 0 ]
case $buffers in *G|*M) buffers=${buffers}B ;; esac
case $memory in *G|*M) memory=${memory}B ;; esac
export PGDATA PGDATABASE=${PGDATABASE:-${PGDATA##*/}}
processes=$workers; [ "$processes" -ge 8 ] || processes=8
out=${out:-/home/femu/results/tpch-$PGDATABASE}
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ "$#" -gt 0 ] || set -- 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22
queries="$*"
modes='host-only filter-only csd-without-hpcp csd-with-hpcp SQLerate'
logs=$out/logs/$kind
mkdir -p "$logs"
tmp=$(mktemp -d /tmp/tpch-test.XXXXXX)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: > "$tmp/data"
printf 'PGDATA=%s\nPGDATABASE=%s\nshared_buffers=%s\nmax_parallel_workers_per_gather=%s\nwork_mem=%s\n%s_runs=%s\n' \
    "$PGDATA" "$PGDATABASE" "$buffers" "$workers" "$memory" "$kind" "$n" > "$logs/settings.txt"
export PGOPTIONS="-c session_preload_libraries=pg_sina_csd,auto_explain"
reset_cache() {
    pg_ctl -D "$PGDATA" -m fast -w stop > /dev/null
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    pg_ctl -D "$PGDATA" -l "$PGDATA/log" -w start -o "-c autovacuum=off -c shared_buffers=$buffers -c work_mem=$memory -c max_parallel_workers_per_gather=$workers -c max_parallel_workers=$processes -c max_worker_processes=$processes" > /dev/null
}
for q in $queries; do
    rm -f "$tmp/reference"
    for mode in $modes; do
        first=1
        while [ "$first" -le "$n" ]; do
            last=$n; [ "$kind" != cold ] || last=$first
            reset_cache
            population=off; explain=-1
            case $mode in csd-with-hpcp|SQLerate) population=on ;; esac
            [ "$mode" != SQLerate ] || explain=0
            {
                printf "SET statement_timeout='300s'; SET pg_sina_csd.profile=off;\n"
                if [ "$kind" = warm ]; then
                    printf "SET pg_sina_csd.exe_path='host-only'; SET pg_sina_csd.hpcp=off; SET pg_sina_csd.dcp=off;\n"
                    printf '\\o /dev/null\n\\i %s/queries/%s.sql\n\\o\n\\timing off\n' "$root" "$q"
                fi
                printf "SET pg_sina_csd.exe_path='%s'; SET pg_sina_csd.hpcp=%s; SET pg_sina_csd.dcp=%s; SET pg_sina_csd.profile=on;\n" "$mode" "$population" "$population"
                printf "SET auto_explain.log_min_duration=%s; SET auto_explain.log_level=notice; SET auto_explain.log_format=text; SET auto_explain.log_analyze=off; SET auto_explain.log_timing=off;\n" "$explain"
                r=$first
                while [ "$r" -le "$last" ]; do
                    printf '\\echo TPCH_BEGIN %s\n\\i %s/queries/%s.sql\n\\timing off\n\\echo TPCH_END %s\n' "$r" "$root" "$q" "$r"
                    if [ "$kind" = repeat ] && [ "$r" -lt "$last" ]; then printf '\\! sleep 1\n'; fi
                    r=$((r+1))
                done
            } > "$tmp/driver.sql"
            prefix=$logs/Q$q.$mode.$first
            # SQL错误写入状态，继续完成全量；Q15的DROP VIEW也能正常执行。
            psql -X -qAt -v ON_ERROR_STOP=0 -d "$PGDATABASE" -f "$tmp/driver.sql" > "$prefix.out" 2> "$prefix.err" || true
            awk -v q="$q" -v mode="$mode" -v first="$first" -v last="$last" -v dir="$tmp" '
                function value(key, s) { s=$0; sub(".*" key "=", "", s); sub(/[^0-9.].*/, "", s); return s+0 }
                BEGIN { OFMT=CONVFMT="%.3f"; er=first-1 }
                FILENAME==ARGV[1] {
                    if($1=="TPCH_BEGIN") { r=$2; next }
                    if($1=="TPCH_END") { done[r]=1; r=0; next }
                    if(r && $1=="Time:") { total[r]+=$2; timed[r]++; next }
                    if(r && $0!~/^Timing is /) print > (dir "/" r ".rows")
                    next
                }
                /SQLerate cache resident / {
                    er++; cache[er]=sprintf("shared=%.3f;file=%.3f;known=%.3f",value("shared_pct"),value("file_pct"),value("known_pct")); next
                }
                /SQLerate planner total_ms=/ { t=value("total_ms"); if(t>plan[er]) plan[er]=t }
                /duration: .* ms  plan:/ { s=$0; sub(/^.*duration: /,"",s); duration=s+0 }
                /Execution Mode: SQLerate/ { execution[er]=duration }
                /Placement:/ { s=$0; sub(/^.*Placement: /,"",s); path[er]=s }
                /cache ledger refresh reason=executor_end/ { ledger[er]+=value("wall_ms") }
                /ERROR:|FATAL:/ { if(er<first) setup_error=1; else error[er]=1 }
                END {
                    for(r=first;r<=last;r++) {
                        status=done[r] && timed[r] && !error[r] && !setup_error?"ok":"error";
                        if(mode=="SQLerate" && status=="ok" && (!plan[r] || !(r in execution) || !path[r])) status="missing_metrics";
                        printf "D\t%s\t%d\t%s\t%s\t%.3f\t%s\t%s\t%s\t%s\t%s\n",q,r,mode,timed[r]?sprintf("%.3f",total[r]):"",ledger[r],cache[r],plan[r],execution[r],path[r],status;
                    }
                }' "$prefix.out" "$prefix.err" >> "$tmp/data"
            r=$first
            while [ "$r" -le "$last" ]; do
                [ -f "$tmp/$r.rows" ] || : > "$tmp/$r.rows"
                if [ "$mode" = host-only ] && [ "$r" -eq 1 ]; then cp "$tmp/$r.rows" "$tmp/reference"; fi
                match=match; cmp -s "$tmp/reference" "$tmp/$r.rows" || match=mismatch
                printf 'C\t%s\t%s\t%s\t%s\n' "$q" "$r" "$mode" "$match" >> "$tmp/data"
                rm -f "$tmp/$r.rows"
                r=$((r+1))
            done
            first=$((last+1))
        done
    done
    sudo nvme admin-passthru /dev/nvme0 --opcode=0xc0 --cdw10=2 > "$logs/Q$q.fdm" 2>&1
    clean=yes; grep -q 'result: 0x00000000' "$logs/Q$q.fdm" || clean=no
    printf 'F\t%s\t%s\n' "$q" "$clean" >> "$tmp/data"
    printf '%s Q%s finished\n' "$kind" "$q"
done
# 只整理原始记录，不在执行过程中分析性能或改变模型。
awk -F '\t' -v qs="$queries" -v modes="$modes" -v n="$n" -v kind="$kind" -v out="$out" '
    function csv(s) { gsub(/"/,"\"\"",s); return "\"" s "\"" }
    $1=="D" { k=$2 SUBSEP $3 SUBSEP $4; ms[k]=$5; ledger[k]=$6; cache[k]=$7; plan[k]=$8; exec[k]=$9; path[k]=$10; status[k]=$11 }
    $1=="C" { check[$2 SUBSEP $3 SUBSEP $4]=$5 }
    $1=="F" { clean[$2]=$3 }
    END {
        count=split(modes,m," "); split("host_only filter_only csd_without_hpcp csd_with_hpcp sqlerate",name," ");
        timing=out "/" kind ".csv"; detail=out "/" kind "-sqlerate.csv";
        header="id";
        for(j=1;j<=count;j++) header=header "," name[j] "_ms";
        for(j=1;j<=count;j++) header=header "," name[j] "_executor_end_ms";
        if(kind!="cold") for(j=1;j<=count;j++) header=header "," name[j] "_cache_residency";
        print header ",checks,fdm_clean" > timing;
        print "id,planning_ms,execution_ms,total_ms,executor_end_ms,cache_residency,path,status" > detail;
        nq=split(qs,q," ");
        for(i=1;i<=nq;i++) for(r=1;r<=n;r++) {
            id="Q" q[i] "-" r; row=id; checks="";
            for(j=1;j<=count;j++) { k=q[i] SUBSEP r SUBSEP m[j]; row=row "," ms[k] }
            for(j=1;j<=count;j++) { k=q[i] SUBSEP r SUBSEP m[j]; row=row "," ledger[k] }
            if(kind!="cold") for(j=1;j<=count;j++) { k=q[i] SUBSEP r SUBSEP m[j]; row=row "," csv(cache[k]) }
            for(j=1;j<=count;j++) {
                k=q[i] SUBSEP r SUBSEP m[j]; s=status[k];
                if(s=="ok" && check[k]!="match") s="mismatch";
                if(s!="ok") bad=1;
                checks=checks (j>1?";":"") m[j] "=" s;
            }
            if(clean[q[i]]!="yes") bad=1;
            print row "," csv(checks) "," clean[q[i]] > timing;
            k=q[i] SUBSEP r SUBSEP "SQLerate"; s=status[k]; if(s=="ok" && check[k]!="match") s="mismatch";
            print id "," plan[k] "," exec[k] "," ms[k] "," ledger[k] "," csv(cache[k]) "," csv(path[k]) "," s > detail;
        }
        exit bad;
    }' "$tmp/data"
printf 'Saved %s/%s.csv and %s/%s-sqlerate.csv\n' "$out" "$kind" "$out" "$kind"
