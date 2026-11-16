#!/bin/sh
# Assemble the raw cold/warm/repeat CSVs from the standard benchmark entry points.
set -eu
src=${1:?supply the benchmark sweep directory}
out=${2:?supply output directory}
mkdir -p "$out"
table=$src/逐次对照.tsv
if [ -f "$src/sel0.1/cold.csv" ]; then
    table=$out/逐次对照.tsv
    printf 'sel\tmode\tid\tHOST\tFilter\tCSD-\tCSD+\tSQLerate\tbest\tgap_pct\tpath\tplan_ms\texec_ms\traw_ms\tfixed_csd_vs_host_filter_pct\tplacement\tchecks\tfdm_clean\n' > "$table"
    for sel in 0.1 0.2 0.5 0.8; do
        [ -d "$src/sel$sel" ] || continue
        for mode in cold warm repeat; do
            prefix=$src/sel$sel/$mode
            if [ ! -s "$prefix.csv" ]; then prefix=$src/sel$sel/partial/$mode; fi
            [ -s "$prefix.csv" ] && [ -s "$prefix-sqlerate.csv" ] || continue
            awk -F, -v sel="$sel" -v mode="$mode" '
                BEGIN {OFS="\t"; split("HOST Filter CSD- CSD+", names, " ")}
                FNR==1 {next}
                NR==FNR {
                    plan[$1]=$2; execution[$1]=$3; raw[$1]=$4;
                    path[$1]=$7; sub(/^"/,"",path[$1]); sub(/"$/,"",path[$1]);
                    status[$1]=$8;
                    next
                }
                {
                    id=$1; checks=$(NF-1); best=0; fixed_csd=0; base=0;
                    split(checks, valid, ";");
                    for (i=1;i<=5;i++) {
                        net[i]=$(i+1)==""?"":sprintf("%.3f",$(i+1)-$(i+6));
                        if (i<=4 && net[i]!="" && valid[i]~/=ok"?$/ &&
                            (!best || net[i]+0<net[best]+0)) best=i;
                        if (i<=2 && net[i]!="" && valid[i]~/=ok"?$/ &&
                            (!base || net[i]+0<net[base]+0)) base=i;
                        if (i>=3 && i<=4 && net[i]!="" && valid[i]~/=ok"?$/ &&
                            (!fixed_csd || net[i]+0<net[fixed_csd]+0)) fixed_csd=i;
                    }
                    route=path[id]; sub(/; query=.*/,"",route);
                    route=route=="subtree:CSD"?"CSD":route~/subtree:CSD/?"Mix":route!=""?"HOST":"";
                    valid_group=checks=="\"host-only=ok;filter-only=ok;csd-without-hpcp=ok;csd-with-hpcp=ok;SQLerate=ok\"" && $NF=="yes";
                    gap=(valid_group && best && status[id]=="ok" && net[best]>0)?sprintf("%.3f",100*(net[5]/net[best]-1)):"";
                    fixed_gap=(base && fixed_csd && net[base]>0)?sprintf("%.3f",100*(net[fixed_csd]/net[base]-1)):"";
                    print sel,mode,id,net[1],net[2],net[3],net[4],net[5],names[best],gap,route,plan[id],execution[id],raw[id],fixed_gap,path[id],checks,$NF;
                }
            ' "$prefix-sqlerate.csv" "$prefix.csv" >> "$table"
        done
    done
fi
awk -F '\t' 'BEGIN {OFS=","; print "selectivity,cache_state,id,host_ms,filter_ms,csd_off_ms,csd_on_ms,sqlerate_ms,path,planning_ms,execution_ms,raw_ms,fdm_clean,checks"}
    NR>1 {print $1,$2,$3,$4,$5,$6,$7,$8,$11,$12,$13,$14,$18,$17}
' "$table" > "$out/RQ1-tpch-performance.csv"
