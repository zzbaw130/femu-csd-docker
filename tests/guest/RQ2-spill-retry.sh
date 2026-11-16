#!/bin/sh
# Guest: sh RQ2-spill-retry.sh /absolute/output /absolute/RQ2-spill-budget
# Independently retry the failed Q18 sample without overwriting the original.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?supply an absolute output directory}
reference=${2:?supply RQ2-spill-budget directory}
mkdir -p "$out/logs"
. "$root/paper-targeted-common.sh"
sample 128 18 1 csd-without-hpcp off off 128
if [ "$(cat "$out/logs/Q18.128.1.status")" = ok ]; then
    check_rows "$reference/logs/Q18.host.1.out" 1 > "$out/logs/reference.rows"
    check_rows "$out/logs/Q18.128.1.out" 1 > "$out/logs/current.rows"
    if ! cmp -s "$out/logs/reference.rows" "$out/logs/current.rows"; then
        echo mismatch > "$out/logs/Q18.128.1.status"
    fi
    rm "$out/logs/reference.rows" "$out/logs/current.rows"
fi
extract_samples RQ2-spill-retry.csv
