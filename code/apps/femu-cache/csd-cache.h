/* FEMU cache refill extension; generic namespace/FDM ranges, no SQL types. */
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
#define FEMU_CACHE_CONTROL 0xe8
#define FEMU_CACHE_READ 0xf3
#define FEMU_CACHE_READ_PROFILE 1U /* F3 CDW14: diagnostic timestamps only */
#define FEMU_CACHE_LOG 0xc8
#define FEMU_CACHE_LOG_BYTES 4096
#define FEMU_CACHE_BLOCK_BYTES (4U*1024*1024)
#define FEMU_CACHE_LOG_COUNT 32
#define FEMU_CACHE_ENABLE 1
#define FEMU_CACHE_DISABLE 2
#define FEMU_CACHE_ACK 3
#define FEMU_CACHE_FETCH 4
#define FEMU_CACHE_AER (7U | (0xc8U<<8) | (FEMU_CACHE_LOG<<16))
struct femu_cache_event {
    fc_u64 token, cookie, offset, lba, bytes, load_ns;
    fc_u32 afdm, nsid;
    fc_u64 reserved;
};
struct femu_cache_log {
    fc_u32 count, dropped;
    struct femu_cache_event events[FEMU_CACHE_LOG_COUNT];
};
/* Guest module registration: physical byte offsets are namespace-relative. */
struct femu_cache_mapping { int fd; fc_u32 nsid; fc_u64 lba_bytes, file_offset, bytes; };
#define FEMU_CACHE_REGISTER _IOW(0xfc,1,struct femu_cache_mapping)
#define FEMU_CACHE_CLEAR _IO(0xfc,2)
#endif
