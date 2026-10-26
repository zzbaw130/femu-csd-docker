#include "dictionary.h"
#include "cs_csf.h"
#include "scan.h"
#include "query.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test NVM only: these buffers emulate tokens, never operator workmem. */
struct storage {
    pthread_mutex_t mutex;
    void **tokens;
    size_t count,capacity,live,stores,loads,frees;
};
struct access {
    struct qs_dictionary_access *state;
    unsigned char io[QS_DICTIONARY_PAGE_BYTES];
};
static uint64_t complete(struct storage *s,int64_t request,void *io)
{
    uint64_t op=(uint64_t)request&~CS_IO_VALUE_MASK;
    uint64_t value=(uint64_t)request&CS_IO_VALUE_MASK;
    assert(!pthread_mutex_lock(&s->mutex));
    if(op==CS_NEED_STORE) {
        assert(value==QS_DICTIONARY_PAGE_BYTES);
        if(s->count+1>=s->capacity) {
            size_t capacity=s->capacity?s->capacity*2:128;
            void **tokens=realloc(s->tokens,capacity*sizeof(*tokens)); assert(tokens);
            memset(tokens+s->capacity,0,(capacity-s->capacity)*sizeof(*tokens));
            s->tokens=tokens; s->capacity=capacity;
        }
        value=++s->count;
        s->tokens[value]=malloc(QS_DICTIONARY_PAGE_BYTES); assert(s->tokens[value]);
        memcpy(s->tokens[value],io,QS_DICTIONARY_PAGE_BYTES); ++s->live; ++s->stores;
    } else {
        assert(value && value<=s->count && s->tokens[value]);
        if(op==CS_NEED_LOAD) { memcpy(io,s->tokens[value],QS_DICTIONARY_PAGE_BYTES); ++s->loads; }
        else {
            assert(op==CS_NEED_FREE);
            free(s->tokens[value]); s->tokens[value]=NULL; --s->live; ++s->frees;
        }
    }
    assert(!pthread_mutex_unlock(&s->mutex)); return value;
}
static void storage_end(struct storage *s)
{
    for(size_t i=1;i<=s->count;++i) free(s->tokens[i]);
    free(s->tokens); assert(!pthread_mutex_destroy(&s->mutex));
}
static void make_key(char *key,unsigned id,unsigned length)
{
    for(unsigned i=0;i<length;++i) key[i]=(char)((id*31+i*17)%251);
    assert(length>=sizeof(id)); memcpy(key,&id,sizeof(id));
}
static uint32_t insert(struct qs_dictionary *d,struct access *a,struct storage *s,
                       const char *key,unsigned length)
{
    uint64_t completion=0; uint32_t id=0;
    for(unsigned attempts=0;attempts<10000000;++attempts) {
        int64_t rc=qs_dictionary_insert(d,a->state,key,length,&id,a->io,completion);
        if(!rc) { assert(id); return id; }
        assert(rc>0); completion=0;
        if(rc==QS_DICTIONARY_BUSY) sched_yield();
        else completion=complete(s,rc,a->io);
    }
    assert(!"insert did not progress"); return 0;
}
static const char *pin(struct qs_dictionary *d,struct access *a,struct storage *s,
                        uint32_t id,unsigned *length)
{
    uint64_t completion=0; const char *bytes=NULL;
    for(unsigned attempts=0;attempts<10000000;++attempts) {
        int64_t rc=qs_dictionary_pin(d,a->state,id,&bytes,length,a->io,completion);
        if(!rc) return bytes;
        assert(rc>0); completion=0;
        if(rc==QS_DICTIONARY_BUSY) sched_yield(); else completion=complete(s,rc,a->io);
    }
    assert(!"pin did not progress"); return NULL;
}
static void close_dictionary(struct qs_dictionary *d,struct access *a,struct storage *s)
{
    uint64_t completion=0;
    for(;;) {
        int64_t rc=qs_dictionary_close(d,a->state,a->io,completion);
        if(!rc) break;
        assert(rc>QS_DICTIONARY_BUSY); completion=complete(s,rc,a->io);
    }
    assert(!s->live && s->stores==s->frees);
}
static void check_budget(cs_work_context *parent,size_t limit)
{
    cs_work_context_stats stats; assert(!cs_work_get_context_stats(parent,&stats));
    assert(stats.peak_used_bytes<=limit);
    printf("dictionary workmem peak=%zu limit=%zu\n",stats.peak_used_bytes,limit);
}
static void paging(unsigned count,unsigned key_length,size_t limit)
{
    cs_work_context *parent=cs_work_begin_limit(limit); assert(parent);
    struct qs_dictionary *d=qs_dictionary_create(parent); assert(d);
    struct access a={.state=qs_dictionary_access_create(d)};
    struct access held={.state=qs_dictionary_access_create(d)};
    assert(a.state && held.state);
    struct storage s={.mutex=PTHREAD_MUTEX_INITIALIZER};
    char key[65535]; unsigned length=0;
    const char *invalid=NULL;
    assert(qs_dictionary_pin(d,a.state,1,&invalid,&length,a.io,0)<0);
    assert(insert(d,&a,&s,"",0)==1);
    make_key(key,0,key_length); assert(insert(d,&a,&s,key,key_length)==2);
    const char *borrowed=pin(d,&held,&s,2,&length);
    assert(length==key_length && !memcmp(borrowed,key,length));
    assert(qs_dictionary_close(d,held.state,held.io,0)==QS_DICTIONARY_BUSY);
    for(unsigned i=1;i<count;++i) {
        make_key(key,i,key_length); assert(insert(d,&a,&s,key,key_length)==i+2);
    }
    make_key(key,0,key_length); assert(!memcmp(borrowed,key,key_length));
    assert(qs_dictionary_close(d,a.state,a.io,0)==QS_DICTIONARY_BUSY);
    qs_dictionary_unpin(held.state);
    /* Reverse/permuted access forces both body and ID directory reloads. */
    for(unsigned i=0;i<count;++i) {
        unsigned index=count-1-i; make_key(key,index,key_length);
        const char *bytes=pin(d,&a,&s,index+2,&length);
        assert(length==key_length && !memcmp(bytes,key,length));
        qs_dictionary_unpin(a.state);
        if(i%37==0) assert(insert(d,&a,&s,key,key_length)==index+2);
    }
    assert(insert(d,&a,&s,"",0)==1);
    /* Maximum length and binary keys (including NUL) are legal. */
    make_key(key,UINT32_MAX,sizeof(key));
    uint32_t last=insert(d,&a,&s,key,sizeof(key)); assert(last==count+2);
    assert(insert(d,&a,&s,key,sizeof(key))==last);
    const char *bytes=pin(d,&a,&s,last,&length);
    assert(length==sizeof(key) && !memcmp(bytes,key,length)); qs_dictionary_unpin(a.state);
    struct qs_dictionary_stats stats; qs_dictionary_get_stats(d,&stats);
    assert(stats.strings==count+2 && stats.bytes==(uint64_t)count*key_length+sizeof(key));
    assert(stats.stores && stats.loads && stats.metadata_stores && stats.metadata_loads);
    printf("dictionary strings=%llu stores=%llu loads=%llu metadata=%llu/%llu\n",
        (unsigned long long)stats.strings,(unsigned long long)stats.stores,
        (unsigned long long)stats.loads,(unsigned long long)stats.metadata_stores,
        (unsigned long long)stats.metadata_loads);
    check_budget(parent,limit); close_dictionary(d,&a,&s);
    cs_work_context_stats remaining; assert(!cs_work_get_context_stats(parent,&remaining));
    assert(!remaining.used_bytes && !remaining.allocations);
    assert(!cs_work_end(parent)); storage_end(&s);
}
struct worker { struct qs_dictionary *dict; struct access access; struct storage *storage; uint32_t ids[3000]; unsigned order; };
static void *worker_insert(void *arg)
{
    struct worker *w=arg; char key[64];
    for(unsigned i=0;i<3000;++i) {
        unsigned index=(i+w->order*719)%3000; make_key(key,index,sizeof(key));
        w->ids[index]=insert(w->dict,&w->access,w->storage,key,sizeof(key));
    }
    return NULL;
}
static void concurrent_insert(void)
{
    size_t limit=256U*1024;
    cs_work_context *parent=cs_work_begin_limit(limit); assert(parent);
    struct qs_dictionary *d=qs_dictionary_create(parent); assert(d);
    struct storage s={.mutex=PTHREAD_MUTEX_INITIALIZER};
    struct worker workers[4]; pthread_t threads[4];
    for(unsigned i=0;i<4;++i) {
        workers[i]=(struct worker){.dict=d,.storage=&s,.order=i};
        workers[i].access.state=qs_dictionary_access_create(d); assert(workers[i].access.state);
    }
    for(unsigned i=0;i<4;++i) assert(!pthread_create(&threads[i],NULL,worker_insert,&workers[i]));
    for(unsigned i=0;i<4;++i) assert(!pthread_join(threads[i],NULL));
    for(unsigned i=0;i<3000;++i)
        for(unsigned w=1;w<4;++w) assert(workers[w].ids[i]==workers[0].ids[i]);
    assert(s.stores && s.loads);
    check_budget(parent,limit); close_dictionary(d,&workers[0].access,&s);
    assert(!cs_work_end(parent)); storage_end(&s);
}
static void cancel_pending(uint64_t operation)
{
    cs_work_context *parent=cs_work_begin_limit(384U*1024); assert(parent);
    struct qs_dictionary *d=qs_dictionary_create(parent); assert(d);
    struct access a={.state=qs_dictionary_access_create(d)}; assert(a.state);
    struct storage s={.mutex=PTHREAD_MUTEX_INITIALIZER};
    char key[800]; uint64_t completion=0; uint32_t id=0;
    int cancelled=0;
    for(unsigned i=0;i<3000 && !cancelled;++i) {
        make_key(key,i,sizeof(key));
        for(;;) {
            int64_t rc=qs_dictionary_insert(d,a.state,key,sizeof(key),&id,a.io,completion);
            completion=0;
            if(!rc) break;
            assert(rc>QS_DICTIONARY_BUSY);
            completion=complete(&s,rc,a.io);
            if(((uint64_t)rc&~CS_IO_VALUE_MASK)==operation) { cancelled=1; break; }
        }
    }
    if(!cancelled && operation==CS_NEED_LOAD) {
        const char *bytes=NULL; unsigned length;
        for(;;) {
            int64_t rc=qs_dictionary_pin(d,a.state,1,&bytes,&length,a.io,completion);
            completion=0;
            if(!rc) break;
            assert(rc>QS_DICTIONARY_BUSY); completion=complete(&s,rc,a.io);
            if(((uint64_t)rc&~CS_IO_VALUE_MASK)==operation) { cancelled=1; break; }
        }
    }
    assert(cancelled); qs_dictionary_abort(d);
    cs_work_context_stats stats; assert(!cs_work_get_context_stats(parent,&stats));
    assert(!stats.used_bytes && !stats.allocations);
    /* Session cleanup must also reclaim the just-created, unacknowledged token. */
    storage_end(&s);
    d=qs_dictionary_create(parent); assert(d); qs_dictionary_abort(d);
    assert(!cs_work_end(parent));
}
static void write16(unsigned char *p,uint16_t value) { memcpy(p,&value,sizeof(value)); }
static void write32(unsigned char *p,uint32_t value) { memcpy(p,&value,sizeof(value)); }
static void heap_scan(void)
{
    enum { PAGES=223, ROWS_PER_PAGE=90, ROWS=PAGES*ROWS_PER_PAGE };
    unsigned char *raw=calloc(PAGES,QS_PAGE); assert(raw);
    for(unsigned page=0;page<PAGES;++page) {
        unsigned char *p=raw+(size_t)page*QS_PAGE;
        write16(p+10,4); write16(p+12,24+ROWS_PER_PAGE*4);
        write16(p+14,QS_PAGE-ROWS_PER_PAGE*64); write16(p+16,QS_PAGE); write16(p+18,QS_PAGE|4);
        for(unsigned i=0;i<ROWS_PER_PAGE;++i) {
            unsigned index=page*ROWS_PER_PAGE+i,offset=QS_PAGE-(i+1)*64;
            unsigned char *t=p+offset;
            write32(p+24+4*i,offset|(1U<<15)|(64U<<17));
            write16(t+18,3); write16(t+20,0xb01); t[22]=24; t[23]=index%7?7:5;
            t[24]=37; make_key((char *)t+25,index,17);
            unsigned at=42;
            if(index%7) { t[at++]=37; make_key((char *)t+at,ROWS-1-index,17); at+=17; }
            at=(at+3)&~3U; write32(t+at,index);
        }
    }
    struct qs_plan plan={.nattrs=3,.ncols=3};
    plan.cols[0]=(struct qs_column){.length=-1,.align=4,.slot=0,.type=Q_STRING};
    plan.cols[1]=(struct qs_column){.length=-1,.align=4,.slot=1,.type=Q_STRING};
    plan.cols[2]=(struct qs_column){.length=4,.align=4,.slot=2,.type=Q_INTEGER};
    size_t chunk_bytes=qs_chunk_bytes(ROWS,3);
    struct qs_chunk *chunk=calloc(1,chunk_bytes); assert(chunk);
    chunk->capacity=ROWS; chunk->ncols=3;
    size_t limit=384U*1024;
    cs_work_context *parent=cs_work_begin_limit(limit); assert(parent);
    struct qs_dictionary *d=qs_dictionary_create(parent); assert(d);
    struct qs_scan_cursor *cursor=qs_scan_cursor_create(parent,d); assert(cursor);
    struct access a={.state=qs_dictionary_access_create(d)}; assert(a.state);
    struct storage s={.mutex=PTHREAD_MUTEX_INITIALIZER};
    struct qs_task task={.input_bytes=PAGES*QS_PAGE};
    void *mrs[]={raw,&plan,chunk,&task,a.io};
    long long lengths[]={PAGES*QS_PAGE,sizeof(plan),chunk_bytes,sizeof(task),sizeof(a.io)};
    struct femu_csd_args args={.numr=5,.mr_addr=mrs,.mr_len=lengths};
    uint64_t completion=0;
    for(unsigned pass=0;pass<2;++pass) {
        completion=0;
        for(;;) {
            int64_t rc=qs_scan_columns(cursor,&args,completion);
            if(!rc) break;
            assert(rc>QS_DICTIONARY_BUSY);
            assert(!chunk->selected); /* No half-decoded batch is published. */
            completion=complete(&s,rc,a.io);
        }
        assert(chunk->rows==ROWS && chunk->selected==ROWS);
    }
    assert(chunk->rows==ROWS && chunk->selected==ROWS);
    char key[17]; unsigned length;
    for(unsigned row=0;row<ROWS;++row) {
        assert(qs_selection(chunk)[row]==row && qs_values(chunk)[2*ROWS+row]==row);
        assert(!!(qs_nulls(chunk)[(ROWS+7)/8+row/8]&(1U<<(row%8)))==!(row%7));
        for(unsigned col=0;col<2;++col) {
            if(col && !(row%7)) continue;
            make_key(key,col?ROWS-1-row:row,sizeof(key));
            const char *bytes=pin(d,&a,&s,(uint32_t)qs_values(chunk)[col*ROWS+row],&length);
            assert(length==sizeof(key) && !memcmp(bytes,key,length)); qs_dictionary_unpin(a.state);
        }
    }
    struct qs_dictionary_stats stats; qs_dictionary_get_stats(d,&stats);
    assert(stats.strings==ROWS && stats.stores && stats.loads && stats.metadata_loads);
    printf("heap dictionary resume: %u rows, %llu STORE, %llu LOAD\n",ROWS,
        (unsigned long long)stats.stores,(unsigned long long)stats.loads);
    check_budget(parent,limit); close_dictionary(d,&a,&s);
    assert(!cs_work_end(parent)); storage_end(&s); free(chunk); free(raw);
}
int main(void)
{
    assert(!cs_work_init(64U*1024*1024));
    paging(10000,800,4U*1024*1024);
    paging(20000,17,384U*1024);
    concurrent_insert(); cancel_pending(CS_NEED_STORE); cancel_pending(CS_NEED_LOAD); heap_scan();
    cs_work_stats stats; assert(!cs_work_get_stats(&stats));
    assert(!stats.used_bytes && !stats.active_contexts && !stats.active_allocations);
    puts("dictionary paging, metadata, pins, concurrent IDs and cancellation passed");
    return 0;
}
