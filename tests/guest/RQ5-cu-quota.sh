#!/bin/sh
# FEMU container: sh /workspace/tests/guest/RQ5-cu-quota.sh /home/femu/results/RQ5-cu-quota
# Change only the running device CUs' cgroup quotas. Leave all other threads
# untouched and restore the original quotas before exiting, including on failure.
set -eu
out=${1:?supply the absolute Guest output directory}
sample_script=${RQ5_GUEST_SAMPLE:-/home/femu/tests/RQ5-cu-sample.sh}
module=${TPCH_PG_MODULE:-pg_sina_csd}
path=$(awk -F: '$2 ~ /(^|,)cpu(,|$)/ {print $3; exit}' /proc/self/cgroup)
set -- /sys/fs/cgroup/cpu"${path%/}"/femu-cu-*/cu0
[ "$#" -eq 1 ] && [ -d "$1" ]
group=${1%/*}
set -- "$group"/cu[0-7]
[ "$#" -eq 8 ]
saved=$(cat "$1/cpu.cfs_quota_us")
for cu do
    [ "$(cat "$cu/cpu.cfs_quota_us")" = "$saved" ]
    [ "$(cat "$cu/cpu.cfs_period_us")" = 10000 ]
done
restore() {
    for cu do printf '%s\n' "$saved" > "$cu/cpu.cfs_quota_us"; done
}
trap 'restore "$@"' EXIT
trap 'exit 130' HUP INT TERM
for quota in 2000 "$saved" 5000; do
    for cu do printf '%s\n' "$quota" > "$cu/cpu.cfs_quota_us"; done
    ./run-guest-ssh.sh "env TPCH_PG_MODULE='$module' sh '$sample_script' '$out' '$quota'"
done
