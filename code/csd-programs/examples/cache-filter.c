/* Read-only input: reproduce Scan/Filter consumption without modifying FDM. */
#include "femu-csd-kernel.h"
#include <stdint.h>
int64_t cache_filter(struct femu_csd_args *a)
{
    if(a->numr!=2 || a->mr_len[1]<16 || a->mr_len[0]%4 || a->cparam1<1) return -1;
    const uint32_t *input=a->mr_addr[0]; uint64_t sum=0,count=0;
    /* Repetitions select light/heavy consumers for the overlap experiment. */
    for(uint64_t pass=0;pass<(uint64_t)a->cparam1;pass++) {
        sum=0; count=0;
        for(uint64_t i=0;i<(uint64_t)a->mr_len[0]/4;i++)
            if(input[i]&1) { sum+=input[i]; count++; }
        __asm__ volatile("" : "+r"(sum), "+r"(count) : : "memory");
    }
    uint64_t *out=a->mr_addr[1]; out[0]=sum; out[1]=count;
    return 0;
}
