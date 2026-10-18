/* Optional CSF-side argument adapter over FEMU's unchanged native E1 ABI.
 * This payload is a convention between the API library and the CSF, not firmware. */
#ifndef SINA_CS_KERNEL_H
#define SINA_CS_KERNEL_H
#include <stdint.h>
#include <stddef.h>
#include <endian.h>
#define CS_API_MAX_ARGS 64
#define CS_KERNEL_MAGIC UINT32_C(0x43534131)
struct femu_csd_args {
    int numr;
    void **mr_addr;
    long long *mr_len;
    long long cparam1, cparam2;
    void *data_buffer;
    long long buffer_len;
} __attribute__((packed));
struct cs_kernel_header {
    uint32_t magic, version, nargs, reserved;
} __attribute__((packed));
struct cs_kernel_wire_arg {
    uint32_t type, range;
    uint64_t offset, value_lo, value_hi;
} __attribute__((packed));
struct cs_kernel_arg {
    uint32_t type;
    void *address;
    uint64_t bytes, value_lo, value_hi;
};
/* Local decoded pointers never cross NVMe. The CSF uses stack/private memory. */
static inline int cs_kernel_decode(const struct femu_csd_args *native,
                                   struct cs_kernel_arg out[CS_API_MAX_ARGS])
{
    if (!native || native->numr <= 0 || !native->mr_addr || !native->mr_len)
        return -1;
    int parameter_range = native->numr - 1;
    if (!native->mr_addr[parameter_range] ||
        native->mr_len[parameter_range] < (long long)sizeof(struct cs_kernel_header))
        return -1;
    const struct cs_kernel_header *h =
        (const struct cs_kernel_header *)native->mr_addr[parameter_range];
    uint32_t n = le32toh(h->nargs);
    if (le32toh(h->magic) != CS_KERNEL_MAGIC || le32toh(h->version) != 1 || n > CS_API_MAX_ARGS ||
        (uint64_t)native->mr_len[parameter_range] != sizeof(*h) + n * sizeof(struct cs_kernel_wire_arg))
        return -1;
    const struct cs_kernel_wire_arg *wire = (const struct cs_kernel_wire_arg *)(h + 1);
    for (uint32_t i = 0; i < n; i++) {
        struct cs_kernel_arg *a = &out[i];
        a->type = le32toh(wire[i].type);
        a->address = NULL;
        a->bytes = 0;
        a->value_lo = le64toh(wire[i].value_lo);
        a->value_hi = le64toh(wire[i].value_hi);
        if (a->type == 1) {
            uint32_t r = le32toh(wire[i].range);
            uint64_t off = le64toh(wire[i].offset);
            if (native->numr <= 0 || r >= (uint32_t)parameter_range || !native->mr_addr ||
                !native->mr_len || native->mr_len[r] < 0 || off > (uint64_t)native->mr_len[r])
                return -1;
            a->address = (uint8_t *)native->mr_addr[r] + off;
            a->bytes = native->mr_len[r] - off;
        } else if (a->type < 2 || a->type > 6)
            return -1;
    }
    return (int)n;
}
#endif
