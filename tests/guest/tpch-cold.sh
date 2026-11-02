#!/bin/sh
# 每次正式执行前清空PG和Guest文件缓存；默认每条SQL三次。
export TPCH_CACHE_MODE=cold TPCH_RUNS=${TPCH_RUNS:-3}
exec sh "$(dirname -- "$0")/tpch-test.sh" "$@"
