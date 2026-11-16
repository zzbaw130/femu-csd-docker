#!/bin/sh
# Select per-query cache trajectories from the independently measured repeat groups.
set -eu
src=${1:?supply frozen sel0.1 directory}
out=${2:?supply output directory}
mkdir -p "$out"
awk -F, '
    function value(k,s) {sub(".*"k"=","",s); sub(/[^0-9.].*/,"",s); return s}
    BEGIN {print "query,run,policy,net_ms,shared_pct,file_pct,known_pct"; split("HOST Filter CSD- CSD+ SQLerate",m," ")}
    NR>1 && $1~/^Q(4|6|12|19)-/ {
        split($1,a,"-");
        for(i=2;i<=6;i++) {
            s=$(i+10)
            if (!index(s,"known=")) exit 1;
            printf "%s,%s,%s,%.3f,%s,%s,%s\n",a[1],a[2],m[i-1],$i-$(i+5),value("shared",s),value("file",s),value("known",s)
        }
    }
' "$src/repeat.csv" > "$out/RQ3-repeat-residency.csv"
