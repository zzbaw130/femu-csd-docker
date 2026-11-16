#!/bin/sh
# Guest: sh RQ3-dma-overlap.sh /absolute/output
# Device event logs are collected separately from the FEMU console.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?supply an absolute output directory}
mkdir -p "$out/logs"
. "$root/paper-targeted-common.sh"
export SNIA_FEMU_PROFILE=1
sample trace 6 1 csd-with-hpcp on on 0
extract_samples RQ3-dma-overlap.csv
test "$(cat "$out/logs/Q6.trace.1.status")" = ok
