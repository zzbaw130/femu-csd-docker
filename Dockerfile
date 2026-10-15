# FEMU in Docker: build the emulator, then boot a guest VM in CSD mode
# (femu_mode=4) with KVM.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# femu-scripts/pkgdep.sh dependencies, plus what configure, the guest image
# builder (make-guest-image.sh) and the CSD example kernels (hw/femu/tests/csd,
# which link against lz4) need.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config git ninja-build python3-venv flex bison \
        libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev libdw-dev \
        libaio-dev libslirp-dev libnuma-dev liblz4-dev \
        sudo ca-certificates curl cloud-image-utils qemu-utils openssh-client \
    && rm -rf /var/lib/apt/lists/*

COPY code/FEMU /femu
WORKDIR /femu/build-femu
RUN cp ../femu-scripts/femu-copy-scripts.sh . \
    && ./femu-copy-scripts.sh \
    && ./femu-compile.sh

# First run: build the guest image that run-csd.sh expects
# ($HOME/images/u20s.qcow2; needs network and /dev/kvm, takes a few minutes).
# Its cloud-init already carries the 9p mount of the share run-csd.sh exports
# (see hw/femu/scripts/make-guest-image.sh), so every boot after that shows
# /home/femu/workspace. Delete the image to have it built again.
CMD test -f "$HOME/images/u20s.qcow2" \
    || ./make-guest-image.sh --packages build-essential; \
    exec ./run-csd.sh
