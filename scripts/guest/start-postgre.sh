#!/bin/bash

FORCE_RELOAD=0
while getopts ":f" opt; do
    case ${opt} in
        f) FORCE_RELOAD=1 ;;
    esac
done
shift $((OPTIND - 1))

SF=${1:?usage: $0 [-f] <SF>}

SRC_TAR=/home/femu/data/tpch_s${SF}.tar
DEST_DIR=/data
PGDATA=${DEST_DIR}/tpch_s${SF}

# pgdata not exist or need force updating
if [ ! -e ${PGDATA} ] || [ ${FORCE_RELOAD} -eq 1 ]; then
    tar -xvf ${SRC_TAR} -C ${DEST_DIR}
fi

if ! command -v psql; then
    echo "Postgre not installed. Run install-guest-postgre.sh first"
    exit 0
fi

# 原生 PG 对照配置；写入数据目录，cold-query.sh 重启后同样生效。
sed -i \
    -e 's/^[#[:space:]]*max_parallel_workers_per_gather[[:space:]]*=.*/max_parallel_workers_per_gather = 4/' \
    -e "s/^[#[:space:]]*shared_buffers[[:space:]]*=.*/shared_buffers = '2GB'/" \
    -e "s/^[#[:space:]]*work_mem[[:space:]]*=.*/work_mem = '16MB'/" \
    "${PGDATA}/postgresql.conf"

pg_ctl -D $(psql postgres -tc "SHOW data_directory;" 2>/dev/null) stop >/dev/null 2>&1

pg_ctl -D ${PGDATA} -l ${PGDATA}/log start
