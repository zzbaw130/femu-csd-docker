#!/bin/sh
# 调用方式为 在 Guest 运行 SF=8 sh tpch-warm.sh [-n 次数] [-b 缓冲] [-w worker] [-m 工作内存] [-s 选择率] [-D PGDATA] [-d 数据库] [-o 输出目录] [查询编号...]。
# 参数由 tpch-test.sh 处理。SF 默认 8，-n 默认 1，-s 默认 0.1，省略查询编号运行全部。
# 每条SQL/模式独立清缓存，以HOST预热一次，再测n次，默认一次。
export TPCH_CACHE_MODE=warm TPCH_RUNS=${TPCH_RUNS:-1}
exec sh "$(dirname -- "$0")/tpch-test.sh" "$@"
