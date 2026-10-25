#include "postgres.h"
#include "access/htup_details.h"
#include "storage/bufpage.h"
#include "storage/itemid.h"
#include <stddef.h>
_Static_assert(PG_VERSION_NUM >= 120000 && PG_VERSION_NUM < 130000, "PG12 required");
_Static_assert(BLCKSZ == 8192, "8KiB pages required");
_Static_assert(sizeof(ItemIdData) == 4, "line pointer size");
_Static_assert(offsetof(PageHeaderData,pd_flags) == 10, "page flags");
_Static_assert(offsetof(PageHeaderData,pd_lower) == 12, "lower");
_Static_assert(offsetof(PageHeaderData,pd_upper) == 14, "upper");
_Static_assert(offsetof(PageHeaderData,pd_special) == 16, "special");
_Static_assert(offsetof(PageHeaderData,pd_pagesize_version) == 18, "page size/version");
_Static_assert(SizeOfPageHeaderData == 24, "page header");
_Static_assert(offsetof(HeapTupleHeaderData,t_infomask2) == 18, "natts");
_Static_assert(offsetof(HeapTupleHeaderData,t_infomask) == 20, "tuple flags");
_Static_assert(offsetof(HeapTupleHeaderData,t_hoff) == 22, "tuple header length");
_Static_assert(HEAP_XMIN_FROZEN == 0x300 && HEAP_XMAX_INVALID == 0x800, "visibility flags");
_Static_assert(PD_ALL_VISIBLE == 4 && LP_NORMAL == 1, "heap flags");
int main(void) { return 0; }
