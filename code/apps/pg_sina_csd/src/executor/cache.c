/* DCP: install immutable disk pages returned by FEMU in PG shared_buffers.
 * Only the backend calls PG APIs. SNIA/device threads only deliver DMA data. */
#include "pg_internal.h"
#include "miscadmin.h"
#include "access/relation.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "utils/rel.h"
#include "utils/memutils.h"
#include "csd-cache.h"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

struct cache_relation {
    MemoryContextCallback cleanup;
    struct cache_relation *next;
    Oid relid,filenode;
    RelFileNode node;
    uint64 bulk_pages;
    unsigned nsid,lba_bytes,count;
    BufferAccessStrategy strategy;
    const struct qs_extent *extents;
    uint64 received,filled,cached,bulk_skipped,fragment_skipped;
};
static struct cache_relation *relations;
static struct {
    int fd;
    struct femu_cache_slot *slots;
    const unsigned char *data;
    unsigned next;
    uint64 seen[FEMU_CACHE_RX_SLOTS];
    int active;
    bool failed,hpcp,paused,handed_off,slot_complete;
    CS_DEV_HANDLE device;
    uint64 position;
    uint64 filled,cached,skipped,invalid,blocks,ns,minor_faults,major_faults;
    uint64 steps,max_step_ns,lookup_ns,allocate_ns,copy_ns;
    uint64 admission_pages,remaining_pages,capacity_skipped;
    uint64 unmapped,resident_hits;
} receiver={.fd=-1};
static uint64 now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64)t.tv_sec*1000000000+t.tv_nsec;
}
static void unregister_relation(void *arg)
{
    struct cache_relation *r=arg,**link=&relations;
    while(*link && *link!=r) link=&(*link)->next;
    if(*link) *link=r->next;
}
uint64 pg_sina_dcp_bulk_pages(uint64 blocks,char relkind,bool indexed)
{
    if(!pg_sina_hpcp_enabled() || indexed || relkind!=RELKIND_RELATION ||
       blocks<=(uint64)(NBuffers/4)) return 0;
    return Min(NBuffers/8,256*1024/BLCKSZ);
}
void pg_sina_dcp_register(const struct qs_request *request,const struct qs_extent *extents)
{
    if((!pg_sina_dcp && !pg_sina_hpcp_enabled()) || !request->nextents) return;
    struct cache_relation *r=palloc0(sizeof(*r));
    r->relid=request->plan.relid; r->filenode=request->plan.relfilenode;
    Relation rel=relation_open(r->relid,AccessShareLock); r->node=rel->rd_node;
    if(pg_sina_dcp)
        r->bulk_pages=pg_sina_dcp_bulk_pages(request->bytes/BLCKSZ,
            rel->rd_rel->relkind,request->plan.join.indexid!=InvalidOid);
    if(r->bulk_pages) r->strategy=GetAccessStrategy(BAS_BULKREAD);
    relation_close(rel,AccessShareLock);
    r->nsid=request->nsid; r->lba_bytes=request->lba_bytes;
    r->count=request->nextents; r->extents=extents;
    r->next=relations; relations=r;
    r->cleanup=(MemoryContextCallback){.func=unregister_relation,.arg=r};
    MemoryContextRegisterResetCallback(CurrentMemoryContext,&r->cleanup);
}
void pg_sina_dcp_finish(void)
{
    if(receiver.fd<0) return;
    /* The caller has stopped device DMA before draining these last slots. */
    for(unsigned i=0;i<FEMU_CACHE_RX_SLOTS;i++)
        do { pg_sina_dcp_poll(true); } while(receiver.active>=0);
    if(pg_sina_profile) ereport(NOTICE,(errmsg("SQLerate DCP blocks=%llu filled_pages=%llu cached_pages=%llu skipped_bytes=%llu invalid_pages=%llu wall_ms=%.3f minor_faults=%llu major_faults=%llu steps=%llu max_step_ms=%.3f lookup_ms=%.3f allocate_ms=%.3f copy_ms=%.3f admission_pages=%llu remaining_pages=%llu capacity_skipped_bytes=%llu",
        (unsigned long long)receiver.blocks,(unsigned long long)receiver.filled,
        (unsigned long long)receiver.cached,(unsigned long long)receiver.skipped,
        (unsigned long long)receiver.invalid,receiver.ns/1e6,
        (unsigned long long)receiver.minor_faults,(unsigned long long)receiver.major_faults,
        (unsigned long long)receiver.steps,receiver.max_step_ns/1e6,receiver.lookup_ns/1e6,
        receiver.allocate_ns/1e6,receiver.copy_ns/1e6,
        (unsigned long long)receiver.admission_pages,(unsigned long long)receiver.remaining_pages,
        (unsigned long long)receiver.capacity_skipped)));
    if(pg_sina_profile && receiver.slots!=MAP_FAILED) {
        ereport(NOTICE,(errmsg("SQLerate DCP fast_resident_pages=%llu",(unsigned long long)receiver.resident_hits)));
        const struct femu_cache_stats *s=(const void *)((const unsigned char *)receiver.slots+FEMU_CACHE_STATS_OFFSET);
        ereport(NOTICE,(errmsg("SQLerate population candidate_bytes=%llu resident_bytes=%llu dma_bytes=%llu queue_dropped_bytes=%llu credit_dropped_bytes=%llu overwrite_dropped_bytes=%llu error_dropped_bytes=%llu stop_dropped_bytes=%llu",
            (unsigned long long)le64toh(s->candidate_bytes),(unsigned long long)le64toh(s->resident_bytes),
            (unsigned long long)le64toh(s->dma_bytes),(unsigned long long)le64toh(s->queue_dropped_bytes),
            (unsigned long long)le64toh(s->credit_dropped_bytes),(unsigned long long)le64toh(s->overwrite_dropped_bytes),
            (unsigned long long)le64toh(s->error_dropped_bytes),(unsigned long long)le64toh(s->stop_dropped_bytes))));
        for(struct cache_relation *r=relations;r;r=r->next)
            if(r->received) ereport(NOTICE,(errmsg("SQLerate DCP relation relid=%u bulk=%u received_bytes=%llu filled_pages=%llu cached_pages=%llu bulk_skipped_bytes=%llu fragment_skipped_bytes=%llu",
                r->relid,r->strategy!=NULL,(unsigned long long)r->received,
                (unsigned long long)r->filled,(unsigned long long)r->cached,
                (unsigned long long)r->bulk_skipped,(unsigned long long)r->fragment_skipped)));
        ereport(NOTICE,(errmsg("SQLerate DCP unmapped_bytes=%llu",(unsigned long long)receiver.unmapped)));
    }
    if(receiver.data!=MAP_FAILED) munmap((void *)receiver.data,FEMU_CACHE_DATA_BYTES);
    if(receiver.slots!=MAP_FAILED) munmap(receiver.slots,FEMU_CACHE_DESCRIPTOR_BYTES);
    close(receiver.fd); receiver.fd=-1;
}
/* PG keeps a 256KiB bulk-read ring for large sequential scans. Once that
 * working set is resident, optional DCP need not rotate every streamed page
 * through it: HPCP retains the full file working set. */
static uint64 shared_target(const struct cache_relation *r,const struct pg_sina_cache_entry *entry)
{
    return r->strategy?Min(entry->blocks,r->bulk_pages):entry->blocks;
}
/* Query-local population hint, never an input to SQL execution. Skip only
 * pages already satisfying every enabled destination, including the bulk
 * ring target. Unknown observations conservatively request population. */
void pg_sina_dcp_resident(Oid relid,uint64 logical,uint64 bytes,unsigned char *bits)
{
    struct cache_relation *r=relations;
    while(r && r->relid!=relid) r=r->next;
    const struct pg_sina_cache_entry *entry=r?pg_sina_cache_lookup(r->node):NULL;
    if(!entry || (pg_sina_hpcp_enabled() && (!entry->file_known || !entry->file_pages))) return;
    bool shared=!pg_sina_dcp || (r->strategy && entry->shared_pages>=shared_target(r,entry));
    if(shared && (!pg_sina_hpcp_enabled() || entry->file_pages==entry->file_total)) {
        memset(bits,255,(bytes/FEMU_CACHE_PAGE_BYTES+7)/8); return;
    }
    for(uint64 i=0;i<bytes/FEMU_CACHE_PAGE_BYTES;i++) {
        uint64 file=logical/FEMU_CACHE_PAGE_BYTES+i,block=(logical+i*FEMU_CACHE_PAGE_BYTES)/BLCKSZ;
        if((shared || (block<entry->blocks && (entry->shared_bits[block/8]&(1U<<(block%8))))) &&
           (!pg_sina_hpcp_enabled() || (file<entry->file_total && (entry->file_bits[file/8]&(1U<<(file%8))))))
            bits[i/8]|=1U<<(i%8);
    }
}
/* Existing bitmaps describe actual pages, not a cumulative fill counter.
 * Unknown/stale observations admit work; never pause merely from table size. */
static void population_needed(bool *file,bool *shared)
{
    *file=*shared=relations==NULL;
    pg_sina_cache_poll();
    for(struct cache_relation *r=relations;r;r=r->next) {
        const struct pg_sina_cache_entry *entry=pg_sina_cache_lookup(r->node);
        *file|=!entry || !entry->file_known || entry->file_pages<entry->file_total;
        *shared|=!entry || entry->shared_pages<shared_target(r,entry);
    }
}
static void pause_population(CS_DEV_HANDLE device)
{
    struct femu_cache_config cache={.receive_fd=-1};
    CsConfigInfo info={.Type=CS_VENDOR_SPECIFIC,.u.VSInfo={.VSData=&cache}};
    CsConfigData out; int length=sizeof(out);
    CS_STATUS status=csConfig(device,&length,&info,&out);
    if(status!=CS_SUCCESS) elog(ERROR,"cannot pause FEMU cache population: status=%d",status);
    receiver.paused=true;
    if(pg_sina_profile) ereport(NOTICE,(errmsg("SQLerate population paused device_dma=off reason=resident_or_capacity")));
}
void pg_sina_dcp_start(CS_DEV_HANDLE device)
{
    if((!pg_sina_dcp && !pg_sina_hpcp_enabled()) || receiver.fd>=0) return;
    memset(&receiver,0,sizeof(receiver)); receiver.fd=-1; receiver.active=-1; receiver.device=device;
    bool file,shared; population_needed(&file,&shared);
    receiver.hpcp=pg_sina_hpcp_enabled() && file;
    /* Population is optional: leave 10% for HOST/catalog work. Count all
     * occupied buffers, not only this query's relations. Within a query the
     * allowance only decreases, preventing repeated whole-table admission. */
    uint64 occupied=0,limit=(uint64)NBuffers-NBuffers/10;
    for(int i=0;pg_sina_dcp && shared && i<NBuffers;++i)
        if(pg_atomic_read_u32(&GetBufferDescriptor(i)->state)&BM_TAG_VALID) ++occupied;
    receiver.admission_pages=receiver.remaining_pages=pg_sina_dcp && shared && occupied<limit?limit-occupied:0;
    if(!receiver.remaining_pages && !receiver.hpcp) { pause_population(device); return; }
    /* No PG pages can be admitted: let SNIA's existing file-cache worker
     * consume the ring instead of routing HPCP through the PG backend. */
    if(!pg_sina_dcp || !receiver.remaining_pages) return;
    receiver.active=-1;
    receiver.fd=open("/dev/femu-cache",O_RDWR|O_CLOEXEC);
    receiver.slots=MAP_FAILED; receiver.data=MAP_FAILED;
    if(receiver.fd<0) ereport(ERROR,(errmsg("cannot open DCP receiver: %m")));
    if(receiver.hpcp && ioctl(receiver.fd,FEMU_CACHE_RX_ADMISSION)==FEMU_CACHE_HPCP_FULL) receiver.hpcp=false;
    if(!receiver.remaining_pages && !receiver.hpcp) {
        close(receiver.fd); receiver.fd=-1; pause_population(device); return;
    }
    if(ioctl(receiver.fd,FEMU_CACHE_RX_SETUP)<0) goto fail;
    unsigned long targets=FEMU_CACHE_TARGET_DCP|(receiver.hpcp?FEMU_CACHE_TARGET_HPCP:0);
    if(ioctl(receiver.fd,FEMU_CACHE_RX_MODE,targets)<0) goto fail;
    receiver.slots=mmap(NULL,FEMU_CACHE_DESCRIPTOR_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,receiver.fd,0);
    receiver.data=mmap(NULL,FEMU_CACHE_DATA_BYTES,PROT_READ,MAP_SHARED,receiver.fd,FEMU_CACHE_DATA_OFFSET);
    if(receiver.slots==MAP_FAILED || receiver.data==MAP_FAILED) goto fail;
    struct femu_cache_config cache={.receive_fd=receiver.fd};
    CsConfigInfo info={.Type=CS_VENDOR_SPECIFIC,.u.VSInfo={.VSData=&cache}};
    CsConfigData out; int length=sizeof(out);
    CS_STATUS status=csConfig(device,&length,&info,&out);
    if(status==CS_SUCCESS) return;
    pg_sina_dcp_finish();
    elog(ERROR,"cannot configure FEMU DCP receiver: status=%d",status);
fail:
    /* No device was bound yet; finish sees no ready slots. */
    pg_sina_dcp_finish();
    ereport(ERROR,(errmsg("cannot map DCP receive data: %m")));
}
static void fill_slot(unsigned slot,bool whole_slot)
{
    const struct femu_cache_slot *s=&receiver.slots[slot];
    uint64 physical=le64toh(s->lba),bytes=le64toh(s->bytes);
    unsigned nsid=le32toh(s->nsid);
    const unsigned char *data=receiver.data+FEMU_CACHE_SLOT_OFFSET(slot);
    PGAlignedBlock checked;
    bool checksums=DataChecksumsEnabled();
    Relation rel=NULL;
    uint64 position=receiver.position,end=whole_slot?bytes:Min(bytes,position+256*1024);
    uint64 deadline=whole_slot?0:now_ns()+250000;
    for(;position<end;) {
        if(!receiver.remaining_pages) {
            receiver.slot_complete=false;
            receiver.skipped+=bytes-position;
            receiver.capacity_skipped+=bytes-position;
            position=bytes;
            break;
        }
        struct cache_relation *match=NULL; uint64 logical=0,available=0;
        for(struct cache_relation *r=relations;r && !match;r=r->next) {
            if(r->nsid!=nsid || physical>UINT64_MAX/r->lba_bytes) continue;
            uint64 at=physical*r->lba_bytes+position;
            for(unsigned i=0;i<r->count;i++) {
                const struct qs_extent *e=&r->extents[i]; uint64 first=e->lba*r->lba_bytes;
                if(at>=first && at-first<e->bytes) {
                    match=r; logical=e->logical+at-first; available=Min(bytes-position,e->bytes-(at-first)); break;
                }
            }
        }
        if(!match) { receiver.slot_complete=false; receiver.unmapped+=FEMU_CACHE_PAGE_BYTES; receiver.skipped+=FEMU_CACHE_PAGE_BYTES; position+=FEMU_CACHE_PAGE_BYTES; continue; }
        const struct pg_sina_cache_entry *cached=pg_sina_cache_lookup(match->node);
        uint64 target=cached?shared_target(match,cached):UINT64_MAX;
        if(match->strategy && cached && cached->shared_pages>=target) {
            uint64 take=Min(available,bytes-position);
            match->received+=take; match->bulk_skipped+=take;
            receiver.skipped+=take; position+=take; continue;
        }
        if(!rel || RelationGetRelid(rel)!=match->relid) {
            if(rel) relation_close(rel,AccessShareLock);
            rel=relation_open(match->relid,AccessShareLock);
        }
        uint64 take=Min(available,end-position);
        if(rel->rd_node.relNode!=match->filenode || logical%BLCKSZ || available<BLCKSZ) {
            receiver.slot_complete=false;
            take=Min(available,(uint64)(BLCKSZ-logical%BLCKSZ));
            match->received+=take; match->fragment_skipped+=take;
            receiver.skipped+=take; position+=take; continue;
        }
        take=Min(available,end-position)/BLCKSZ*BLCKSZ;
        if(!take) break; /* Leave a page straddling the slice for the next turn. */
        for(uint64 offset=0;offset<take;offset+=BLCKSZ) {
            if(!receiver.remaining_pages || (match->strategy && cached && cached->shared_pages>=target)) {
                take=offset;
                break;
            }
            BlockNumber block=(logical+offset)/BLCKSZ;
            if(cached && block<cached->blocks && (cached->shared_bits[block/8]&(1U<<(block%8)))) {
                ++receiver.cached; ++receiver.resident_hits; ++match->cached; continue;
            }
            const void *page=data+position+offset;
            /* PG's checksum verifier temporarily clears pd_checksum. Keep
             * the DMA mapping read-only; only checksum-enabled clusters need
             * this scratch copy. Normal header checks do not write the page. */
            if(checksums) { memcpy(checked.data,page,BLCKSZ); page=checked.data; }
            if(!PageIsVerifiedExtended((Page)page,block,0)) { receiver.slot_complete=false; ++receiver.invalid; continue; }
            uint64 mark=pg_sina_profile?now_ns():0;
            /* With HPCP retaining the large sequential working set, use
             * PG's own bounded bulk-read ring for DCP. Default allocation
             * would duplicate gigabytes and evict the file cache just filled.
             * Small tables/indexes and DCP-only retain normal PG admission. */
            Buffer buffer=ReadBufferExtended(rel,MAIN_FORKNUM,block,RBM_ZERO_AND_LOCK,
                match->strategy);
            if(pg_sina_profile) receiver.allocate_ns+=now_ns()-mark;
            /* ReadBufferExtended does the lookup, pin and locking itself.
             * Keep its zeroing step; never overwrite an existing page,
             * including dirty hint bits. No PG buffer internals are changed. */
            if(PageIsNew(BufferGetPage(buffer)) &&
               !(pg_atomic_read_u32(&GetBufferDescriptor(buffer-1)->state)&BM_DIRTY)) {
                mark=pg_sina_profile?now_ns():0;
                memcpy(BufferGetPage(buffer),page,BLCKSZ); ++receiver.filled;
                ++match->filled;
                --receiver.remaining_pages;
                if(pg_sina_profile) receiver.copy_ns+=now_ns()-mark;
            } else { ++receiver.cached; ++match->cached; }
            pg_sina_cache_buffer(buffer);
            /* These are existing immutable disk bytes: no WAL or dirty mark. */
            UnlockReleaseBuffer(buffer);
            /* Yield only between complete pages, with no buffer pin held.
             * Retain the DMA slot until every slice has been consumed. */
            if(!whole_slot && now_ns()>=deadline) { take=offset+BLCKSZ; break; }
        }
        match->received+=take; position+=take;
        if(!whole_slot && now_ns()>=deadline) break;
    }
    if(rel) relation_close(rel,AccessShareLock);
    receiver.position=position;
}
bool pg_sina_dcp_poll(bool whole_slot)
{
    pg_sina_cache_poll();
    if(receiver.paused || receiver.handed_off || receiver.fd<0 || receiver.slots==MAP_FAILED || receiver.data==MAP_FAILED) return false;
    uint64 start=now_ns();
    struct rusage before,after;
    if(pg_sina_profile) getrusage(RUSAGE_THREAD,&before);
    if(receiver.active<0) for(unsigned n=0;n<FEMU_CACHE_RX_SLOTS;n++) {
        volatile unsigned slot=(receiver.next+n)%FEMU_CACHE_RX_SLOTS;
        if(le64toh(__atomic_load_n(&receiver.slots[slot].ready,__ATOMIC_ACQUIRE))!=1) continue;
        if(__atomic_load_n(&receiver.slots[slot].token,__ATOMIC_RELAXED)==receiver.seen[slot]) continue;
        if(ioctl(receiver.fd,FEMU_CACHE_RX_ACQUIRE,(unsigned long)slot)<0) {
            if(errno==EAGAIN || errno==EALREADY) continue;
            ereport(ERROR,(errmsg("cannot acquire DCP receive slot: %m")));
        }
        receiver.seen[slot]=__atomic_load_n(&receiver.slots[slot].token,__ATOMIC_RELAXED);
        receiver.active=slot; receiver.position=0; receiver.slot_complete=true;
        receiver.next=(slot+1)%FEMU_CACHE_RX_SLOTS;
        break;
    }
    if(receiver.active>=0) {
        unsigned slot=receiver.active;
        PG_TRY();
        {
            if(!receiver.paused && !receiver.failed && IsTransactionState() && !QueryCancelPending && !ProcDiePending) fill_slot(slot,whole_slot);
            else { receiver.position=le64toh(receiver.slots[slot].bytes); receiver.slot_complete=false; }
        }
        PG_CATCH();
        { ioctl(receiver.fd,FEMU_CACHE_RX_RELEASE,slot); receiver.active=-1; receiver.failed=true; PG_RE_THROW(); }
        PG_END_TRY();
        if(receiver.position>=le64toh(receiver.slots[slot].bytes)) {
            unsigned flags=slot|(receiver.slot_complete?FEMU_CACHE_RELEASE_DCP:0);
            int result=ioctl(receiver.fd,FEMU_CACHE_RX_RELEASE,flags);
            if(result<0)
                ereport(ERROR,(errmsg("cannot release DCP receive slot: %m")));
            receiver.active=-1; ++receiver.blocks;
            if(receiver.hpcp && !whole_slot) {
                bool file,shared; population_needed(&file,&shared);
                if(!shared) receiver.remaining_pages=0;
            }
            if(!receiver.remaining_pages && receiver.hpcp && !whole_slot) {
                struct femu_cache_config cache={.receive_fd=FEMU_CACHE_HPCP_WORKER};
                CsConfigInfo info={.Type=CS_VENDOR_SPECIFIC,.u.VSInfo={.VSData=&cache}};
                CsConfigData out; int length=sizeof(out);
                CS_STATUS status=csConfig(receiver.device,&length,&info,&out);
                if(status!=CS_SUCCESS) elog(ERROR,"cannot return cache receiver to HPCP: status=%d",status);
                receiver.handed_off=true;
            }
            if(!receiver.paused && !receiver.hpcp) {
                bool file,shared; population_needed(&file,&shared);
                if(!receiver.remaining_pages || !shared) pause_population(receiver.device);
            }
        }
        uint64 elapsed=now_ns()-start;
        receiver.ns+=elapsed; ++receiver.steps;
        receiver.max_step_ns=Max(receiver.max_step_ns,elapsed);
        if(pg_sina_profile) {
            getrusage(RUSAGE_THREAD,&after);
            receiver.minor_faults+=after.ru_minflt-before.ru_minflt;
            receiver.major_faults+=after.ru_majflt-before.ru_majflt;
        }
        return true;
    }
    return false;
}
