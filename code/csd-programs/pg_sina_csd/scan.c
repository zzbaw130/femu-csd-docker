/* Projection-aware PG12 heap scan, producing compact typed column batches. */
#include "femu-csd-kernel.h"
#include "query.h"
#include <stdbool.h>
#include <string.h>
#include "heap_numeric.h"
#include "scan.h"
static uint16_t read16(const unsigned char *p) { uint16_t v; memcpy(&v,p,2); return v; }
static uint32_t read32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
/* Small '%' glob matcher for scan-level LIKE; '_' and escapes stay native. */
static int glob_match(const char *pattern,unsigned plen,const char *text,unsigned tlen)
{
    if(!plen) return !tlen;
    if(pattern[0]=='%') {
        while(plen && pattern[0]=='%') { ++pattern; --plen; }
        if(!plen) return 1;
        for(unsigned i=0;i<=tlen;++i) if(glob_match(pattern,plen,text+i,tlen-i)) return 1;
        return 0;
    }
    return tlen && pattern[0]==text[0] && glob_match(pattern+1,plen-1,text+1,tlen-1);
}
/* A batch carries independent offsets: variable-width fields depend on the
 * preceding field of the same tuple, never on another tuple. Work by column
 * so conversions and destination writes use one type and contiguous slots. */
struct heap_tuple {
    const unsigned char *data;
    unsigned bytes,absolute,row,mask,off,first;
};
struct heap_prefix {
    unsigned attrs,bytes,count;
    bool borrowed_tail;
    struct { unsigned offset,size,slot; } projected[QS_COLS];
};
struct qs_scan_cursor {
    struct qs_dictionary *dictionary;
    struct qs_dictionary_access *access;
    struct heap_tuple tuples[64];
    unsigned active,page,line,count,column,tuple,decoding;
    void *io;
    uint64_t completion;
};

static inline int heap_attribute(const struct qs_column *spec,
    const unsigned char *tuple, unsigned len, unsigned *offset,
    unsigned *size, unsigned *header, int *compressed)
{
    unsigned off=*offset;
    *header=0;
    *compressed=0;
    if(off>=len) return -2;
    if(spec->length>0) {
        off=(off+spec->align-1)&~(spec->align-1U);
        *size=spec->length;
    } else if(spec->length==-1) {
        if(!tuple[off]) off=(off+spec->align-1)&~(spec->align-1U);
        if(off>=len) return -2;
        unsigned lead=tuple[off];
        if(lead&1) {
            if(lead==1) { *size=18; *header=2; *compressed=1; }
            else { *size=lead>>1; *header=1; }
        } else {
            if(len-off<4) return -2;
            uint32_t tag=read32(tuple+off);
            *size=tag>>2; *header=4; *compressed=tag&3;
        }
    } else return -2;
    if(*size<*header || off>len || *size>len-off) return -2;
    *offset=off;
    return 0;
}

struct qs_scan_cursor *qs_scan_cursor_create(cs_work_context *context,struct qs_dictionary *dictionary)
{
    struct qs_scan_cursor *cursor=cs_work_calloc(context,1,sizeof(*cursor));
    if(!cursor) return NULL;
    cursor->dictionary=dictionary;
    cursor->access=qs_dictionary_access_create(dictionary);
    if(!cursor->access) { cs_work_free(context,cursor); return NULL; }
    return cursor;
}
static inline int64_t fixed_number(const unsigned char *v,unsigned size)
{
    if(size==8) { int64_t value; memcpy(&value,v,8); return value; }
    if(size==4) return (int32_t)read32(v);
    if(size==2) return (int16_t)read16(v);
    return *v;
}
/* With no NULL bitmap or paging, each tuple's offsets can stay local while
 * walking its variable-width headers. Values still go into column vectors. */
static int scan_borrowed_tail(const struct qs_plan *p,struct heap_tuple *tuples,
    unsigned count,struct qs_chunk *c,unsigned first,int last)
{
    int64_t *values[QS_COLS];
    uint8_t *scales[QS_COLS];
    unsigned short_run[QS_COLS]={0};
    unsigned run=0;
    for(int col=last;col>=(int)first;--col) {
        const struct qs_column *spec=&p->cols[col];
        run=spec->slot<0 && spec->length==-1?run+1:0;
        short_run[col]=run;
    }
    for(unsigned col=first;(int)col<=last;++col) {
        int slot=p->cols[col].slot;
        if(slot<0) continue;
        size_t at=(size_t)slot*c->capacity;
        values[col]=qs_values(c)+at;
        scales[col]=qs_dscales(c)+at;
    }
    for(unsigned i=0;i<count;++i) {
        const struct heap_tuple *t=&tuples[i];
        unsigned off=t->off;
        for(unsigned col=first;(int)col<=last;++col) {
            const struct qs_column *spec=&p->cols[col];
            unsigned size,header; int compressed;
            if(short_run[col]) {
                unsigned end=col+short_run[col];
                /* An inline short varlena carries its total size in the
                 * first byte and needs no alignment or payload decoding. */
                for(;col<end;++col) {
                    if(off>=t->bytes) return -2;
                    unsigned tag=t->data[off];
                    if((tag&1) && tag!=1) size=tag>>1;
                    else if(heap_attribute(&p->cols[col],t->data,t->bytes,&off,&size,&header,&compressed))
                        return -2;
                    if(size>t->bytes-off) return -2;
                    off+=size;
                }
                --col;
                continue;
            }
            if(heap_attribute(spec,t->data,t->bytes,&off,&size,&header,&compressed)) return -2;
            if(spec->slot>=0) {
                if(compressed) return -3;
                if(spec->deferred) {
                    struct qs_heap_ref ref={t->absolute+off+header,size-header};
                    memcpy(&values[col][t->row],&ref,sizeof(ref));
                } else values[col][t->row]=fixed_number(t->data+off,size);
                scales[col][t->row]=0;
            }
            off+=size;
        }
    }
    return 0;
}
static int64_t scan_tuples(const struct qs_plan *p,struct heap_tuple *tuples,unsigned count,struct qs_chunk *c,struct qs_dict *dict,int last,const struct heap_prefix *prefix,struct qs_scan_cursor *cursor)
{
    unsigned start=prefix?prefix->attrs:0;
    bool no_nulls=true;
    for(unsigned i=0;(!cursor || !cursor->decoding) && i<count;++i) {
        struct heap_tuple *t=&tuples[i]; const unsigned char *tuple=t->data;
        unsigned mask=read16(tuple+20),off=tuple[22];
        if((read16(tuple+18)&0x7ff)!=p->nattrs || off>t->bytes || off%8 ||
            off<23+((mask&1)?(p->nattrs+7)/8:0) || (mask&8) || (mask&0x300)!=0x300 || !(mask&0x800)) return -2;
        t->mask=mask; t->first=(mask&1) || !prefix?0:prefix->attrs;
        t->off=off+(t->first?prefix->bytes:0);
        if(t->off>t->bytes) return -2;
        if(mask&1) { start=0; no_nulls=false; }
        if(t->first) for(unsigned j=0;j<prefix->count;++j) {
            unsigned at=prefix->projected[j].slot*c->capacity+t->row;
            qs_values(c)[at]=fixed_number(tuple+off+prefix->projected[j].offset,prefix->projected[j].size);
            qs_dscales(c)[at]=0;
        }
    }
    if(!cursor && no_nulls && prefix && prefix->borrowed_tail)
        return scan_borrowed_tail(p,tuples,count,c,start,last);
    for(int col=cursor && cursor->decoding?(int)cursor->column:(int)start;col<=last;++col) {
        const struct qs_column *spec=&p->cols[col]; int slot=spec->slot;
        if(slot<0) {
            int end=col+1;
            while(end<=last && p->cols[end].slot<0) ++end;
            /* Skip adjacent unprojected attributes while the tuple header
             * and offset are local. Only projected columns need a batch. */
            for(unsigned i=0;i<count;++i) {
                struct heap_tuple *t=&tuples[i];
                const unsigned char *tuple=t->data;
                unsigned off=t->off;
                for(int at=col>(int)t->first?col:(int)t->first;at<end;++at) {
                    if((t->mask&1) && !(tuple[23+at/8]&(1U<<(at%8)))) continue;
                    unsigned size,header; int compressed;
                    if(heap_attribute(&p->cols[at],tuple,t->bytes,&off,&size,&header,&compressed)) return -2;
                    off+=size;
                }
                t->off=off;
            }
            col=end-1;
            continue;
        }
        int64_t *values=qs_values(c)+(size_t)slot*c->capacity;
        uint8_t *scales=qs_dscales(c)+(size_t)slot*c->capacity;
        uint8_t *nulls=qs_nulls(c)+(size_t)slot*((c->capacity+7)/8);
        unsigned begin=cursor && cursor->decoding && (unsigned)col==cursor->column?cursor->tuple:0;
        for(unsigned i=begin;i<count;++i) {
            struct heap_tuple *t=&tuples[i];
            if((unsigned)col<t->first) continue;
            const unsigned char *tuple=t->data;
            unsigned mask=t->mask,off=t->off,len=t->bytes,row=t->row;
            if((mask&1) && !(tuple[23+col/8]&(1U<<(col%8)))) {
                nulls[row/8]|=1U<<(row%8); values[row]=0; scales[row]=0;
                continue;
            }
            unsigned size,header; int compressed;
            if(heap_attribute(spec,tuple,len,&off,&size,&header,&compressed)) return -2;
            {
                int64_t value=0; unsigned dscale=0; const unsigned char *v=tuple+off+header;
                if(compressed) return -3;
                if(spec->deferred) {
                    /* Reuse this value slot for the raw payload location.
                     * Filter replaces it with a number only for survivors. */
                    struct qs_heap_ref ref={t->absolute+off+header,size-header};
                    memcpy(&value,&ref,sizeof(ref));
                }
                else if(spec->type==Q_STRING) {
                    if(!dict && !cursor) return -3;
                    unsigned length=size-header;
                    if(spec->trim_spaces) while(length && v[length-1]==' ') --length;
                    if(cursor) {
                        uint32_t id=0;
                        int64_t status=qs_dictionary_insert(cursor->dictionary,cursor->access,
                            (const char *)v,length,&id,cursor->io,cursor->completion);
                        cursor->completion=0;
                        if(status) {
                            cursor->column=col; cursor->tuple=i; cursor->decoding=1;
                            return status;
                        }
                        value=id;
                    } else {
                        int id=qs_dict_insert(dict,(const char *)v,length); if(id<0) return -3;
                        value=id;
                    }
                }
                else if(spec->type==Q_DICTIONARY) {
                    unsigned length=size-header;
                    if(spec->trim_spaces) while(length && v[length-1]==' ') --length;
                    for(unsigned s=0;s<p->nstrings;++s) {
                        const struct qs_string *text=&p->strings[s];
                        if(text->length>=sizeof(text->bytes)) return -3;
                        if(text->column==col && text->length==length && !memcmp(v,text->bytes,length)) { value=s+1; break; }
                    }
                }
                else if(spec->type==Q_PREFIX) { if(spec->prefix_len>sizeof(spec->prefix)) return -3; value=size-header>=spec->prefix_len && !memcmp(v,spec->prefix,spec->prefix_len); }
                else if(spec->type==Q_PATTERN) {
                    if(spec->prefix_len>sizeof(spec->prefix)) return -3;
                    unsigned length=size-header;
                    if(spec->trim_spaces) while(length && v[length-1]==' ') --length;
                    value=glob_match(spec->prefix,spec->prefix_len,(const char *)v,length);
                }
                else if(spec->type==Q_DECIMAL) { if(qs_numeric_cents(v,size-header,&value,&dscale)) return -3; }
                else if(spec->type==Q_CHAR) { if(size-header!=1 || *v>=128) return -3; value=*v; }
                else if(size==8 || size==4 || size==2 || size==1) value=fixed_number(v,size);
                else return -3;
                values[row]=value;
                scales[row]=dscale;
            }
            t->off=off+size;
        }
    }
    if(cursor) cursor->decoding=0;
    return 0;
}
/* Index scans retain the same tuple decoder and visibility checks. */
int qs_scan_tuple(const struct qs_plan *p,const unsigned char *raw,unsigned offset,unsigned len,unsigned page,struct qs_chunk *c,unsigned row,struct qs_dict *dict)
{
    int last=-1; for(unsigned i=0;i<p->nattrs;++i) if(p->cols[i].slot>=0) last=i;
    struct heap_tuple tuple={.data=raw+offset,.bytes=len,.absolute=page+offset,.row=row};
    return scan_tuples(p,&tuple,1,c,dict,last,NULL,NULL);
}
int64_t qs_scan_tuple_paged(struct qs_scan_cursor *cursor,const struct qs_plan *p,
    const unsigned char *raw,unsigned offset,unsigned len,unsigned page,struct qs_chunk *c,
    unsigned row,void *io,uint64_t completion)
{
    if(!cursor->active) {
        cursor->tuples[0]=(struct heap_tuple){.data=raw+offset,.bytes=len,.absolute=page+offset,.row=row};
        cursor->active=1; cursor->decoding=0;
    } else if(cursor->tuples[0].data!=raw+offset || cursor->tuples[0].bytes!=len || cursor->tuples[0].row!=row) return -1;
    cursor->io=io; cursor->completion=completion;
    int last=-1; for(unsigned i=0;i<p->nattrs;++i) if(p->cols[i].slot>=0) last=i;
    int64_t status=scan_tuples(p,cursor->tuples,1,c,NULL,last,NULL,cursor);
    if(!status) cursor->active=0;
    return status;
}
static int64_t scan_columns(struct femu_csd_args *a,struct qs_scan_cursor *cursor)
{
    if(!a || (a->numr!=4 && a->numr!=5) || a->mr_len[1]<(long long)sizeof(struct qs_plan) || a->mr_len[3]<(long long)sizeof(struct qs_task)) return -1;
    const struct qs_plan *p=a->mr_addr[1]; struct qs_chunk *c=a->mr_addr[2];
    struct qs_dict *dict=a->numr==5?a->mr_addr[4]:NULL;
    unsigned bytes=((struct qs_task *)a->mr_addr[3])->input_bytes;
    if(p->nattrs>QS_COLS || p->ncols>p->nattrs || p->nstrings>QS_STRINGS || a->mr_len[2]<(long long)sizeof(*c) ||
        c->ncols!=p->ncols || qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[2] ||
        bytes%QS_PAGE || bytes>(uint64_t)a->mr_len[0]) return -1;
    if(!cursor || !cursor->active) {
        c->rows=c->selected=0; memset(qs_nulls(c),0,((c->capacity+7)/8)*(size_t)c->ncols);
        if(cursor) { cursor->active=1; cursor->page=cursor->line=cursor->count=cursor->decoding=0; }
    }
    int last=-1; for(unsigned i=0;i<p->nattrs;++i) if(p->cols[i].slot>=0) last=i;
    /* Tuple data is MAXALIGNed. For tuples without NULLs, the leading fixed
     * offsets are known for the whole vector, including projected fields. */
    struct heap_prefix prefix;
    prefix.attrs=prefix.bytes=prefix.count=0;
    while((int)prefix.attrs<=last) {
        const struct qs_column *spec=&p->cols[prefix.attrs];
        unsigned size=spec->length;
        if(spec->length<=0 || (spec->slot>=0 && ((spec->type!=Q_INTEGER && spec->type!=Q_DATE) || spec->deferred ||
           (size!=1 && size!=2 && size!=4 && size!=8)))) break;
        unsigned offset=(prefix.bytes+spec->align-1)&~(spec->align-1U);
        if(spec->slot>=0) {
            prefix.projected[prefix.count].offset=offset;
            prefix.projected[prefix.count].size=size;
            prefix.projected[prefix.count++].slot=spec->slot;
        }
        prefix.bytes=offset+size;
        ++prefix.attrs;
    }
    prefix.borrowed_tail=(int)prefix.attrs<=last;
    for(unsigned col=prefix.attrs;prefix.borrowed_tail && (int)col<=last;++col) {
        const struct qs_column *spec=&p->cols[col];
        if(spec->slot<0 || spec->deferred) continue;
        prefix.borrowed_tail=(spec->type==Q_INTEGER || spec->type==Q_DATE) &&
            (spec->length==1 || spec->length==2 || spec->length==4 || spec->length==8);
    }
    for(unsigned page=cursor?cursor->page:0;page<bytes;page+=QS_PAGE) {
        const unsigned char *raw=(unsigned char *)a->mr_addr[0]+page;
        unsigned lower=read16(raw+12),upper=read16(raw+14);
        if(lower<24 || (lower-24)%4 || lower>upper || upper>QS_PAGE ||
            read16(raw+16)!=QS_PAGE || read16(raw+18)!=(QS_PAGE|4) || !(read16(raw+10)&4)) return -2;
        struct heap_tuple local[64];
        struct heap_tuple *tuples=cursor?cursor->tuples:local;
        unsigned count=cursor?cursor->count:0;
        if(cursor && cursor->decoding) {
            int64_t status=scan_tuples(p,tuples,count,c,dict,last,&prefix,cursor);
            if(status) return status;
            for(unsigned j=0;j<count;++j) qs_selection(c)[c->rows+j]=c->rows+j;
            c->rows+=count; count=cursor->count=0;
        }
        for(unsigned i=cursor?cursor->line:0;i<(lower-24)/4;++i) {
            uint32_t lp=read32(raw+24+4*i); unsigned offset=lp&0x7fff,flags=(lp>>15)&3,len=lp>>17;
            if(flags!=1) continue;
            if(offset<upper || offset>QS_PAGE || len<23 || len>QS_PAGE-offset || c->rows+count>=c->capacity) return -2;
            tuples[count]=(struct heap_tuple){.data=raw+offset,.bytes=len,.absolute=page+offset,.row=c->rows+count};
            ++count;
            if(cursor) { cursor->page=page; cursor->line=i+1; cursor->count=count; }
            if(count==64) {
                int64_t status=scan_tuples(p,tuples,count,c,dict,last,&prefix,cursor); if(status) return status;
                for(unsigned j=0;j<count;++j) qs_selection(c)[c->rows+j]=c->rows+j;
                c->rows+=count; count=0; if(cursor) cursor->count=0;
            }
        }
        if(count) {
            int64_t status=scan_tuples(p,tuples,count,c,dict,last,&prefix,cursor); if(status) return status;
            for(unsigned j=0;j<count;++j) qs_selection(c)[c->rows+j]=c->rows+j;
            c->rows+=count;
        }
        if(cursor) { cursor->page=page+QS_PAGE; cursor->line=cursor->count=0; }
    }
    if(cursor) cursor->active=0;
    c->selected=c->rows; return 0;
}
int64_t pg_sina_scan(struct femu_csd_args *a)
{ return scan_columns(a,NULL); }
int64_t qs_scan_columns(struct qs_scan_cursor *cursor,struct femu_csd_args *a,uint64_t completion)
{
    if(!cursor || !a || a->numr!=5 || a->mr_len[4]<QS_DICTIONARY_PAGE_BYTES) return -1;
    /* In this private call the fifth MR is I/O, not a flat dictionary. */
    void *mrs[]={a->mr_addr[0],a->mr_addr[1],a->mr_addr[2],a->mr_addr[3]};
    struct femu_csd_args input=*a; input.numr=4; input.mr_addr=mrs;
    cursor->io=a->mr_addr[4]; cursor->completion=completion;
    return scan_columns(&input,cursor);
}
