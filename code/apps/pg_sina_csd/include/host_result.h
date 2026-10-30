/* Private CSD -> PG boundary. Typed columns share the existing result AFDM;
 * variable values carry PG varlena bytes, never device pointers. */
#ifndef PG_SINA_HOST_RESULT_H
#define PG_SINA_HOST_RESULT_H
#include "query.h"
#define QP_TYPED_RESULT 2U
/* An independent parallel Scan lets each CU encode its own HOST batch. */
#define QP_DIRECT_RESULT 3U
static inline unsigned qp_host_width(unsigned oid)
{
    switch(oid) {
    case 16:return 1; case 21:return 2;
    case 20:case 701:return 8;
    default:return 4;
    }
}
static inline int qp_host_variable(unsigned oid)
{ return oid==1700 || oid==25 || oid==1043 || oid==1042; }
static inline unsigned qp_host_offset(const struct qs_plan *p,unsigned rows,unsigned col)
{
    unsigned at=0;
    for(unsigned i=0;i<col;++i) at=(at+rows*qp_host_width(p->outputs[i].type_oid)+(rows+7)/8+3)&~3U;
    return at;
}
static inline unsigned qp_host_bytes(const struct qs_plan *p,unsigned rows)
{ return qp_host_offset(p,rows,p->nout); }
/* Direct CU packets place variable bytes immediately after typed columns,
 * so a single AFDM read suffices. The packed payload can overlap str_bytes;
 * its authoritative length is carried in the compute completion. */
static inline void qp_host_pack(struct qs_result *r,const struct qs_plan *p,unsigned bytes)
{ memmove((unsigned char *)r+offsetof(struct qs_result,data)+qp_host_bytes(p,r->rows),r->str_data,bytes); }
static inline void qp_host_unpack(struct qs_result *r,const struct qs_plan *p,unsigned bytes)
{
    memmove(r->str_data,(unsigned char *)r+offsetof(struct qs_result,data)+qp_host_bytes(p,r->rows),bytes);
    r->str_bytes=bytes;
}
static inline unsigned qp_host_capacity(const struct qs_plan *p)
{
    unsigned low=0,high=sizeof(((struct qs_result *)0)->data);
    while(low<high) { unsigned mid=low+(high-low+1)/2;
        if(qp_host_bytes(p,mid)<=sizeof(((struct qs_result *)0)->data)) low=mid; else high=mid-1;
    }
    return low;
}
static inline unsigned char *qp_host_values(struct qs_result *r,const struct qs_plan *p,unsigned rows,unsigned col)
{ return (unsigned char *)r->data+qp_host_offset(p,rows,col); }
static inline unsigned char *qp_host_nulls(struct qs_result *r,const struct qs_plan *p,unsigned rows,unsigned col)
{ return qp_host_values(r,p,rows,col)+rows*qp_host_width(p->outputs[col].type_oid); }
/* NumericLong: varlena, sign/display scale, weight, base-10000 int16 digits.
 * Used only for calculated values; plain Scan projections preserve Heap's
 * original Numeric body, including its short/long numeric header. */
static inline unsigned qp_host_numeric(void *to,struct qs_cell v)
{
    uint16_t digits[12]; unsigned count=0,first=0,scale=v.scale;
    unsigned display=v.dscale<scale?v.dscale:scale;
    unsigned __int128 magnitude=v.value<0?0-(unsigned __int128)v.value:(unsigned __int128)v.value;
    while(scale>display) { magnitude/=10; --scale; }
    do { digits[count++]=magnitude%10000; magnitude/=10000; } while(magnitude);
    unsigned padding=(4-scale%4)%4,factor=1,carry=0;
    while(padding--) factor*=10;
    for(unsigned i=0;i<count;++i) { unsigned n=digits[i]*factor+carry; digits[i]=n%10000; carry=n/10000; }
    if(carry) digits[count++]=carry;
    int16_t weight=(int)count-(scale+3)/4-1;
    while(first<count && !digits[first]) ++first;
    if(first==count) weight=0;
    unsigned n=count-first,bytes=8+2*n; uint32_t header=bytes<<2;
    uint16_t sign=display | (v.value<0 && n?0x4000:0);
    memcpy(to,&header,4); memcpy((char *)to+4,&sign,2); memcpy((char *)to+6,&weight,2);
    for(unsigned i=0;i<n;++i) memcpy((char *)to+8+2*i,&digits[count-1-i],2);
    return bytes;
}
static inline void qp_host_compact(struct qs_result *r,const struct qs_plan *p,unsigned capacity)
{
    for(unsigned col=0;col<p->nout;++col) {
        unsigned width=qp_host_width(p->outputs[col].type_oid);
        memmove(qp_host_values(r,p,r->rows,col),qp_host_values(r,p,capacity,col),(size_t)r->rows*width);
        memmove(qp_host_nulls(r,p,r->rows,col),qp_host_nulls(r,p,capacity,col),(r->rows+7)/8);
    }
}
static inline int qp_host_isnull(struct qs_result *r,const struct qs_plan *p,unsigned row,unsigned col)
{ return (qp_host_nulls(r,p,r->rows,col)[row/8]>>(row%8))&1; }
static inline const void *qp_host_var(struct qs_result *r,const void *field,unsigned *bytes)
{
    uint32_t offset,length; memcpy(&offset,field,4);
    if(offset%4 || offset>r->str_bytes || r->str_bytes-offset<4) return NULL;
    memcpy(&length,r->str_data+offset,4); length>>=2;
    if(length<4 || length>r->str_bytes-offset) return NULL;
    *bytes=length; return r->str_data+offset;
}
/* A PG consumer retains its claimed rows while SNIA reuses the shared batch.
 * Copy only selected variable values, with offsets rebased to that slice. */
static inline int qp_host_slice(struct qs_result *to,struct qs_result *from,const struct qs_plan *p,unsigned start,unsigned rows)
{
    to->rows=rows; to->str_bytes=0;
    for(unsigned col=0;col<p->nout;++col) {
        unsigned width=qp_host_width(p->outputs[col].type_oid);
        unsigned char *values=qp_host_values(to,p,rows,col),*nulls=qp_host_nulls(to,p,rows,col);
        memcpy(values,qp_host_values(from,p,from->rows,col)+(size_t)start*width,(size_t)rows*width);
        const unsigned char *source_nulls=qp_host_nulls(from,p,from->rows,col);
        unsigned first=start/8,shift=start%8,source_bytes=(from->rows+7)/8,bytes=(rows+7)/8;
        for(unsigned b=0;b<bytes;++b) {
            unsigned bits=source_nulls[first+b]>>shift;
            if(shift && first+b+1<source_bytes) bits|=(unsigned)source_nulls[first+b+1]<<(8-shift);
            nulls[b]=bits;
        }
        if(rows%8) nulls[bytes-1]&=(1U<<(rows%8))-1;
        if(!qp_host_variable(p->outputs[col].type_oid)) continue;
        for(unsigned row=0;row<rows;++row) {
            if(!((nulls[row/8]>>(row%8))&1)) {
                unsigned bytes; const void *value=qp_host_var(from,values+(size_t)row*width,&bytes);
                unsigned offset=(to->str_bytes+3)&~3U;
                if(!value || offset>sizeof(to->str_data) || bytes>sizeof(to->str_data)-offset) return -1;
                memcpy(to->str_data+offset,value,bytes); memcpy(values+(size_t)row*width,&offset,4);
                to->str_bytes=offset+bytes;
            }
        }
    }
    return 0;
}
#endif
