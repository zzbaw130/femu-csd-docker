#!/bin/sh
# Match the measured cold timings with calibrated per-query retention and spill.
set -eu
src=${1:?supply the benchmark sweep directory}
out=${2:?supply output directory}
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$out"
printf '%s\n' 'query,target_selectivity,retained_fraction,retained_rows,input_rows,net_ms,workmem_peak_bytes,spill_read_bytes,spill_write_bytes' > "$out/RQ5-query-sensitivity.csv"
for sel in 0.1 0.2 0.5 0.8; do
    summary=$src/sel$sel/cold.csv
    if [ ! -s "$summary" ]; then summary=$src/sel$sel/partial/cold.csv; fi
    [ -s "$summary" ] || continue
    for q in 1 11 18; do
        profile=$src/sel$sel/logs/cold/Q$q.csd-without-hpcp.1.err
        if [ ! -s "$profile" ]; then profile=/dev/null; fi
        awk -v q="Q$q" -v sel="$sel" '
            FILENAME==ARGV[1] && /\(selectivity=/ {
                split($0,t,"selectivity="); fraction=t[2]+0;
                split($(NF-1),r,"/"); retained=r[1]; input=r[2]
            }
            FILENAME==ARGV[2] {
                split($0,a,","); if(a[1]==q "-1" && $0~/csd-without-hpcp=ok/) net=a[4]-a[9]
            }
            /CSD Workmem Peak:/ {peak=$(NF-1)}
            /CSD Spill Read:/ {read+=$(NF-1)}
            /CSD Spill Write:/ {write+=$(NF-1)}
            END {
                if(!input) exit 1;
                if(!net || !peak) printf "%s,%s,%.8f,%s,%s,,,,\n",q,sel,fraction,retained,input;
                else printf "%s,%s,%.8f,%s,%s,%.3f,%.0f,%.0f,%.0f\n",q,sel,fraction,retained,input,net,peak,read,write
            }
        ' "$root/queries/sf8_sel$sel/$q.sql" "$summary" "$profile" >> "$out/RQ5-query-sensitivity.csv"
    done
done
