#!/bin/sh
# Guest: sh RQ2-spill-budget.sh /absolute/output [runs]
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?supply an absolute output directory}
runs=${2:-1}
mkdir -p "$out/logs"
. "$root/paper-targeted-common.sh"

for query in 3 18; do
    sample host "$query" 1 host-only off off 0
    sample filter "$query" 1 filter-only off off 0
    run=1
    while [ "$run" -le "$runs" ]; do
        for mib in 64 128 256 896; do
            sample "$mib" "$query" "$run" csd-without-hpcp off off "$mib"
        done
        run=$((run + 1))
    done
done
for file in "$out"/logs/*.out; do
    prefix=${file%.out}
    name=${prefix##*/}
    q=${name%%.*}
    check_rows "$out/logs/$q.host.1.out" 1 > "$out/logs/reference.rows"
    check_rows "$file" 1 > "$out/logs/current.rows"
    if ! cmp -s "$out/logs/reference.rows" "$out/logs/current.rows" && [ "$(cat "$prefix.status")" = ok ]; then
        echo mismatch > "$prefix.status"
    fi
done
rm "$out/logs/reference.rows" "$out/logs/current.rows"
extract_samples RQ2-spill-budget.csv
