#!/bin/sh
# 由 tpch-cold/warm/repeat.sh 调用，位置参数为查询编号（省略时运行 1 至 22）。
# SF 默认 8。-n 次数，-b shared_buffers，-w 每个 Gather 的 worker 数，-m work_mem。
# -s 选择率，默认 0.1。运行对应 sf<SF>_sel<选择率> 下已生成的 SQL。
# -D PGDATA，-d 数据库，-o 输出目录（也可用 TPCH_RESULTS）。显式 PGDATA/PGDATABASE 优先于 SF。
# 包装入口用 TPCH_CACHE_MODE 指定缓存模式，TPCH_RUNS 指定默认次数。
# TPCH_MODES 可指定以空格分隔的执行模式；默认测试全部五种。
# TPCH_REFERENCE_LOGS 可指定同一批正式 SQL 的已有 HOST 日志，用于只测卸载模式时校验结果。
# TPCH_DCP=off 关闭启用 HPCP 的模式中的 DCP，默认两层同时启用。
# TPCH_COMPARE_PROFILE=1 为所有待测模式保留 EXPLAIN 和设备缓存汇总，串口输出每个样本的边界。
# 同时设置 SNIA_FEMU_PROFILE=1 可记录设备逐事件诊断，用于针对性测试。
# TPCH_WARM_LAYER=page、database 或 both 用于 warm 的 HOST 缓存对照。
# 公共执行部分；全量测试从 tpch-cold/warm/repeat.sh 进入。
set -eu
export PATH=/home/femu/opt/postgres/release/bin:$PATH
kind=${TPCH_CACHE_MODE:?use tpch-cold.sh, tpch-warm.sh or tpch-repeat.sh}
n=${TPCH_RUNS:-3}; buffers=2GB; workers=4; memory=16MB
selectivity=0.1
out=${TPCH_RESULTS:-}
while getopts 'n:b:w:m:s:D:d:o:' option; do
    case $option in
        n) n=$OPTARG ;; b) buffers=$OPTARG ;; w) workers=$OPTARG ;;
        m) memory=$OPTARG ;; s) selectivity=$OPTARG ;; D) PGDATA=$OPTARG ;; d) PGDATABASE=$OPTARG ;;
        o) out=$OPTARG ;; *) exit 2 ;;
    esac
done
shift $((OPTIND-1))
PGDATA=${PGDATA:-/data/${PGDATABASE:-tpch_s${SF:-8}}}
[ "$n" -gt 0 ]
case $buffers in *G|*M) buffers=${buffers}B ;; esac
case $memory in *G|*M) memory=${memory}B ;; esac
export PGDATA PGDATABASE=${PGDATABASE:-${PGDATA##*/}}
processes=$workers; [ "$processes" -ge 8 ] || processes=8
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
sf=${PGDATABASE#tpch_s}
case "$sf" in *[!0-9.]*) sf= ;; esac
if ! printf '%s\n' "$sf" | grep -Eq '^[0-9]+([.][0-9]+)?$' || [ "$PGDATABASE" != "tpch_s$sf" ]; then
    echo "数据库名必须使用 tpch_s<SF>：$PGDATABASE" >&2
    exit 2
fi
case "$selectivity" in *[!0-9.]*) selectivity= ;; esac
if ! printf '%s\n' "$selectivity" | grep -Eq '^(0([.][0-9]+)?|1([.]0+)?)$'; then
    echo '-s 需要 0 到 1 的目标选择率，例如 0.1' >&2
    exit 2
fi
query_dir=$root/queries/sf${sf}_sel$selectivity
[ "$#" -gt 0 ] || set -- 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22
for q do
    case "$q" in
        [1-9]|1[0-9]|2[0-2]) ;;
        *) echo "查询编号必须为 1 到 22：$q" >&2; exit 2 ;;
    esac
    if [ ! -s "$query_dir/$q.sql" ]; then
        echo "缺少已生成的 SQL：$query_dir/$q.sql" >&2
        echo "请在宿主机运行 sh tests/guest/queries/selectivity/generate.sh $selectivity $sf" >&2
        exit 1
    fi
done
queries="$*"
out=${out:-/home/femu/results/tpch-$PGDATABASE-sel$selectivity}
modes=${TPCH_MODES:-'host-only filter-only csd-without-hpcp csd-with-hpcp SQLerate'}
reference_logs=${TPCH_REFERENCE_LOGS:-}
cache_mode_dcp=${TPCH_DCP:-on}
case "$cache_mode_dcp" in
    on|off) ;;
    *) echo 'TPCH_DCP 需要 on 或 off' >&2; exit 2 ;;
esac
compare_profile=${TPCH_COMPARE_PROFILE:-0}
warm_layer=${TPCH_WARM_LAYER:-}
case "$kind:$warm_layer" in
    *:) ;;
    warm:page|warm:database|warm:both) modes=host-only ;;
    *) echo 'TPCH_WARM_LAYER 仅支持 warm 模式下的 page、database 或 both' >&2; exit 2 ;;
esac
logs=$out/logs/$kind
mkdir -p "$logs"
tmp=$(mktemp -d /tmp/tpch-test.XXXXXX)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: > "$tmp/data"
printf 'PGDATA=%s\nPGDATABASE=%s\nselectivity=%s\nquery_dir=%s\nshared_buffers=%s\nmax_parallel_workers_per_gather=%s\nwork_mem=%s\n%s_runs=%s\n' \
    "$PGDATA" "$PGDATABASE" "$selectivity" "$query_dir" "$buffers" "$workers" "$memory" "$kind" "$n" > "$logs/settings.txt"
printf 'modes=%s\n' "$modes" >> "$logs/settings.txt"
printf 'cache_mode_dcp=%s\n' "$cache_mode_dcp" >> "$logs/settings.txt"
[ -z "$reference_logs" ] || printf 'reference_logs=%s\n' "$reference_logs" >> "$logs/settings.txt"
[ "$compare_profile" != 1 ] || printf 'compare_profile=1\n' >> "$logs/settings.txt"
[ -z "$warm_layer" ] || printf 'warm_layer=%s\n' "$warm_layer" >> "$logs/settings.txt"
export PGOPTIONS="-c session_preload_libraries=pg_sina_csd,auto_explain"
[ "$compare_profile" != 1 ] || export SNIA_FEMU_PROFILE=${SNIA_FEMU_PROFILE:-cache}
start_pg() {
    pg_ctl -D "$PGDATA" -l "$PGDATA/log" -w start -o "-c autovacuum=off -c shared_buffers=$buffers -c work_mem=$memory -c max_parallel_workers_per_gather=$workers -c max_parallel_workers=$processes -c max_worker_processes=$processes" > /dev/null
}
reset_cache() {
    pg_ctl -D "$PGDATA" -m fast -w stop > /dev/null
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    start_pg
}
for q in $queries; do
    query_sql=$query_dir/$q.sql
    rm -f "$tmp/reference"
    if [ -n "$reference_logs" ]; then
        awk '
            /^TPCH_BEGIN 1$/ { capture=1; next }
            /^TPCH_END 1$/ { complete=1; exit }
            capture && !/^(Time:|Timing is )/ { print }
            END { if (!complete) exit 1 }
        ' "$reference_logs/Q$q.host-only.1.out" > "$tmp/reference"
    fi
    for mode in $modes; do
        first=1
        while [ "$first" -le "$n" ]; do
            last=$n; [ "$kind" != cold ] || last=$first
            [ -z "$warm_layer" ] || last=$first
            reset_cache
            population=off; dcp=off; explain=-1
            prewarm=ok
            prefix=$logs/Q$q.$mode.$first
            case $mode in csd-with-hpcp|SQLerate) population=on; dcp=$cache_mode_dcp ;; esac
            [ "$mode" != SQLerate ] || explain=0
            [ "$compare_profile" != 1 ] || explain=0
            if [ -n "$warm_layer" ]; then
                # HOST 预热产生两层缓存。按对照条件保留指定缓存。
                psql -X -qAt -v ON_ERROR_STOP=1 -d "$PGDATABASE" \
                    -c "SET statement_timeout='300s'; SET pg_sina_csd.exe_path='host-only'; SET pg_sina_csd.hpcp=off; SET pg_sina_csd.dcp=off;" \
                    -f "$query_sql" > /dev/null 2>"$prefix.prewarm.err" || prewarm=error
                if [ "$warm_layer" = page ]; then
                    pg_ctl -D "$PGDATA" -m fast -w stop > /dev/null
                    start_pg
                elif [ "$warm_layer" = database ]; then
                    sync
                    sudo sh -c 'echo 1 > /proc/sys/vm/drop_caches'
                fi
            fi
            {
                printf "SET statement_timeout='300s'; SET pg_sina_csd.profile=off;\n"
                if [ "$kind" = warm ] && [ -z "$warm_layer" ]; then
                    printf "SET pg_sina_csd.exe_path='host-only'; SET pg_sina_csd.hpcp=off; SET pg_sina_csd.dcp=off;\n"
                    printf '\\o /dev/null\n\\i %s\n\\o\n\\timing off\n' "$query_sql"
                fi
                printf "SET pg_sina_csd.exe_path='%s'; SET pg_sina_csd.hpcp=%s; SET pg_sina_csd.dcp=%s; SET pg_sina_csd.profile=on;\n" "$mode" "$population" "$dcp"
                printf "SET auto_explain.log_min_duration=%s; SET auto_explain.log_level=notice; SET auto_explain.log_format=text; SET auto_explain.log_analyze=off; SET auto_explain.log_timing=off;\n" "$explain"
                if [ -n "$warm_layer" ]; then
                    printf 'SET auto_explain.log_min_duration=0; SET auto_explain.log_analyze=on; SET auto_explain.log_buffers=on;\n'
                fi
                r=$first
                while [ "$r" -le "$last" ]; do
                    # Test-driver markers run outside the measured SQL.
                    printf '\\! echo TPCH_BEGIN %s >&2\n' "$r"
                    printf '\\echo TPCH_BEGIN %s\n\\i %s\n\\timing off\n\\echo TPCH_END %s\n' "$r" "$query_sql" "$r"
                    printf '\\! echo TPCH_END %s >&2\n' "$r"
                    if [ "$kind" = repeat ] && [ "$r" -lt "$last" ]; then printf '\\! sleep 1\n'; fi
                    r=$((r+1))
                done
            } > "$tmp/driver.sql"
            # 累计计数器做前后差；不启用逐页计时，不在SQL计时内采集。
            sudo cat /sys/kernel/debug/femu-cache/profile > "$prefix.cache-before"
            [ -z "$warm_layer" ] || cat /sys/block/nvme0n1/stat > "$prefix.nvme-before"
            if [ "$compare_profile" = 1 ]; then
                printf 'TPCH_DEVICE_BEGIN Q%s %s %s\n' "$q" "$mode" "$first" | sudo tee /dev/ttyS0 >/dev/null
            fi
            # SQL错误写入状态，继续完成全量；Q15的DROP VIEW也能正常执行。
            psql -X -qAt -v ON_ERROR_STOP=0 -d "$PGDATABASE" -f "$tmp/driver.sql" > "$prefix.out" 2> "$prefix.err" || true
            if [ "$compare_profile" = 1 ]; then
                printf 'TPCH_DEVICE_END Q%s %s %s\n' "$q" "$mode" "$first" | sudo tee /dev/ttyS0 >/dev/null
            fi
            [ -z "$warm_layer" ] || cat /sys/block/nvme0n1/stat > "$prefix.nvme-after"
            sudo cat /sys/kernel/debug/femu-cache/profile > "$prefix.cache-after"
            awk -v q="$q" -v mode="$mode" -v first="$first" -v last="$last" -v dir="$tmp" -v prewarm="$prewarm" '
                function value(key, s) { s=$0; sub(".*" key "=", "", s); sub(/[^0-9.].*/, "", s); return s+0 }
                BEGIN { OFMT=CONVFMT="%.3f"; er=first-1; setup_error=prewarm!="ok" }
                FILENAME==ARGV[1] {
                    if($1=="TPCH_BEGIN") { r=$2; next }
                    if($1=="TPCH_END") { done[r]=1; r=0; next }
                    if(r && $1=="Time:") { total[r]+=$2; timed[r]++; next }
                    if(r && $0!~/^Timing is /) print > (dir "/" r ".rows")
                    next
                }
                /^TPCH_BEGIN / { er=$2; next }
                /^TPCH_END / { er=0; next }
                /SQLerate cache resident / {
                    if(er>=first && er<=last) cache[er]=sprintf("shared=%.3f;file=%.3f;known=%.3f",value("shared_pct"),value("file_pct"),value("known_pct")); next
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
                match=match
                if ! cmp -s "$tmp/reference" "$tmp/$r.rows"; then
                    match=mismatch
                    # Q11 orders by numeric value alone. A different order
                    # within equal values is legal; check order and all rows.
                    if [ "$q" -eq 11 ] &&
                       LC_ALL=C sort -s -c -t '|' -k2,2nr "$tmp/$r.rows" 2>/dev/null; then
                        LC_ALL=C sort "$tmp/reference" > "$tmp/reference.sorted"
                        LC_ALL=C sort "$tmp/$r.rows" > "$tmp/result.sorted"
                        if cmp -s "$tmp/reference.sorted" "$tmp/result.sorted"; then match=match; fi
                    fi
                fi
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
        count=split(modes,m," ");
        for(j=1;j<=count;j++) { name[j]=tolower(m[j]); gsub(/-/,"_",name[j]) }
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
