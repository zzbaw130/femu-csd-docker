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

pg_ctl -D $(psql postgres -tc "SHOW data_directory;" 2>/dev/null) stop >/dev/null 2>&1

pg_ctl -D ${PGDATA} -l ${PGDATA}/log start
