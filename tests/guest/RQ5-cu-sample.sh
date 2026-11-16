#!/bin/sh
# Called by RQ5-cu-quota.sh after the running FEMU CU quotas have been set.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?supply an absolute output directory}
quota=${2:?supply quota in microseconds per 10ms}
mkdir -p "$out/logs"
. "$root/paper-targeted-common.sh"
for query in 6 18; do
    sample "$quota" "$query" 1 csd-without-hpcp off off 0
    check_rows "$out/logs/Q$query.$quota.1.out" 1 > "$out/logs/Q$query.$quota.rows"
    # Compare all levels against the first quota; RQ2 validates the fixed path
    # against HOST independently.
    if ! cmp -s "$out/logs/Q$query.2000.rows" "$out/logs/Q$query.$quota.rows"; then
        echo mismatch > "$out/logs/Q$query.$quota.1.status"
    fi
done
extract_samples RQ5-cu-quota.csv
