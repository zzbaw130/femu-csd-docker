#include "dictionary.h"
#include "cs_csf.h"
#include <limits.h>
#include <stdatomic.h>
#include <string.h>

/* String bodies, ID entries and hash heads all use the same bounded page
 * cache. Only small page descriptors remain resident. A returned string is
 * pinned, like DuckDB's BufferHandle; eviction never invalidates its bytes. */
#define PAGE QS_DICTIONARY_PAGE_BYTES
#define CHUNK_PAGES 64U
enum { ENTRY_FILE, BODY_FILE, HASH_FILE, REHASH_FILE, FILES };
enum { INSERT=1, PIN, CLOSE };
enum { STORE=1, LOAD, RETIRE, DROP };
struct entry { uint32_t hash,next,page; uint16_t offset,length; };
_Static_assert(sizeof(struct entry)==16,"dictionary entry layout");
struct page {
    char *data;
    uint64_t token,touched;
    atomic_uint pins;
    unsigned dirty,valid,metadata;
};
struct page_chunk { struct page_chunk *next; struct page pages[CHUNK_PAGES]; };
struct file { struct page_chunk *chunks; unsigned count; };
struct qs_dictionary {
    cs_work_context *context;
    atomic_uintptr_t owner;
    struct file files[FILES];
    uint32_t capacity,count,body_page,body_used;
    uint64_t clock,retired;
    unsigned io;
    struct page *pending;
    struct qs_dictionary_stats stats;
};
struct qs_dictionary_access {
    unsigned operation,phase,length,bucket,head,candidate,rehash_id;
    uint32_t hash,id;
    struct entry entry;
    struct page *pinned;
};

struct qs_dictionary *qs_dictionary_create(cs_work_context *parent)
{
    cs_work_context *context=cs_work_begin_child(parent,0);
    if(!context) return NULL;
    struct qs_dictionary *d=cs_work_calloc(context,1,sizeof(*d));
    if(!d) { cs_work_end(context); return NULL; }
    d->context=context; d->capacity=512;
    atomic_init(&d->owner,0);
    return d;
}
struct qs_dictionary_access *qs_dictionary_access_create(struct qs_dictionary *d)
{ return cs_work_calloc(d->context,1,sizeof(struct qs_dictionary_access)); }

static int64_t finish_io(struct qs_dictionary *d,void *io,uint64_t completion)
{
    if(!d->io) return completion?-3:0;
    if(!completion) return -3;
    struct page *p=d->pending;
    switch(d->io) {
    case STORE:
        d->retired=p->token; p->token=completion; p->dirty=0;
        ++d->stats.stores; d->stats.metadata_stores+=p->metadata;
        if(d->retired) { d->io=RETIRE; return csNeedFree(d->retired); }
        break;
    case LOAD:
        if(completion!=p->token) return -3;
        memcpy(p->data,io,PAGE); p->valid=1;
        ++d->stats.loads; d->stats.metadata_loads+=p->metadata;
        break;
    case RETIRE:
        if(completion!=d->retired) return -3;
        d->retired=0;
        break;
    case DROP:
        if(completion!=p->token) return -3;
        p->token=0;
        break;
    }
    d->io=0; d->pending=NULL;
    return 0;
}

static struct page *victim(struct qs_dictionary *d)
{
    struct page *best=NULL;
    for(unsigned f=0;f<FILES;++f)
        for(struct page_chunk *c=d->files[f].chunks;c;c=c->next)
            for(unsigned i=0;i<CHUNK_PAGES;++i) {
                struct page *p=&c->pages[i];
                if(p->data && !atomic_load_explicit(&p->pins,memory_order_acquire) &&
                   (!best || p->touched<best->touched)) best=p;
            }
    return best;
}
static int64_t allocate(struct qs_dictionary *d,size_t bytes,void **out,void *io)
{
    for(;;) {
        *out=cs_work_alloc(d->context,bytes);
        if(*out) return 0;
        struct page *p=victim(d);
        if(!p) return -4;
        if(p->dirty) {
            memcpy(io,p->data,PAGE); d->pending=p; d->io=STORE;
            return csNeedStore(PAGE);
        }
        cs_work_free(d->context,p->data); p->data=NULL; p->valid=0;
    }
}
/* A missing hash page reads as zeros and is allocated only when written.
 * Other files are append-only; descriptor addresses never change. */
static int64_t get_page(struct qs_dictionary *d,unsigned file,unsigned index,
                        int create,struct page **out,void *io)
{
    struct file *f=&d->files[file];
    if(index>=f->count && !create) { *out=NULL; return 0; }
    struct page_chunk **at=&f->chunks;
    for(unsigned n=0;n<=index/CHUNK_PAGES;++n) {
        if(!*at) {
            void *memory=NULL;
            int64_t rc=allocate(d,sizeof(struct page_chunk),&memory,io);
            if(rc) return rc;
            *at=memory; memset(*at,0,sizeof(**at));
            for(unsigned i=0;i<CHUNK_PAGES;++i) atomic_init(&(*at)->pages[i].pins,0);
        }
        if(n<index/CHUNK_PAGES) at=&(*at)->next;
    }
    if(index>=f->count) f->count=index+1;
    struct page *p=&(*at)->pages[index%CHUNK_PAGES];
    p->metadata=file!=BODY_FILE;
    if(!p->data) {
        void *memory=NULL;
        int64_t rc=allocate(d,PAGE,&memory,io);
        if(rc) return rc;
        p->data=memory;
        if(!p->token) { memset(p->data,0,PAGE); p->valid=1; }
    }
    p->touched=++d->clock;
    if(!p->valid) {
        d->pending=p; d->io=LOAD;
        return csNeedLoad(p->token);
    }
    *out=p; return 0;
}
static int64_t entry_page(struct qs_dictionary *d,uint32_t id,struct page **p,void *io)
{ return get_page(d,ENTRY_FILE,(id-1)/(PAGE/sizeof(struct entry)),1,p,io); }
static struct entry *entry_at(struct page *p,uint32_t id)
{ return &((struct entry *)p->data)[(id-1)%(PAGE/sizeof(struct entry))]; }
static uint32_t *head_at(struct page *p,unsigned bucket)
{ return &((uint32_t *)p->data)[bucket%(PAGE/sizeof(uint32_t))]; }
static int64_t hash_page(struct qs_dictionary *d,unsigned file,unsigned bucket,
                         int create,struct page **p,void *io)
{ return get_page(d,file,bucket/(PAGE/sizeof(uint32_t)),create,p,io); }
static int64_t discard_file(struct qs_dictionary *d,unsigned file)
{
    struct file *f=&d->files[file];
    for(struct page_chunk *c=f->chunks;c;c=c->next)
        for(unsigned i=0;i<CHUNK_PAGES;++i) {
            struct page *p=&c->pages[i];
            if(atomic_load_explicit(&p->pins,memory_order_acquire)) return QS_DICTIONARY_BUSY;
            if(p->token) { d->pending=p; d->io=DROP; return csNeedFree(p->token); }
        }
    struct page_chunk *c=f->chunks;
    while(c) {
        struct page_chunk *next=c->next;
        for(unsigned i=0;i<CHUNK_PAGES;++i) cs_work_free(d->context,c->pages[i].data);
        cs_work_free(d->context,c); c=next;
    }
    *f=(struct file){0}; return 0;
}
static int64_t begin(struct qs_dictionary *d,struct qs_dictionary_access *a,
                     unsigned operation,void *io,uint64_t completion)
{
    uintptr_t owner=0;
    if(!atomic_compare_exchange_strong_explicit(&d->owner,&owner,(uintptr_t)a,
        memory_order_acquire,memory_order_relaxed) && owner!=(uintptr_t)a)
        return QS_DICTIONARY_BUSY;
    if(a->pinned) {
        atomic_store_explicit(&d->owner,0,memory_order_release);
        return QS_DICTIONARY_BUSY;
    }
    if(a->operation && a->operation!=operation) return -3;
    a->operation=operation;
    return finish_io(d,io,completion);
}
static void done(struct qs_dictionary *d,struct qs_dictionary_access *a)
{
    a->operation=0; a->phase=0;
    atomic_store_explicit(&d->owner,0,memory_order_release);
}

int64_t qs_dictionary_insert(struct qs_dictionary *d,struct qs_dictionary_access *a,
    const char *key,unsigned length,uint32_t *id,void *io,uint64_t completion)
{
    if(length>UINT16_MAX || (!key && length)) return -3;
    int64_t rc=begin(d,a,INSERT,io,completion);
    if(rc) return rc;
    struct page *p=NULL;
    for(;;) switch(a->phase) {
    case 0:
        a->hash=2166136261U;
        for(unsigned i=0;i<length;++i) { a->hash^=(unsigned char)key[i]; a->hash*=16777619U; }
        a->length=length; a->bucket=a->hash&(d->capacity-1); a->phase=1;
        break;
    case 1:
        rc=hash_page(d,HASH_FILE,a->bucket,0,&p,io); if(rc) return rc;
        a->head=p?*head_at(p,a->bucket):0; a->candidate=a->head; a->phase=2;
        break;
    case 2:
        if(!a->candidate) { a->phase=4; break; }
        rc=entry_page(d,a->candidate,&p,io); if(rc) return rc;
        a->entry=*entry_at(p,a->candidate);
        if(a->entry.hash==a->hash && a->entry.length==length) a->phase=3;
        else a->candidate=a->entry.next;
        break;
    case 3:
        if(length) {
            rc=get_page(d,BODY_FILE,a->entry.page,0,&p,io); if(rc) return rc;
            if(!p) return -3;
        }
        if(!length || !memcmp(p->data+a->entry.offset,key,length)) {
            *id=a->candidate; done(d,a); return 0;
        }
        a->candidate=a->entry.next; a->phase=2;
        break;
    case 4:
        if(d->count==INT_MAX) return -4;
        if(d->count+1>d->capacity-d->capacity/4) {
            if(d->capacity>UINT_MAX/2) return -4;
            a->rehash_id=1; a->phase=10; break;
        }
        a->id=d->count+1;
        a->entry=(struct entry){.hash=a->hash,.next=a->head,.length=(uint16_t)length};
        a->phase=5;
        break;
    case 5:
        if(length) {
            unsigned page=d->body_page,offset=d->body_used;
            if(length>PAGE-offset) { ++page; offset=0; }
            rc=get_page(d,BODY_FILE,page,1,&p,io); if(rc) return rc;
            memcpy(p->data+offset,key,length); p->dirty=1;
            a->entry.page=page; a->entry.offset=(uint16_t)offset;
            d->body_page=page; d->body_used=offset+length;
        }
        a->phase=6;
        break;
    case 6:
        rc=entry_page(d,a->id,&p,io); if(rc) return rc;
        *entry_at(p,a->id)=a->entry; p->dirty=1; a->phase=7;
        break;
    case 7:
        rc=hash_page(d,HASH_FILE,a->bucket,1,&p,io); if(rc) return rc;
        *head_at(p,a->bucket)=a->id; p->dirty=1;
        ++d->count; d->stats.strings=d->count; d->stats.bytes+=length;
        *id=a->id; done(d,a); return 0;
    /* Rehash by stable ID, not by the chains being rewritten. Every phase
     * copies its inputs before a possible miss, so no page must remain pinned. */
    case 10:
        if(a->rehash_id>d->count) { a->phase=15; break; }
        rc=entry_page(d,a->rehash_id,&p,io); if(rc) return rc;
        a->entry=*entry_at(p,a->rehash_id);
        a->bucket=a->entry.hash&(d->capacity*2-1); a->phase=11;
        break;
    case 11:
        rc=hash_page(d,REHASH_FILE,a->bucket,0,&p,io); if(rc) return rc;
        a->entry.next=p?*head_at(p,a->bucket):0; a->phase=12;
        break;
    case 12:
        rc=entry_page(d,a->rehash_id,&p,io); if(rc) return rc;
        *entry_at(p,a->rehash_id)=a->entry; p->dirty=1; a->phase=13;
        break;
    case 13:
        rc=hash_page(d,REHASH_FILE,a->bucket,1,&p,io); if(rc) return rc;
        *head_at(p,a->bucket)=a->rehash_id; p->dirty=1;
        ++a->rehash_id; a->phase=10;
        break;
    case 15:
        rc=discard_file(d,HASH_FILE); if(rc) return rc;
        d->files[HASH_FILE]=d->files[REHASH_FILE]; d->files[REHASH_FILE]=(struct file){0};
        d->capacity*=2; a->bucket=a->hash&(d->capacity-1); a->phase=1;
        break;
    default: return -3;
    }
}

int64_t qs_dictionary_pin(struct qs_dictionary *d,struct qs_dictionary_access *a,
    uint32_t id,const char **bytes,unsigned *length,void *io,uint64_t completion)
{
    int64_t rc=begin(d,a,PIN,io,completion);
    if(rc) return rc;
    if(!id || id>d->count) { done(d,a); return -3; }
    struct page *p=NULL;
    if(!a->phase) {
        rc=entry_page(d,id,&p,io); if(rc) return rc;
        a->entry=*entry_at(p,id); a->phase=1;
    }
    *length=a->entry.length; *bytes="";
    if(*length) {
        rc=get_page(d,BODY_FILE,a->entry.page,0,&p,io); if(rc) return rc;
        if(!p) return -3;
        atomic_fetch_add_explicit(&p->pins,1,memory_order_acquire);
        a->pinned=p; *bytes=p->data+a->entry.offset;
    }
    done(d,a); return 0;
}
void qs_dictionary_unpin(struct qs_dictionary_access *a)
{
    if(a->pinned) atomic_fetch_sub_explicit(&a->pinned->pins,1,memory_order_release);
    a->pinned=NULL;
}
void qs_dictionary_get_stats(struct qs_dictionary *d,struct qs_dictionary_stats *out)
{ *out=d->stats; }
int64_t qs_dictionary_close(struct qs_dictionary *d,struct qs_dictionary_access *a,
    void *io,uint64_t completion)
{
    int64_t rc=begin(d,a,CLOSE,io,completion);
    if(rc) return rc;
    /* Do not partially destroy a dictionary while a caller still reads it. */
    for(unsigned f=0;f<FILES;++f)
        for(struct page_chunk *c=d->files[f].chunks;c;c=c->next)
            for(unsigned i=0;i<CHUNK_PAGES;++i)
                if(atomic_load_explicit(&c->pages[i].pins,memory_order_acquire)) {
                    done(d,a); return QS_DICTIONARY_BUSY;
                }
    for(unsigned f=0;f<FILES;++f) { rc=discard_file(d,f); if(rc) return rc; }
    cs_work_context *context=d->context;
    return cs_work_end(context);
}
void qs_dictionary_abort(struct qs_dictionary *d)
{ cs_work_end(d->context); }
