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
#define FEMU_CACHE_RX_SLOTS 8
#define FEMU_CACHE_PAGE_BYTES 4096
#define FEMU_CACHE_RX_PAGES (FEMU_CACHE_BLOCK_BYTES/FEMU_CACHE_PAGE_BYTES)
/* ready is published last by the device, reset by the Guest after consumption. */
struct femu_cache_slot {
    fc_u64 ready, token, cookie, lba, bytes, load_ns, dma_start_ns, dma_ns;
    fc_u32 nsid, error;
};
struct femu_cache_rx {
    fc_u64 descriptor_dma;
    fc_u64 pages[FEMU_CACHE_RX_SLOTS][FEMU_CACHE_RX_PAGES];
};
/* Guest file mapping: physical byte offsets are namespace-relative. */
struct femu_cache_mapping { int fd; fc_u32 nsid; fc_u64 lba_bytes, file_offset, bytes; };
#define FEMU_CACHE_REGISTER _IOW(0xfc,1,struct femu_cache_mapping)
#define FEMU_CACHE_CLEAR _IO(0xfc,2)
/* SETUP allocates receive memory; BIND registers it to a graph session. */
#define FEMU_CACHE_RX_SETUP _IO(0xfc,3)
/* PUBLISH consumes the slot number given directly as the ioctl argument. */
#define FEMU_CACHE_RX_PUBLISH _IO(0xfc,4)
#define FEMU_CACHE_RX_BIND _IO(0xfc,5)
#define FEMU_CACHE_RX_UNBIND _IO(0xfc,6)
#endif
