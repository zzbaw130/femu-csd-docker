#define _GNU_SOURCE
#include "dictionary.h"
#include "cs_csf.h"
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* String bodies, ID entries and hash heads all use the same bounded page
 * cache. Only small page descriptors remain resident. A returned string is
 * pinned, like DuckDB's BufferHandle; eviction never invalidates its bytes. */
#define PAGE QS_DICTIONARY_PAGE_BYTES
#define CHUNK_PAGES 64U
enum { ENTRY_FILE, BODY_FILE, HASH_FILE, REHASH_FILE, FILES };
enum { INSERT=1, PIN, CLOSE, EVICT };
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
    /* Serialize only the current resident call. The logical owner below
     * survives I/O reentry; this mutex is always released before NEED_*. */
    pthread_mutex_t resident;
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
    uint32_t hash,id,pin_id;
    struct entry entry;
    struct page *pinned;
};
struct text_reference {
    struct qs_dictionary_access access;
    const char *bytes;
    uint32_t id;
    unsigned length,ready,copied;
    char copy[64];
};
/* Small per-CU memo of already interned short values. It owns copies, never
 * borrowed page pointers, so eviction cannot invalidate a hit. This avoids
 * serial dictionary operations for every repeated categorical Heap value.
 * Four entries per bucket prevent short categorical keys with the same hash
 * slot from evicting one another on every row. */
struct text_intern { uint32_t id; unsigned length; char bytes[64]; };
struct qs_dictionary_text {
    struct qs_dictionary *dictionary;
    struct text_intern (*interned)[4];
    unsigned intern_next;
    struct text_intern (*resolved)[4];
    unsigned resolved_next;
    struct text_reference references[2];
    struct qs_dictionary_access writer;
    const char *key;
    unsigned length,pending,insert_ready;
    uint32_t inserted;
    int64_t request;
};

struct qs_dictionary *qs_dictionary_create(cs_work_context *parent)
{
    cs_work_context *context=cs_work_begin_child(parent,0);
    if(!context) return NULL;
    struct qs_dictionary *d=cs_work_calloc(context,1,sizeof(*d));
    if(!d) { cs_work_end(context); return NULL; }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_ADAPTIVE_NP);
    int error=pthread_mutex_init(&d->resident,&attr); pthread_mutexattr_destroy(&attr);
    if(error) { cs_work_end(context); return NULL; }
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
        __atomic_fetch_add(&d->stats.stores,1,__ATOMIC_RELAXED); __atomic_fetch_add(&d->stats.metadata_stores,p->metadata,__ATOMIC_RELAXED);
        if(d->retired) { d->io=RETIRE; return csNeedFree(d->retired); }
        break;
    case LOAD:
        if(completion!=p->token) return -3;
        memcpy(p->data,io,PAGE); p->valid=1;
        __atomic_fetch_add(&d->stats.loads,1,__ATOMIC_RELAXED); __atomic_fetch_add(&d->stats.metadata_loads,p->metadata,__ATOMIC_RELAXED);
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
    if(operation!=PIN) a->pin_id=0;
    a->operation=operation;
    return finish_io(d,io,completion);
}
static void done(struct qs_dictionary *d,struct qs_dictionary_access *a)
{
    a->operation=0; a->phase=0;
    atomic_store_explicit(&d->owner,0,memory_order_release);
}

static int64_t dictionary_insert(struct qs_dictionary *d,struct qs_dictionary_access *a,
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
        __atomic_store_n(&d->count,d->count+1,__ATOMIC_RELEASE);
        __atomic_store_n(&d->stats.strings,d->count,__ATOMIC_RELEASE);
        __atomic_store_n(&d->stats.bytes,__atomic_load_n(&d->stats.bytes,__ATOMIC_RELAXED)+length,__ATOMIC_RELEASE);
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

static int64_t dictionary_pin(struct qs_dictionary *d,struct qs_dictionary_access *a,
    uint32_t id,const char **bytes,unsigned *length,void *io,uint64_t completion)
{
    int64_t rc=begin(d,a,PIN,io,completion);
    if(rc) return rc;
    if(!id || id>d->count) { done(d,a); return -3; }
    struct page *p=NULL;
    if(!a->phase) {
        /* Body location/length never change for a stable ID, even when
         * rehash rewrites its next link. Retain the last location, not a
         * borrowed metadata page pointer, across comparisons and eviction. */
        if(a->pin_id!=id) {
            rc=entry_page(d,id,&p,io); if(rc) return rc;
            a->entry=*entry_at(p,id); a->pin_id=id;
        }
        a->phase=1;
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
static int64_t dictionary_evict(struct qs_dictionary *d,struct qs_dictionary_access *a,
    void *io,uint64_t completion)
{
    int64_t rc=begin(d,a,EVICT,io,completion);
    if(rc) return rc;
    struct page *p=victim(d);
    if(!p) { done(d,a); return -4; }
    if(p->dirty) {
        memcpy(io,p->data,PAGE); d->pending=p; d->io=STORE;
        return csNeedStore(PAGE);
    }
    cs_work_free(d->context,p->data); p->data=NULL; p->valid=0;
    done(d,a); return 0;
}
void qs_dictionary_get_stats(struct qs_dictionary *d,struct qs_dictionary_stats *out)
{
    out->strings=__atomic_load_n(&d->stats.strings,__ATOMIC_ACQUIRE);
    out->bytes=__atomic_load_n(&d->stats.bytes,__ATOMIC_ACQUIRE);
    out->stores=__atomic_load_n(&d->stats.stores,__ATOMIC_RELAXED);
    out->loads=__atomic_load_n(&d->stats.loads,__ATOMIC_RELAXED);
    out->metadata_stores=__atomic_load_n(&d->stats.metadata_stores,__ATOMIC_RELAXED);
    out->metadata_loads=__atomic_load_n(&d->stats.metadata_loads,__ATOMIC_RELAXED);
}
int qs_dictionary_valid_id(const struct qs_dictionary *d,uint32_t id)
{ return id && id<=__atomic_load_n(&d->count,__ATOMIC_ACQUIRE); }
static int64_t dictionary_close(struct qs_dictionary *d,struct qs_dictionary_access *a,
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
    return 0;
}
/* Contention on a resident operation must not park an entire scan morsel
 * until a host completion. A call that needs storage unlocks immediately;
 * other callers then observe the logical owner and return BUSY as before. */
int64_t qs_dictionary_insert(struct qs_dictionary *d,struct qs_dictionary_access *a,
    const char *key,unsigned length,uint32_t *id,void *io,uint64_t completion)
{
    pthread_mutex_lock(&d->resident);
    int64_t status=dictionary_insert(d,a,key,length,id,io,completion);
    pthread_mutex_unlock(&d->resident); return status;
}
int64_t qs_dictionary_pin(struct qs_dictionary *d,struct qs_dictionary_access *a,
    uint32_t id,const char **bytes,unsigned *length,void *io,uint64_t completion)
{
    pthread_mutex_lock(&d->resident);
    int64_t status=dictionary_pin(d,a,id,bytes,length,io,completion);
    pthread_mutex_unlock(&d->resident); return status;
}
int64_t qs_dictionary_evict(struct qs_dictionary *d,struct qs_dictionary_access *a,
    void *io,uint64_t completion)
{
    pthread_mutex_lock(&d->resident);
    int64_t status=dictionary_evict(d,a,io,completion);
    pthread_mutex_unlock(&d->resident); return status;
}
int64_t qs_dictionary_close(struct qs_dictionary *d,struct qs_dictionary_access *a,
    void *io,uint64_t completion)
{
    pthread_mutex_lock(&d->resident);
    int64_t status=dictionary_close(d,a,io,completion);
    pthread_mutex_unlock(&d->resident);
    if(status) return status;
    pthread_mutex_destroy(&d->resident); return cs_work_end(d->context);
}
void qs_dictionary_abort(struct qs_dictionary *d)
{ pthread_mutex_destroy(&d->resident); cs_work_end(d->context); }

struct qs_dictionary_text *qs_dictionary_text_create(struct qs_dictionary *d)
{
    struct qs_dictionary_text *text=cs_work_calloc(d->context,1,sizeof(*text));
    if(text) text->dictionary=d;
    return text;
}
int64_t qs_dictionary_text_resume(struct qs_dictionary_text *t,void *io,uint64_t completion)
{
    if(!t->pending) return completion?-3:0;
    int64_t status;
    if(t->pending<=2) {
        struct text_reference *r=&t->references[t->pending-1];
        r->copied=0;
        status=qs_dictionary_pin(t->dictionary,&r->access,r->id,&r->bytes,&r->length,io,completion);
        if(!status) {
            /* Constant vectors repeatedly read the same small literal. Keep
             * its bytes between expressions, not a pin on its cache page. */
            if(r->length<=sizeof(r->copy)) {
                if(r->length) memcpy(r->copy,r->bytes,r->length);
                qs_dictionary_unpin(&r->access); r->bytes=r->copy; r->copied=1;
                /* Repeated categorical expressions need no shared dictionary
                 * operation once this CU owns a short immutable byte copy. */
                if(!t->resolved) t->resolved=cs_work_calloc(t->dictionary->context,64,sizeof(*t->resolved));
                if(t->resolved) {
                    struct text_intern *bucket=t->resolved[r->id%64],*entry=&bucket[t->resolved_next++%4];
                    for(unsigned j=0;j<4;++j) if(!bucket[j].id || bucket[j].id==r->id) { entry=&bucket[j]; break; }
                    entry->id=r->id; entry->length=r->length;
                    if(r->length) memcpy(entry->bytes,r->copy,r->length);
                }
            }
            r->ready=1;
        }
    } else {
        status=qs_dictionary_insert(t->dictionary,&t->writer,t->key,t->length,&t->inserted,io,completion);
        if(!status) t->insert_ready=1;
    }
    t->request=status;
    if(!status) t->pending=0;
    return status;
}
int64_t qs_dictionary_text_get(struct qs_dictionary_text *t,uint32_t id,
    const char **bytes,unsigned *length,void *io)
{
    for(unsigned i=0;i<2;++i) if(t->references[i].ready && t->references[i].id==id) {
        *bytes=t->references[i].bytes; *length=t->references[i].length; return 0;
    }
    if(t->pending) return t->request;
    for(unsigned i=0;i<2;++i) if(t->references[i].copied && t->references[i].id==id) {
        struct text_reference *r=&t->references[i];
        r->ready=1; *bytes=r->copy; *length=r->length; return 0;
    }
    for(unsigned i=0;i<2;++i) if(!t->references[i].ready) {
        struct text_reference *r=&t->references[i];
        if(t->resolved) for(unsigned j=0;j<4;++j) {
            const struct text_intern *entry=&t->resolved[id%64][j];
            if(entry->id!=id) continue;
            /* Preserve the existing two-reference lifetime: the returned
             * pointer must not change if a memo bucket is replaced later. */
            r->id=id; r->length=entry->length; r->ready=r->copied=1;
            if(r->length) memcpy(r->copy,entry->bytes,r->length);
            r->bytes=r->copy; *bytes=r->bytes; *length=r->length; return 0;
        }
        r->id=id; t->pending=i+1;
        int64_t status=qs_dictionary_text_resume(t,io,0);
        if(!status) { *bytes=r->bytes; *length=r->length; }
        return status;
    }
    return -4;
}
int64_t qs_dictionary_text_insert(struct qs_dictionary_text *t,const char *bytes,
    unsigned length,uint32_t *id,void *io)
{
    struct text_intern *cached=NULL;
    if(length<=sizeof(t->interned[0][0].bytes) && (!length || bytes)) {
        /* Numeric-only CUs need no cache. This optimization is optional when
         * its workmem allocation cannot fit; the paged lookup still works. */
        if(!t->interned && !t->pending && !t->insert_ready)
            t->interned=cs_work_calloc(t->dictionary->context,64,sizeof(*t->interned));
        if(!t->interned) goto lookup;
        uint32_t hash=2166136261U;
        for(unsigned i=0;i<length;++i) { hash^=(unsigned char)bytes[i]; hash*=16777619U; }
        struct text_intern *bucket=t->interned[hash%64];
        cached=&bucket[t->intern_next%4];
        for(unsigned i=0;i<4;++i) {
            struct text_intern *entry=&bucket[i];
            if(!entry->id) cached=entry;
            if(!t->pending && !t->insert_ready && entry->id && entry->length==length &&
               (!length || !memcmp(entry->bytes,bytes,length))) { *id=entry->id; return 0; }
        }
    }
lookup:
    if(!t->insert_ready) {
        if(t->pending) return t->request;
        t->key=bytes; t->length=length; t->pending=3;
        int64_t status=qs_dictionary_text_resume(t,io,0); if(status) return status;
    }
    if(t->length!=length || (length && memcmp(t->key,bytes,length))) return -3;
    *id=t->inserted;
    if(cached) {
        cached->id=*id; cached->length=length; if(length) memcpy(cached->bytes,bytes,length);
        ++t->intern_next;
    }
    t->insert_ready=0; t->key=NULL; return 0;
}
int64_t qs_dictionary_text_request(const struct qs_dictionary_text *t)
{ return t->request; }
int qs_dictionary_text_release(struct qs_dictionary_text *t)
{
    if(t->pending || t->insert_ready) return -1;
    for(unsigned i=0;i<2;++i) {
        qs_dictionary_unpin(&t->references[i].access); t->references[i].ready=0;
    }
    t->request=0; return 0;
}
