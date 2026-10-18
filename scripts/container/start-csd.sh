#!/usr/bin/env bash
# Compose uses the mounted launcher, so launch configuration follows this checkout.
set -euo pipefail
if [[ ! -f /root/images/u20s.qcow2 ]]; then
    ./make-guest-image.sh --packages build-essential,zlib1g-dev,libreadline-dev,bison,flex
fi
exec /workspace/code/FEMU/hw/femu/scripts/run-csd.sh
