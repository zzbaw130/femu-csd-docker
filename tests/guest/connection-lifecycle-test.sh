#!/bin/sh
# 调用方式为 在 Guest 运行 SF=8 sh connection-lifecycle-test.sh，无位置参数。
# SF 默认 8，PGDATA/PGDATABASE 可由 cold-query.sh 覆盖默认值。需已准备所选数据库的 CSD 映射。
# 同一连接执行 Q2/Q9，验证取消和恢复。
# Correctness only: reuse one connection, cancel a query, then execute again.
set -eu
OUT=$(mktemp -d /home/femu/results/connection-lifecycle.XXXXXX)
chmod 755 "$OUT"
DRIVER=$(mktemp /tmp/pg-sina-connection.XXXXXX.sql)
trap 'rm -f "$DRIVER"' EXIT
cat > "$DRIVER" <<EOF
SET pg_sina_csd.exe_path='host-only';
\\o $OUT/host.2.log
\\i /home/femu/tests/queries/template/2.sql
\\o $OUT/host.9.log
\\i /home/femu/tests/queries/template/9.sql
SET pg_sina_csd.exe_path='SQLerate';
EOF
for round in 1 2; do
    for query in 2 9; do
        printf '\\o %s/reuse.%s.%s.log\n\\i /home/femu/tests/queries/template/%s.sql\n' \
            "$OUT" "$round" "$query" "$query" >> "$DRIVER"
    done
done
cat >> "$DRIVER" <<EOF
SET statement_timeout='200ms';
\\set ON_ERROR_STOP off
\\o $OUT/cancel.log
\\i /home/femu/tests/queries/template/9.sql
\\set ON_ERROR_STOP on
SET statement_timeout=0;
\\o $OUT/recovered.9.log
\\i /home/femu/tests/queries/template/9.sql
EOF
PG_SINA_PROFILE=on sh /home/femu/scripts/cold-query.sh SQLerate "$DRIVER" \
    > "$OUT/driver.log" 2> "$OUT/driver.err"
[ "$(grep -c 'SQLerate connection init ' "$OUT/driver.err")" -eq 1 ]
grep -q 'canceling statement due to statement timeout' "$OUT/driver.err"
if grep 'SQLerate planner total_ms=' "$OUT/driver.err" | grep -vq 'resources_inclusive_ms=0.000'; then exit 1; fi
for query in 2 9; do
    sed '/^Time:/d; /^Timing is /d' "$OUT/host.$query.log" > "$OUT/reference"
    for round in 1 2; do
        sed '/^Time:/d; /^Timing is /d' "$OUT/reuse.$round.$query.log" > "$OUT/actual"
        cmp "$OUT/reference" "$OUT/actual"
    done
done
sed '/^Time:/d; /^Timing is /d' "$OUT/recovered.9.log" > "$OUT/actual"
cmp "$OUT/reference" "$OUT/actual"
rm -f "$OUT/reference" "$OUT/actual"
echo "Connection lifecycle regression passed: $OUT"
