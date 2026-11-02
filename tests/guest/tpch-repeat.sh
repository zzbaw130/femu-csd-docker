#!/bin/sh
# 每条SQL/模式从冷态连续执行n次，间隔1秒，默认五次。
export TPCH_CACHE_MODE=repeat TPCH_RUNS=${TPCH_RUNS:-5}
exec sh "$(dirname -- "$0")/tpch-test.sh" "$@"
