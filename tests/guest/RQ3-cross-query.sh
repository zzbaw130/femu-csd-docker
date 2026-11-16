#!/bin/sh
# Guest: sh RQ3-cross-query.sh /absolute/output [runs]
# Q6 reads lineitem first. A different HOST query (Q1) consumes the warmed data.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?supply an absolute output directory}
runs=${2:-3}
mkdir -p "$out/logs"
. "$root/paper-targeted-common.sh"
export PAPER_NEXT_QUERY=1
if [ "$runs" != --from-logs ]; then
    run=1
    while [ "$run" -le "$runs" ]; do
        sample neither 6 "$run" csd-with-hpcp off off 0 host-only off off 0
        sample both 6 "$run" csd-with-hpcp on on 0 host-only off off 0
        for phase in 1 2; do
            check_rows "$out/logs/Q6.neither.$run.out" "$phase" > "$out/logs/reference.rows"
            check_rows "$out/logs/Q6.both.$run.out" "$phase" > "$out/logs/current.rows"
            if ! cmp -s "$out/logs/reference.rows" "$out/logs/current.rows"; then
                echo mismatch > "$out/logs/Q6.both.$run.status"
            fi
        done
        run=$((run+1))
    done
    rm "$out/logs/reference.rows" "$out/logs/current.rows"
fi
extract_samples RQ3-cross-query.csv
awk -F, 'NR==1 {print $0 ",executed_query"; next} {print $0 "," ($4==1?"Q6":"Q1")}' \
    "$out/RQ3-cross-query.csv" > "$out/RQ3-cross-query.tmp"
mv -f "$out/RQ3-cross-query.tmp" "$out/RQ3-cross-query.csv"
