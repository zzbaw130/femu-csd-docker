#!/usr/bin/env bash

FORCE_REGEN=0
while getopts ":f" opt; do
    case ${opt} in
        f) FORCE_REGEN=1 ;;
    esac
done
shift $((OPTIND - 1))

ROOT=$(dirname $(realpath $0))
SF="${1:?usage: $0 [-f] <sf> [pgdata_root]}"
DATA_ROOT="${2:-/data/dengyao/pgdata}"
DB_NAME="tpch_s${SF}"
PGDATA="${DATA_ROOT}/${DB_NAME}"

# need force updating
if [ ${FORCE_REGEN} -eq 1 ]; then
    echo "remove data and regen..."
    rm -rf ${PGDATA}
elif [ -d ${PGDATA} ]; then
    echo "${PGDATA} is already exist, force re-gen use -f"
    exit 0
fi

mkdir -p "${DATA_ROOT}"

# pg_ctl -D $(psql postgres -tc "SHOW data_directory;" 2>/dev/null) stop >/dev/null 2>&1
pg_ctl -D $(psql postgres -tc "SHOW data_directory;") stop -w -m fast

set -eu

# --locale setting is to compatible with all languages
# -A trust setting is to suppress warning
initdb -D "${PGDATA}" --locale=C.UTF-8 -A trust
pg_ctl -D "${PGDATA}" -l "${PGDATA}/log" start

createdb "${DB_NAME}"
psql "${DB_NAME}" -f ${ROOT}/create-tables.sql

DUCKDB_SQL="LOAD tpch; LOAD postgres; CALL dbgen(sf=${SF});"
DUCKDB_SQL+=" ATTACH 'dbname=${DB_NAME}' AS pg (TYPE POSTGRES);"
for table in region nation part supplier partsupp customer orders lineitem; do
    DUCKDB_SQL+=" INSERT INTO pg.public.${table} SELECT * FROM ${table};"
done
duckdb -c "${DUCKDB_SQL}"

psql -d "${DB_NAME}" -f ${ROOT}/create-indexes.sql
psql -d "${DB_NAME}" -c 'ANALYZE;CHECKPOINT;'
psql -d postgres -c 'CREATE ROLE femu LOGIN SUPERUSER;'

tar -cvf ${ROOT}/../data/${DB_NAME}.tar -C ${DATA_ROOT} ${DB_NAME}

echo "Created ${DB_NAME}: PGDATA=${PGDATA}"
