# FEMU in Docker: build the emulator, then boot a guest VM in CSD mode
# (femu_mode=4) with KVM.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# femu-scripts/pkgdep.sh dependencies, plus what configure and the guest image
# builder (make-guest-image.sh) need.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config git ninja-build python3-venv flex bison \
        libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev libdw-dev \
        libaio-dev libslirp-dev libnuma-dev liblz4-dev libreadline-dev \
        sudo ca-certificates curl cloud-image-utils qemu-utils openssh-client \
    && rm -rf /var/lib/apt/lists/*


WORKDIR /workspace/FEMU/build-femu

# compile FEMU source code
COPY code/FEMU /workspace/FEMU
RUN cp ../femu-scripts/femu-compile.sh . \
    && ./femu-compile.sh

# copy frequently changing files
COPY --from=hw-femu-scripts . /workspace/FEMU/hw/femu/scripts
COPY --from=femu-scripts . /workspace/FEMU/femu-scripts
RUN cp ../femu-scripts/femu-copy-scripts.sh . \
    && ./femu-copy-scripts.sh

# First run: build the guest image that run-csd.sh expects
# (/root/images/u20s.qcow2; needs network and /dev/kvm, takes a few minutes).
# Its cloud-init already carries the 9p mount above, so every boot after that
# shows /home/femu/workspace. Delete the image to have it built again.

CMD test -f "/root/images/u20s.qcow2" || ./make-guest-image.sh --packages \
    build-essential,zlib1g-dev,libreadline-dev,bison,flex; \
    exec ./run-csd.sh
