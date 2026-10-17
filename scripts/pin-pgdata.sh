#!/usr/bin/env bash

ROOT=$(dirname $(realpath $0))
SF=${1:?usage: $0 <sf> [pgdata_root]}
DATA_ROOT=${2:-/data/dengyao/pgdata}
DB_NAME=tpch_s${SF}
PGDATA=${DATA_ROOT}/${DB_NAME}

# pg_ctl -D $(psql postgres -tc "SHOW data_directory;" 2>/dev/null) stop >/dev/null 2>&1
cur_pgdata=$(psql postgres -tc "SHOW data_directory;")
if [ ${PGDATA} != ${cur_pgdata} ]; then
    pg_ctl -D ${cur_pgdata} stop -w -m fast
    pg_ctl -D ${PGDATA} -l ${PGDATA}/log start
fi

set -eu

# load extension
psql ${DB_NAME} -c "CREATE EXTENSION IF NOT EXISTS pg_visibility;"
# VACUUM
psql ${DB_NAME} -c "VACUUM (FREEZE, ANALYZE);"

# check visible and frozen
result=$(psql ${DB_NAME} -tA <<SQL
SELECT
    COALESCE(bool_and(v.all_visible = pg_relation_size(c.oid) / 8192), true) AS all_pages_visible,
    COALESCE(bool_and(v.all_frozen  = pg_relation_size(c.oid) / 8192), true) AS all_pages_frozen
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
CROSS JOIN LATERAL pg_visibility_map_summary(c.oid::regclass)
    AS v(all_visible, all_frozen)
WHERE c.relkind = 'r'
  AND n.nspname NOT IN ('pg_catalog', 'information_schema');
SQL
)
visible=$(echo "$result" | cut -d'|' -f1)
frozen=$(echo "$result" | cut -d'|' -f2)

if [ "$visible" = "t" ] && [ "$frozen" = "t" ]; then
    echo "所有表已完全可见且冻结，执行 CHECKPOINT"
    psql ${DB_NAME} -c 'CHECKPOINT;'

    pg_ctl -D ${PGDATA} stop -w -m fast
    tar -cf ${ROOT}/../data/${DB_NAME}.tar -C ${DATA_ROOT} ${DB_NAME}
    echo Created stable ${DB_NAME}.tar
    exit 0
fi

echo "尚有表未稳定:"
psql ${DB_NAME} <<SQL
SELECT
    c.relname AS table_name,
    pg_relation_size(c.oid) / 8192 AS total_pages,
    v.all_visible,
    v.all_frozen,
    pg_relation_size(c.oid) / 8192 - v.all_visible AS not_all_visible,
    pg_relation_size(c.oid) / 8192 - v.all_frozen  AS not_all_frozen
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
CROSS JOIN LATERAL pg_visibility_map_summary(c.oid::regclass)
    AS v(all_visible, all_frozen)
WHERE c.relkind = 'r'                     -- 只查普通表
  AND n.nspname NOT IN ('pg_catalog', 'information_schema')
ORDER BY not_all_visible DESC,
         not_all_frozen DESC,
         n.nspname,
         c.relname;
SQL
