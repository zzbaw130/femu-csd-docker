/* PG12/x86-64 heap count: input pages and a small per-chunk output only. */
#ifndef PG_SINA_HEAP_COUNT_H
#define PG_SINA_HEAP_COUNT_H
#include <stdint.h>
#define PG_SINA_PAGE_BYTES 8192U
struct pg_sina_count {
    uint64_t rows, pages;
    uint32_t error_page, error_item;
};
#endif
