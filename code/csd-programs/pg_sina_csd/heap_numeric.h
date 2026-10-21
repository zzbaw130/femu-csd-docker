/* PG numeric payload decoder shared by eager Scan and Filter late decoding. */
#ifndef PG_SINA_HEAP_NUMERIC_H
#define PG_SINA_HEAP_NUMERIC_H
#include <stdint.h>
#include <string.h>
#include <limits.h>
static inline uint16_t qs_numeric_read16(const unsigned char *p)
{ uint16_t v; memcpy(&v,p,2); return v; }
static inline int qs_numeric_cents(const unsigned char *p,unsigned size,int64_t *out,unsigned *display_scale)
{
    if(size<2) return -1;
    uint16_t head=qs_numeric_read16(p); int weight; unsigned header,negative,dscale;
    if((head&0xc000)==0xc000) return -1;
    if((head&0xc000)==0x8000) {
        header=2; negative=head&0x2000; weight=(head&0x3f)-((head&0x40)?64:0); dscale=(head&0x1f80)>>7;
    } else {
        if(size<4) return -1;
        header=4; negative=head&0x4000; weight=(int16_t)qs_numeric_read16(p+2); dscale=head&0x3fff;
    }
    if(dscale>2 || (size-header)%2) return -1;
    /* Common finite numerics occupy at most three base-10000 digits.
     * Their cents fit in int64 without the generic int128 scaling loop. */
    unsigned digits=(size-header)/2;
    if(!digits) { *out=0; *display_scale=dscale; return 0; }
    if((weight==0 && digits<=2) || (weight==-1 && digits==1)) {
        unsigned first=qs_numeric_read16(p+header),fraction=weight==-1?first:digits==2?qs_numeric_read16(p+header+2):0;
        if(first>=10000 || fraction>=10000 || fraction%100) return -1;
        int64_t cents=(weight==0?(int64_t)first*100:0)+fraction/100;
        *out=negative?-cents:cents; *display_scale=dscale; return 0;
    }
    if(weight==1 && digits<=3) {
        unsigned high=qs_numeric_read16(p+header),low=digits>1?qs_numeric_read16(p+header+2):0;
        unsigned fraction=digits>2?qs_numeric_read16(p+header+4):0;
        if(high>=10000 || low>=10000 || fraction>=10000 || fraction%100) return -1;
        int64_t cents=(int64_t)high*1000000+(int64_t)low*100+fraction/100;
        *out=negative?-cents:cents; *display_scale=dscale; return 0;
    }
    __int128 value=0;
    for(unsigned i=header;i<size;i+=2,--weight) {
        unsigned digit=qs_numeric_read16(p+i); if(digit>=10000) return -1;
        if(!digit) continue;
        __int128 part=digit;
        if(weight==-1) { if(digit%100) return -1; part/=100; }
        else if(weight>=0 && weight<=4) { part*=100; for(int j=0;j<weight;++j) part*=10000; }
        else return -1;
        value+=part;
    }
    if(negative) value=-value;
    if(value>INT64_MAX || value<INT64_MIN) return -1;
    *out=value; *display_scale=dscale; return 0;
}
#endif
