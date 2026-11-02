#!/bin/sh
# 每条SQL/模式独立清缓存，以HOST预热一次，再测n次。
export TPCH_CACHE_MODE=warm TPCH_RUNS=${TPCH_RUNS:-3}
exec sh "$(dirname -- "$0")/tpch-test.sh" "$@"
