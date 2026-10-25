/* PG12 on-disk fields follow bufpage.h, itemid.h and htup_details.h.
 * No SQL planning or I/O here: the Guest schedules LOAD -> this CSF. */
#include "femu-csd-kernel.h"
#include "heap_count.h"
#include <string.h>
static uint16_t read16(const unsigned char *p)
{
    uint16_t value; memcpy(&value, p, sizeof(value)); return value;
}
static uint32_t read32(const unsigned char *p)
{
    uint32_t value; memcpy(&value, p, sizeof(value)); return value;
}
/* MR[0]: whole heap pages; MR[1]: pg_sina_count; cparam1: physical nattrs; cparam2: valid input bytes. */
int64_t pg_sina_heap_count(struct femu_csd_args *args)
{
    if (!args || args->numr != 2 || !args->mr_addr[0] || !args->mr_addr[1] ||
        args->cparam2 <= 0 || args->cparam2 > args->mr_len[0] || args->cparam2 % PG_SINA_PAGE_BYTES ||
        args->mr_len[1] < (long long)sizeof(struct pg_sina_count) ||
        args->cparam1 <= 0 || args->cparam1 > 1600) return -1;
    struct pg_sina_count *out = args->mr_addr[1];
    memset(out, 0, sizeof(*out));
    for (uint64_t p = 0; p < (uint64_t)args->cparam2 / PG_SINA_PAGE_BYTES; ++p) {
        const unsigned char *page = (const unsigned char *)args->mr_addr[0] + p * PG_SINA_PAGE_BYTES;
        unsigned lower = read16(page + 12), upper = read16(page + 14);
        out->error_page = p; out->error_item = 0;
        if (read16(page + 18) != (PG_SINA_PAGE_BYTES | 4) || lower < 24 ||
            (lower - 24) % 4 || lower > upper || upper > PG_SINA_PAGE_BYTES ||
            read16(page + 16) != PG_SINA_PAGE_BYTES) return -2;
        if (!(read16(page + 10) & 4)) return -3; /* PD_ALL_VISIBLE */
        for (unsigned i = 0; i < (lower - 24) / 4; ++i) {
            uint32_t item = read32(page + 24 + i * 4);
            unsigned offset = item & 0x7fff, flags = (item >> 15) & 3, bytes = item >> 17;
            out->error_item = i + 1;
            if (flags != 1) continue; /* LP_UNUSED/REDIRECT/DEAD */
            if (offset < upper || bytes < 23 || offset > PG_SINA_PAGE_BYTES ||
                bytes > PG_SINA_PAGE_BYTES - offset) return -4;
            const unsigned char *tuple = page + offset;
            unsigned mask = read16(tuple + 20), header = tuple[22];
            if ((read16(tuple + 18) & 0x7ff) != args->cparam1 || header > bytes ||
                header < 23 + ((mask & 1) ? ((unsigned)args->cparam1 + 7) / 8 : 0) ||
                header % 8 || (mask & 8)) return -4;
            if ((mask & 0x300) != 0x300 || !(mask & 0x800)) return -3;
            ++out->rows;
        }
        ++out->pages;
    }
    return 0;
}
