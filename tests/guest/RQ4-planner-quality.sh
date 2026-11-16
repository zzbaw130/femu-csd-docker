#!/bin/sh
# The cold/warm controls share an initial cache protocol. In repeat, each path
# creates its own cache history, so the fixed-path gap is descriptive only.
set -eu
src=${1:?supply frozen sweep directory}
out=${2:?supply output directory}
mkdir -p "$out"
awk -F '\t' '
    function cost(key, s) { s=$16; if (!index(s,key "=")) return ""; sub(".*" key "=","",s); sub(/[[:space:]].*/,"",s); sub(/ms$/,"",s); return s=="unsupported"?"":s }
    BEGIN {OFS=","; print "selectivity,cache_state,id,path,selected_estimate_ms,original_host_ms,original_csd_ms,shared_search_ms,planning_ms,execution_ms,net_total_ms,best_fixed,gap_pct"}
    NR>1 {
        h=cost("original HOST"); d=cost("original CSD"); s=cost("shared search"); chosen="";
        if($16~/query=(rewritten|original) PG physical tree/) chosen=d;
        if($16~/query=original HOST/) chosen=h;
        if($16~/query=shared PG Path search/) chosen=s;
        print $1,$2,$3,$11,chosen,h,d,s,$12,$13,$8,$9,$10
    }
' "$src/逐次对照.tsv" > "$out/RQ4-planner-quality.csv"
