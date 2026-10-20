#include "femu-csd-kernel.h"
#include "heap_count.h"
#include <assert.h>
#include <string.h>
int64_t pg_sina_heap_count(struct femu_csd_args *args);
static void write16(unsigned char *p, uint16_t v) { memcpy(p, &v, sizeof(v)); }
static void write32(unsigned char *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
int main(void)
{
    unsigned char page[8192] = {0};
    struct pg_sina_count out;
    void *addresses[] = {page, &out};
    long long sizes[] = {sizeof(page), sizeof(out)};
    struct femu_csd_args args = {.numr=2, .mr_addr=addresses, .mr_len=sizes, .cparam1=1, .cparam2=8192};
    write16(page+10, 4); write16(page+12, 28); write16(page+14, 8168);
    write16(page+16, 8192); write16(page+18, 8196);
    write32(page+24, 8168U | (1U << 15) | (24U << 17));
    write16(page+8168+18, 1); write16(page+8168+20, 0xb00); page[8168+22]=24;
    assert(!pg_sina_heap_count(&args) && out.rows==1 && out.pages==1);
    write16(page+8168+20, 0x900); assert(pg_sina_heap_count(&args)==-3);
    write16(page+8168+20, 0xb00); write16(page+10, 0); assert(pg_sina_heap_count(&args)==-3);
    write16(page+10, 4); write16(page+12, 25); assert(pg_sina_heap_count(&args)==-2);
    write16(page+12, 28); write32(page+24, 8190U | (1U << 15) | (24U << 17));
    assert(pg_sina_heap_count(&args)==-4);
    write32(page+24, 0); assert(!pg_sina_heap_count(&args) && !out.rows);
    sizes[0]=8191; assert(pg_sina_heap_count(&args)==-1);
    return 0;
}
