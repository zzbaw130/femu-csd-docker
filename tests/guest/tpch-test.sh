#!/bin/bash
set -eu

ROOT=$(dirname $(readlink -f $0))
QUERIES=${ROOT}/queries

PGDATA=$(psql postgres -tc "SHOW data_directory;")
DB=$(basename ${PGDATA})

RESULTS=${ROOT}/../results/${DB}
mkdir -p ${RESULTS}

stats() {
    psql ${DB} <<SQL
SELECT * FROM pg_stat_bgwriter;
SELECT * FROM pg_stat_database WHERE datname = '${DB}';
SQL
}

echo "== TPC-H on ${DB}"
stats > ${RESULTS}/stats-before.log 2>&1

for n in $(seq 1 22)
do
    echo "query ${n} is running..."
    psql ${DB} -f ${QUERIES}/${n}.explain.sql > ${RESULTS}/explain-${n}.log 2>&1
    psql ${DB} -f ${QUERIES}/${n}.sql         > ${RESULTS}/result-${n}.log  2>&1
done

stats > ${RESULTS}/stats-after.log 2>&1
echo "== done: $(realpath ${RESULTS})"