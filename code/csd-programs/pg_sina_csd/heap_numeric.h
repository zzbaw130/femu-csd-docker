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
