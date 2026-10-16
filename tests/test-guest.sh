#!/bin/bash
if [[ $# -eq 0 ]]; then
    echo "usage: $0 <guest_test_script> [args...]" >&2
    exit 1
fi

GUEST_TEST_PATH=/home/femu/tests/$(basename $1)
shift

docker compose exec -T femu-csd ./run-guest-ssh.sh bash -l ${GUEST_TEST_PATH} "$@"