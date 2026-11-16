#!/bin/sh
# Export the profiled device plan and individual pipeline counters.
set -eu
out=${1:?supply the directory produced by RQ2-spill-budget.sh}
printf '%s\n' 'query,workmem_mib,run,pipeline,rounds,wall_ms,sink_rows,spill_calls,spill_wait_sum_ms' > "$out/RQ2-spill-stages.csv"
printf '%s\n' 'query,workmem_mib,run,internal_read_bytes,tree_wall_ms,plan' > "$out/RQ2-spill-plans.csv"
for log in "$out"/logs/*.err; do
    name=${log##*/}
    IFS=. read -r q budget run suffix <<ID
$name
ID
    case $budget in host|filter) continue;; esac
    awk -v q="$q" -v cap="$budget" -v run="$run" -v out="$out" '
        function value(k,s) {s=$0; sub(".*"k"=","",s); sub(/[^0-9.].*/,"",s); return s+0}
        /CSD Plan:/ {plan=$0; sub(/.*CSD Plan: /,"",plan); gsub(/"/,"\"\"",plan)}
        /CSD Internal Read:/ {io=$(NF-1)}
        /CSD Tree Wall:/ {tree=$(NF-1)}
        /CSD Pipeline [0-9]+: rounds=/ {
            idx=$3+0; order[++n]=idx; rounds[idx]=value("rounds"); wall[idx]=value("wall"); rows[idx]=value("sink_rows")
        }
        /CSD Pipeline [0-9]+ spill_io:/ {calls[$3]=value("calls"); wait[$3]=value("task_wait_sum")}
        END {
            if (plan) printf "%s,%s,%s,%.0f,%.3f,\"%s\"\n",q,cap,run,io,tree,plan >> (out "/RQ2-spill-plans.csv")
            for (i=1;i<=n;i++) {
                k=order[i]; printf "%s,%s,%s,%d,%d,%.3f,%.0f,%d,%.3f\n",q,cap,run,k,rounds[k],wall[k],rows[k],calls[k],wait[k]
            }
        }
    ' "$log" >> "$out/RQ2-spill-stages.csv"
done
