/* Native test CSF: descriptor overlap, two successive internal read requests,
 * and malformed ranges. It has no PostgreSQL or file-system dependencies. */
#include "cs_csf.h"
#include <endian.h>
int64_t cs_test_nvm_read(struct femu_csd_args *a)
{
    if(a->numr!=2 || a->mr_len[1]<2*(long long)sizeof(struct femu_graph_read)) return -1;
    struct femu_graph_read *r=a->mr_addr[1];
    if(!csIsReentry(a)) {
        r[0]=(struct femu_graph_read){.nsid=htole32(1),.blocks=htole32(1),.afdm_index=htole32(1),.lba=htole64(2)};
        r[1]=(struct femu_graph_read){.nsid=htole32(1),.blocks=htole32(1),.lba=htole64(3),.offset=htole64(512)};
        switch(a->cparam1) {
        case 1:r[1].afdm_index=htole32(2);break;
        case 2:r[1].offset=htole64(a->mr_len[0]);break;
        case 3:r[1].lba=htole64(UINT64_MAX);break;
        case 4:r[1].blocks=0;break;
        case 5:r[1].nsid=htole32(2);break;
        case 6:r[1].reserved=htole32(1);break;
        }
        return csNeedNvmRead(2);
    }
    if(csReentryValue(a)==2) {
        r[0]=(struct femu_graph_read){.nsid=htole32(1),.blocks=htole32(1),.lba=htole64(4)};
        return csNeedNvmRead(1);
    }
    return csReentryValue(a)==1?0xbeef:-1;
}
