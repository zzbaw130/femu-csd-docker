#!/bin/sh
# Summaries for the four measured suites. Each query remains available in RQ1.
set -eu
src=${1:?supply RQ1-tpch-performance.csv}
out=${2:?supply output directory}
mkdir -p "$out"
awk -F, '
    BEGIN { print "target_selectivity,cache_state,host_sum_ms,filter_sum_ms,csd_off_sum_ms,csd_on_sum_ms,sqlerate_sum_ms,csd_count,mix_count,host_count" }
    NR>1 {
        key=$1 SUBSEP $2
        if (!(key in seen)) {seen[key]=1; order[++n]=key}
        if ($13!="yes" || (NF>=14 && $14!="\"host-only=ok;filter-only=ok;csd-without-hpcp=ok;csd-with-hpcp=ok;SQLerate=ok\"")) next
        complete[key]++
        for (i=4;i<=8;i++) latency[key,i]+=$i
        count[key,$9]++
    }
    END {
        for (j=1;j<=n;j++) {
            k=order[j]; split(k,a,SUBSEP); printf "%s,%s",a[1],a[2]
            for (i=4;i<=8;i++) {
                if (complete[k]==(a[2]=="repeat"?66:22)) printf ",%.3f",latency[k,i]
                else printf ","
            }
            printf ",%d,%d,%d\n",count[k,"CSD"],count[k,"Mix"],count[k,"HOST"]
        }
    }
' "$src" > "$out/RQ5-selectivity.csv"
