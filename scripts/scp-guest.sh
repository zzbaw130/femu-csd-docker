#!/bin/bash

KEY=$(dirname $(realpath $0))/../images/femu-guest-key
HOST_PATH=${1:?usage: $0 <host_path> <guest_path>}
GUEST_PATH=${2:?usage: $0 <host_path> <guest_path>}

# change ssh key owner
if [ ! -r ${KEY} ]; then
    sudo chown $USER:$USER ${KEY}
fi

scp -P 8080 -i ${KEY} -r ${HOST_PATH} femu@localhost:${GUEST_PATH}