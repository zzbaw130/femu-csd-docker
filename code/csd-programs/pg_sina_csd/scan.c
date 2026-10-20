/* Projection-aware PG12 heap scan, producing compact typed column batches. */
#include "femu-csd-kernel.h"
#include "query.h"
#include <string.h>
#include <limits.h>
static uint16_t read16(const unsigned char *p) { uint16_t v; memcpy(&v,p,2); return v; }
static uint32_t read32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
static int numeric_cents(const unsigned char *p,unsigned size,int64_t *out,unsigned *display_scale)
{
    if(size<2) return -1;
    uint16_t head=read16(p); int weight; unsigned header,negative,dscale;
    if((head&0xc000)==0xc000) return -1;
    if((head&0xc000)==0x8000) {
        header=2; negative=head&0x2000; weight=(head&0x3f)-((head&0x40)?64:0); dscale=(head&0x1f80)>>7;
    } else {
        if(size<4) return -1;
        header=4; negative=head&0x4000; weight=(int16_t)read16(p+2); dscale=head&0x3fff;
    }
    if(dscale>2 || (size-header)%2) return -1;
    __int128 value=0;
    for(unsigned i=header;i<size;i+=2,--weight) {
        unsigned digit=read16(p+i); if(digit>=10000) return -1;
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
int64_t pg_sina_scan(struct femu_csd_args *a)
{
    if(!a || a->numr!=4 || a->mr_len[1]<(long long)sizeof(struct qs_plan) || a->mr_len[3]<(long long)sizeof(struct qs_task)) return -1;
    const struct qs_plan *p=a->mr_addr[1]; struct qs_chunk *c=a->mr_addr[2];
    unsigned bytes=((struct qs_task *)a->mr_addr[3])->input_bytes;
    if(p->nattrs>QS_COLS || p->ncols>p->nattrs || a->mr_len[2]<(long long)sizeof(*c) ||
        c->ncols!=p->ncols || qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[2] ||
        bytes%QS_PAGE || bytes>(uint64_t)a->mr_len[0]) return -1;
    c->rows=c->selected=0; memset(qs_nulls(c),0,((c->capacity+7)/8)*(size_t)c->ncols);
    int last=-1; for(unsigned i=0;i<p->nattrs;++i) if(p->cols[i].slot>=0) last=i;
    for(unsigned page=0;page<bytes;page+=QS_PAGE) {
        const unsigned char *raw=(unsigned char *)a->mr_addr[0]+page;
        unsigned lower=read16(raw+12),upper=read16(raw+14);
        if(lower<24 || (lower-24)%4 || lower>upper || upper>QS_PAGE ||
            read16(raw+16)!=QS_PAGE || read16(raw+18)!=(QS_PAGE|4) || !(read16(raw+10)&4)) return -2;
        for(unsigned i=0;i<(lower-24)/4;++i) {
            uint32_t lp=read32(raw+24+4*i); unsigned offset=lp&0x7fff,flags=(lp>>15)&3,len=lp>>17;
            if(flags!=1) continue;
            if(offset<upper || offset>QS_PAGE || len<23 || len>QS_PAGE-offset || c->rows>=c->capacity) return -2;
            const unsigned char *tuple=raw+offset; unsigned mask=read16(tuple+20),off=tuple[22];
            if((read16(tuple+18)&0x7ff)!=p->nattrs || off>len || off%8 ||
                off<23+((mask&1)?(p->nattrs+7)/8:0) || (mask&8) || (mask&0x300)!=0x300 || !(mask&0x800)) return -2;
            for(int col=0;col<=last;++col) {
                const struct qs_column *spec=&p->cols[col]; int slot=spec->slot;
                if((mask&1) && !(tuple[23+col/8]&(1U<<(col%8)))) {
                    if(slot>=0) { qs_nulls(c)[(size_t)slot*((c->capacity+7)/8)+c->rows/8]|=1U<<(c->rows%8); qs_values(c)[(size_t)slot*c->capacity+c->rows]=0; qs_dscales(c)[(size_t)slot*c->capacity+c->rows]=0; }
                    continue;
                }
                if(off>=len) return -2;
                unsigned size,header=0; int compressed=0;
                if(spec->length==-1) {
                    if(!tuple[off]) off=(off+spec->align-1)&~(spec->align-1U);
                    if(off>=len) return -2;
                    if(tuple[off]==1) { size=18; header=2; compressed=1; }
                    else if(tuple[off]&1) { size=tuple[off]>>1; header=1; }
                    else { if(len-off<4) return -2; size=read32(tuple+off)>>2; header=4; compressed=read32(tuple+off)&3; }
                } else if(spec->length>0) { off=(off+spec->align-1)&~(spec->align-1U); size=spec->length; }
                else return -2;
                if(size<header || off>len || size>len-off) return -2;
                if(slot>=0) {
                    int64_t value=0; unsigned dscale=0; const unsigned char *v=tuple+off+header;
                    if(compressed) return -3;
                    if(spec->type==Q_DECIMAL) { if(numeric_cents(v,size-header,&value,&dscale)) return -3; }
                    else if(spec->type==Q_CHAR) { if(size-header!=1 || *v>=128) return -3; value=*v; }
                    else if(size==8) memcpy(&value,v,8);
                    else if(size==4) value=(int32_t)read32(v);
                    else if(size==2) value=(int16_t)read16(v);
                    else if(size==1) value=*v;
                    else return -3;
                    qs_values(c)[(size_t)slot*c->capacity+c->rows]=value;
                    qs_dscales(c)[(size_t)slot*c->capacity+c->rows]=dscale;
                }
                off+=size;
            }
            qs_selection(c)[c->rows]=c->rows; ++c->rows;
        }
    }
    c->selected=c->rows; return 0;
}
