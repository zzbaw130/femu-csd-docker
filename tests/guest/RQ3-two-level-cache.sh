#!/bin/sh
# Guest: sh RQ3-two-level-cache.sh /absolute/output [runs]
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?supply an absolute output directory}
runs=${2:-3}
mkdir -p "$out/logs"
. "$root/paper-targeted-common.sh"

for query in 6 4; do
    run=1
    while [ "$run" -le "$runs" ]; do
        sample host "$query" "$run" host-only off off 0 host-only off off 0
        for policy in neither hpcp dcp both; do
            hpcp=off; dcp=off
            case $policy in hpcp|both) hpcp=on;; esac
            case $policy in dcp|both) dcp=on;; esac
            sample "$policy" "$query" "$run" csd-with-hpcp "$hpcp" "$dcp" 0 host-only off off 0
        done
        run=$((run + 1))
    done
done
for file in "$out"/logs/*.out; do
    prefix=${file%.out}
    name=${prefix##*/}
    q=${name%%.*}
    run=${name##*.}
    check_rows "$out/logs/$q.host.$run.out" 1 > "$out/logs/reference.rows"
    for phase in 1 2; do
        check_rows "$file" "$phase" > "$out/logs/current.rows"
        if ! cmp -s "$out/logs/reference.rows" "$out/logs/current.rows" && [ "$(cat "$prefix.status")" = ok ]; then
            echo mismatch > "$prefix.status"
        fi
    done
done
rm "$out/logs/reference.rows" "$out/logs/current.rows"
extract_samples RQ3-two-level-cache.csv
