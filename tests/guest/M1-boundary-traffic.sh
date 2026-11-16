#!/bin/sh
# Read the measured SF8 output.
set -eu
src=${1:?supply the measured sel0.1 directory}
out=${2:?supply output directory}
mkdir -p "$out"
csv=$out/M1-boundary-traffic.csv
printf '%s\n' 'query,policy,raw_ms,maintenance_ms,host_storage_read_bytes,csd_result_bytes' > "$csv"
for q in 3 6 14 18; do
    for policy in host-only filter-only; do
        case $policy in host-only) field=2;; filter-only) field=3;; esac
        awk -F, -v q="Q$q" -v f="$field" -v policy="$policy" '
            NR==FNR {if ($1==q "-1") {raw=$f; end=$(f+5)}; next}
            FNR==1 {FS="[[:space:]]+"; $0=$0}
            /SQLerate query IO available=1/ {
                for(i=1;i<=NF;i++) if($i~/^device_read_bytes=/) {split($i,a,"="); read+=a[2]}
            }
            /CSD Host Result Bytes:/ {returned+=$(NF-1)}
            END {
                if (!raw) exit 1;
                printf "%s,%s,%s,%s,%.0f,%.0f\n",q,policy,raw,end,read,returned
            }
        ' "$src/cold.csv" "$src/logs/cold/Q$q.$policy.1.err" >> "$csv"
    done
done
