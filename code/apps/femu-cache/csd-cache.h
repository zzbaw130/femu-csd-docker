/* FEMU private receive ring. Wire fields are little endian; not SNIA API. */
#ifndef FEMU_CSD_CACHE_H
#define FEMU_CSD_CACHE_H
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
typedef u64 fc_u64;
typedef u32 fc_u32;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint64_t fc_u64;
typedef uint32_t fc_u32;
#endif
#define FEMU_CACHE_CONTROL 0xeb /* host-to-device registration payload */
#define FEMU_CACHE_RX_REGISTER 1
#define FEMU_CACHE_RX_STOP 2
#define FEMU_CACHE_BLOCK_BYTES (4U*1024*1024)
#define FEMU_CACHE_LARGE_SLOTS 8
#define FEMU_CACHE_RX_SLOTS (FEMU_CACHE_LARGE_SLOTS+32)
#define FEMU_CACHE_PAGE_BYTES 4096
#define FEMU_CACHE_RX_PAGES (FEMU_CACHE_BLOCK_BYTES/FEMU_CACHE_PAGE_BYTES)
/* Keep 4MiB LOADs intact; small internal reads have independent 64KiB slots. */
#define FEMU_CACHE_SLOT_BYTES(i) ((i)<FEMU_CACHE_LARGE_SLOTS?FEMU_CACHE_BLOCK_BYTES:65536U)
#define FEMU_CACHE_SLOT_OFFSET(i) ((i)<FEMU_CACHE_LARGE_SLOTS?(i)*FEMU_CACHE_BLOCK_BYTES:FEMU_CACHE_LARGE_SLOTS*FEMU_CACHE_BLOCK_BYTES+((i)-FEMU_CACHE_LARGE_SLOTS)*65536U)
#define FEMU_CACHE_DATA_BYTES FEMU_CACHE_SLOT_OFFSET(FEMU_CACHE_RX_SLOTS)
#define FEMU_CACHE_TOTAL_PAGES (FEMU_CACHE_DATA_BYTES/FEMU_CACHE_PAGE_BYTES)
/* ready is published last by the device, reset by the Guest after consumption. */
struct femu_cache_slot {
    fc_u64 ready, token, cookie, lba, bytes, load_ns, dma_start_ns, dma_ns;
    fc_u32 nsid, error;
};
struct femu_cache_rx {
    fc_u64 descriptor_dma;
    fc_u64 resident_dma, resident_bytes;
    fc_u64 pages[FEMU_CACHE_TOTAL_PAGES];
};
/* Query totals written by STOP, not per-block diagnostic logging. */
struct femu_cache_stats {
    fc_u64 candidate_bytes,resident_bytes,dma_bytes,queue_dropped_bytes;
    fc_u64 credit_dropped_bytes,overwrite_dropped_bytes,error_dropped_bytes,stop_dropped_bytes;
};
#define FEMU_CACHE_STATS_OFFSET (FEMU_CACHE_RX_SLOTS*sizeof(struct femu_cache_slot))
/* Guest file mapping: physical byte offsets are namespace-relative. */
struct femu_cache_mapping {
    int fd; fc_u32 nsid; fc_u64 lba_bytes, file_offset, bytes;
    fc_u64 resident_bits; /* userspace pointer, one bit per 4KiB in this extent */
    fc_u32 flags;
};
#define FEMU_CACHE_NEEDS_DCP 1U
#define FEMU_CACHE_REGISTER _IOW(0xfc,1,struct femu_cache_mapping)
#define FEMU_CACHE_CLEAR _IO(0xfc,2)
/* SETUP allocates receive memory; BIND registers it to a graph session. */
#define FEMU_CACHE_RX_SETUP _IO(0xfc,3)
/* PUBLISH consumes the slot number given directly as the ioctl argument. */
#define FEMU_CACHE_RX_PUBLISH _IO(0xfc,4)
#define FEMU_CACHE_RX_BIND _IO(0xfc,5)
#define FEMU_CACHE_RX_UNBIND _IO(0xfc,6)
/* External database consumer: mmap data at this offset, then ACQUIRE/RELEASE
 * one complete slot. RELEASE may also publish the same bytes to file cache. */
#define FEMU_CACHE_DATA_OFFSET FEMU_CACHE_PAGE_BYTES
#define FEMU_CACHE_RX_ACQUIRE _IO(0xfc,7)
#define FEMU_CACHE_RX_RELEASE _IO(0xfc,8)
/* Read-only Guest admission check, no receiver allocation or device I/O. */
#define FEMU_CACHE_RX_ADMISSION _IO(0xfc,9)
#define FEMU_CACHE_RELEASE_HPCP 0x100U
#define FEMU_CACHE_RELEASE_DCP 0x200U /* all pages met this slot's DCP target */
/* Successful publication, but no more optional file-cache allocation. */
#define FEMU_CACHE_HPCP_FULL 1
/* FEMU csConfig(CS_VENDOR_SPECIFIC) input: borrow this prepared receiver.
 * The adapter duplicates the fd; the caller consumes its data, not a thread. */
/* receive_fd=-1 pauses population for this execution; reset restores it. */
/* After releasing its last DCP slot, the backend may return consumption to
 * the existing HPCP worker, using the same fd/ring and device registration. */
#define FEMU_CACHE_HPCP_WORKER (-2)
struct femu_cache_config { int receive_fd; };
#endif
