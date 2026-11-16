#!/bin/sh
# Fixed paths with population disabled. Reuse the already measured cold/warm controls.
set -eu
src=${1:?supply frozen sel0.1 directory}
out=${2:?supply output directory}
mkdir -p "$out"
printf '%s\n' 'query,cache_state,host_ms,filter_ms,csd_ms' > "$out/M3-fixed-placement.csv"
for state in cold warm; do
    awk -F, -v s="$state" 'NR>1 && $1~/^Q(4|6|9|12|14|19)-/ {
        split($1,a,"-"); printf "%s,%s,%.3f,%.3f,%.3f\n",a[1],s,$2-$7,$3-$8,$4-$9
    }' "$src/$state.csv" >> "$out/M3-fixed-placement.csv"
done
