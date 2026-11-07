#!/bin/bash
# Run FEMU as a Computational Storage Drive (CSD)
set -e -o pipefail

# Per-CU CPU time, not a whole-container quota. This host uses cgroup v1.
# 0/1 disable limits; Linux needs at least 1ms quota in the fixed 10ms period.
cu_cpu_ratio=${CSD_CU_CPU_RATIO:-0.3333333333}
cu_cpu_quota=$(awk -v ratio="$cu_cpu_ratio" 'BEGIN {
    if (ratio !~ /^(0([.][0-9]+)?|1([.]0+)?)$/ || (ratio > 0 && ratio < 0.1)) exit 1;
    print (ratio == 0 || ratio == 1) ? -1 : int(ratio * 10000 + 0.5);
}') || { echo 'CSD_CU_CPU_RATIO must be 0, 1, or within [0.1, 1); period is 10ms' >&2; exit 1; }

# Image directory
IMGDIR=${IMGDIR:-$HOME/images}
# Virtual machine disk image
OSIMGF=${OSIMGF:-$IMGDIR/u20s.qcow2}
# Host port forwarded to the guest's SSH port; run-guest-ssh.sh reads it too
SSH_PORT=${SSH_PORT:-8080}

# Configurable SSD Controller layout parameters (must be power of 2)
secsz=512 # sector size in bytes
secs_per_pg=8 # number of sectors in a flash page
pgs_per_blk=256 # number of pages per flash block
blks_per_pl=1024 # number of blocks per plane
pls_per_lun=1 # planes per LUN
luns_per_ch=8 # number of chips per channel
nchs=8 # number of channels
ssd_size=${CSD_NVM_SIZE} # in megabytes, 48GB with 25%(16GB) op

# Latency in nanoseconds
pg_rd_lat=40000 # page read latency
pg_wr_lat=200000 # page write latency
blk_er_lat=2000000 # block erase latency
ch_xfer_lat=0 # channel transfer time, ignored for now

# GC Threshold (1-100)
gc_thres_pcent=75
gc_thres_pcent_high=95

# FEMU CSD parameters. nr_thread, time_slice and context_switch_time are
# accepted for CEMU config compatibility but configure a scheduler this port
# does not carry, so they have no effect; values other than these defaults
# print a warning.
fdm_size=${CSD_FDM_SIZE:-128}
device_memory=${CSD_DEVICE_MEMORY_MB:-1024}
if [[ ! $fdm_size =~ ^[1-9][0-9]*$ || ! $device_memory =~ ^[1-9][0-9]*$ ]] ||
   (( device_memory % 2 || fdm_size >= device_memory )); then
    echo 'CSD device memory must be a positive multiple of 2 MiB and larger than FDM' >&2
    exit 1
fi
workmem_size=$((device_memory - fdm_size))
nr_cu=${CSD_NR_CU:-8}
nr_thread=4
time_slice=200000
context_switch_time=200

# container-side only; not shared with the guest
csd_program_dir=${CSD_PROGRAM_DIR}

#-----------------------------------------------------------------------

# Compose the entire FEMU CSD command line options
FEMU_OPTIONS="-device femu"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${ssd_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=4"
# Eight queues, four per poller: two normal I/O pollers plus two CSD DMA workers.
FEMU_OPTIONS=${FEMU_OPTIONS}",queues=8,multipoller_enabled=1,poller_ratio=4"
# Shared per-direction link budget in decimal MB/s (the FEMU property name
# uses mbps). Internal NVM/FDM copies do not traverse this link.
FEMU_OPTIONS=${FEMU_OPTIONS}",pcie_bandwidth_mbps=${CSD_PCIE_BANDWIDTH_MBPS:-3500}"
FEMU_OPTIONS=${FEMU_OPTIONS}",secsz=${secsz}"
FEMU_OPTIONS=${FEMU_OPTIONS}",secs_per_pg=${secs_per_pg}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pgs_per_blk=${pgs_per_blk}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blks_per_pl=${blks_per_pl}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS=${FEMU_OPTIONS}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nchs=${nchs}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent_high=${gc_thres_pcent_high}"
FEMU_OPTIONS=${FEMU_OPTIONS}",fdm_size=${fdm_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",csd_spill_size_mb=${CSD_SPILL_SIZE_MB:-256}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nr_cu=${nr_cu}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nr_thread=${nr_thread}"
FEMU_OPTIONS=${FEMU_OPTIONS}",time_slice=${time_slice}"
FEMU_OPTIONS=${FEMU_OPTIONS}",context_switch_time=${context_switch_time}"
FEMU_OPTIONS=${FEMU_OPTIONS}",csd_program_dir=${csd_program_dir}"

echo ${FEMU_OPTIONS}

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Build one with ./make-guest-image.sh, or place an image at $OSIMGF"
	echo "(set OSIMGF or IMGDIR to use another path), then rerun this script"
	echo ""
	exit 1
fi

# Keep groups beneath the current container's CPU cgroup, preserving its
# parent limits. No PID/TID scanning and no change to other device threads.
cu_cgroup=
if [ "$cu_cpu_quota" -ne -1 ]; then
    cpu_path=$(awk -F: '$2 ~ /(^|,)cpu(,|$)/ { print $3; exit }' /proc/self/cgroup)
    [ -n "$cpu_path" ] || { echo 'CU CPU quota requires the cgroup v1 CPU controller' >&2; exit 1; }
    cu_cgroup="/sys/fs/cgroup/cpu${cpu_path}/femu-cu-$$"
    mkdir "$cu_cgroup"
    cleanup_cu_cgroups() {
        for ((cu=0; cu<nr_cu; cu++)); do rmdir "$cu_cgroup/cu$cu" 2>/dev/null || true; done
        rmdir "$cu_cgroup" 2>/dev/null || true
    }
    trap cleanup_cu_cgroups EXIT
    for ((cu=0; cu<nr_cu; cu++)); do
        mkdir "$cu_cgroup/cu$cu"
        echo 10000 > "$cu_cgroup/cu$cu/cpu.cfs_period_us"
        echo "$cu_cpu_quota" > "$cu_cgroup/cu$cu/cpu.cfs_quota_us"
    done
fi
cu_cpu_effective=$(awk -v quota="$cu_cpu_quota" 'BEGIN { printf "%.4f", quota < 0 ? 1 : quota / 10000 }')
echo "CSD CU CPU ratio=$cu_cpu_ratio effective=$cu_cpu_effective quota=$cu_cpu_quota us period=10000 us"

# Bind thread groups at creation, without finding and repinning Linux TIDs.
# Default allocations belong to the device; Guest RAM overrides that policy.
sudo env CSD_WORKMEM_SIZE_MB="$workmem_size" \
    FEMU_CSD_HUGEPAGES=1 \
    CSD_CU_CPU_RATIO="$cu_cpu_effective" \
    FEMU_DEVICE_CPUS="${FEMU_DEVICE_CPUS}" \
    FEMU_CU_CGROUP="$cu_cgroup" \
    numactl --physcpubind="${FEMU_HOST_CPUS}" --membind="${FEMU_DEVICE_NODE}" \
    ./qemu-system-x86_64 \
    -name "FEMU-CSD-VM",debug-threads=on \
    -enable-kvm \
    -cpu host \
    -smp ${FEMU_SMP} \
    -m ${FEMU_MEM} \
    -object memory-backend-ram,id=guest-ram,size=${FEMU_MEM},host-nodes=${FEMU_HOST_NODE},policy=bind,prealloc=on \
    -numa node,memdev=guest-ram \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow2,id=hd0 \
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::${SSH_PORT}-:22 \
    -net nic,model=virtio \
    -virtfs local,path=/workspace/code/apps,mount_tag=apps,security_model=passthrough,multidevs=remap \
    -virtfs local,path=/workspace/data,mount_tag=data,security_model=passthrough,multidevs=remap \
    -virtfs local,path=/workspace/results,mount_tag=results,security_model=passthrough,multidevs=remap \
    -virtfs local,path=/workspace/scripts/guest,mount_tag=scripts,security_model=passthrough,multidevs=remap \
    -virtfs local,path=/workspace/tests/guest,mount_tag=tests,security_model=passthrough,multidevs=remap \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
