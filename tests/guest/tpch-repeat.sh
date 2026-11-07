#!/bin/sh
# 调用方式为 在 Guest 运行 SF=8 sh tpch-repeat.sh [-n 次数] [-b 缓冲] [-w worker] [-m 工作内存] [-s 选择率] [-D PGDATA] [-d 数据库] [-o 输出目录] [查询编号...]。
# 参数由 tpch-test.sh 处理。SF 默认 8，-n 默认 3，-s 默认 0.1，省略查询编号运行全部。
# 每条SQL/模式从冷态连续执行n次，间隔1秒，默认三次。
export TPCH_CACHE_MODE=repeat TPCH_RUNS=${TPCH_RUNS:-3}
exec sh "$(dirname -- "$0")/tpch-test.sh" "$@"
