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
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

struct cache_relation {
    MemoryContextCallback cleanup;
    struct cache_relation *next;
    Oid relid,filenode;
    unsigned nsid,lba_bytes,count;
    const struct qs_extent *extents;
};
static struct cache_relation *relations;
static struct {
    int fd;
    struct femu_cache_slot *slots;
    const unsigned char *data;
    unsigned next;
    uint64 filled,cached,skipped,invalid,blocks,ns,minor_faults,major_faults;
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
void pg_sina_dcp_register(const struct qs_request *request,const struct qs_extent *extents)
{
    if(!pg_sina_dcp || !request->nextents) return;
    struct cache_relation *r=palloc0(sizeof(*r));
    r->relid=request->plan.relid; r->filenode=request->plan.relfilenode;
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
    for(unsigned i=0;i<FEMU_CACHE_RX_SLOTS;i++) pg_sina_dcp_poll();
    if(pg_sina_profile) ereport(NOTICE,(errmsg("SQLerate DCP blocks=%llu filled_pages=%llu cached_pages=%llu skipped_bytes=%llu invalid_pages=%llu wall_ms=%.3f minor_faults=%llu major_faults=%llu",
        (unsigned long long)receiver.blocks,(unsigned long long)receiver.filled,
        (unsigned long long)receiver.cached,(unsigned long long)receiver.skipped,
        (unsigned long long)receiver.invalid,receiver.ns/1e6,
        (unsigned long long)receiver.minor_faults,(unsigned long long)receiver.major_faults)));
    if(receiver.data!=MAP_FAILED) munmap((void *)receiver.data,FEMU_CACHE_DATA_BYTES);
    if(receiver.slots!=MAP_FAILED) munmap(receiver.slots,FEMU_CACHE_PAGE_BYTES);
    close(receiver.fd); receiver.fd=-1;
}
void pg_sina_dcp_start(CS_DEV_HANDLE device)
{
    if(!pg_sina_dcp || receiver.fd>=0) return;
    memset(&receiver,0,sizeof(receiver));
    receiver.fd=open("/dev/femu-cache",O_RDWR|O_CLOEXEC);
    receiver.slots=MAP_FAILED; receiver.data=MAP_FAILED;
    if(receiver.fd<0) ereport(ERROR,(errmsg("cannot open DCP receiver: %m")));
    if(ioctl(receiver.fd,FEMU_CACHE_RX_SETUP)<0) goto fail;
    receiver.slots=mmap(NULL,FEMU_CACHE_PAGE_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,receiver.fd,0);
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
static int in_buffer_pool(Relation rel,BlockNumber block)
{
    BufferTag tag; INIT_BUFFERTAG(tag,rel->rd_node,MAIN_FORKNUM,block);
    uint32 hash=BufTableHashCode(&tag);
    LWLock *lock=BufMappingPartitionLock(hash);
    LWLockAcquire(lock,LW_SHARED);
    int id=BufTableLookup(&tag,hash);
    int exists=id<0?0:(pg_atomic_read_u32(&GetBufferDescriptor(id)->state)&BM_VALID)?1:2;
    LWLockRelease(lock);
    return exists;
}
static void fill_slot(unsigned slot)
{
    const struct femu_cache_slot *s=&receiver.slots[slot];
    uint64 physical=le64toh(s->lba),bytes=le64toh(s->bytes);
    unsigned nsid=le32toh(s->nsid);
    const unsigned char *data=receiver.data+(size_t)slot*FEMU_CACHE_BLOCK_BYTES;
    PGAlignedBlock checked;
    bool checksums=DataChecksumsEnabled();
    Relation rel=NULL;
    for(uint64 position=0;position<bytes;) {
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
        if(!match) { receiver.skipped+=FEMU_CACHE_PAGE_BYTES; position+=FEMU_CACHE_PAGE_BYTES; continue; }
        if(!rel || RelationGetRelid(rel)!=match->relid) {
            if(rel) relation_close(rel,AccessShareLock);
            rel=relation_open(match->relid,AccessShareLock);
        }
        uint64 take=available;
        if(rel->rd_node.relNode!=match->filenode || logical%BLCKSZ || available<BLCKSZ) {
            take=Min(available,(uint64)(BLCKSZ-logical%BLCKSZ));
            receiver.skipped+=take; position+=take; continue;
        }
        take=available/BLCKSZ*BLCKSZ;
        for(uint64 offset=0;offset<take;offset+=BLCKSZ) {
            BlockNumber block=(logical+offset)/BLCKSZ;
            const void *page=data+position+offset;
            int present=in_buffer_pool(rel,block);
            if(present) {
                if(present==1) ++receiver.cached; else receiver.skipped+=BLCKSZ;
                continue;
            }
            /* PG's checksum verifier temporarily clears pd_checksum. Keep
             * the DMA mapping read-only; only checksum-enabled clusters need
             * this scratch copy. Normal header checks do not write the page. */
            if(checksums) { memcpy(checked.data,page,BLCKSZ); page=checked.data; }
            if(!PageIsVerifiedExtended((Page)page,block,0)) { ++receiver.invalid; continue; }
            Buffer buffer=ReadBufferExtended(rel,MAIN_FORKNUM,block,RBM_ZERO_AND_LOCK,NULL);
            /* A concurrent backend may have installed the page since lookup.
             * Never overwrite an existing page, including dirty hint bits. */
            if(PageIsNew(BufferGetPage(buffer)) &&
               !(pg_atomic_read_u32(&GetBufferDescriptor(buffer-1)->state)&BM_DIRTY)) {
                memcpy(BufferGetPage(buffer),page,BLCKSZ); ++receiver.filled;
            } else ++receiver.cached;
            /* These are existing immutable disk bytes: no WAL or dirty mark. */
            UnlockReleaseBuffer(buffer);
        }
        position+=take;
    }
    if(rel) relation_close(rel,AccessShareLock);
}
void pg_sina_dcp_poll(void)
{
    if(receiver.fd<0 || receiver.slots==MAP_FAILED || receiver.data==MAP_FAILED) return;
    for(unsigned n=0;n<FEMU_CACHE_RX_SLOTS;n++) {
        unsigned slot=(receiver.next+n)%FEMU_CACHE_RX_SLOTS;
        if(le64toh(__atomic_load_n(&receiver.slots[slot].ready,__ATOMIC_ACQUIRE))!=1) continue;
        receiver.next=(slot+1)%FEMU_CACHE_RX_SLOTS;
        uint64 start=now_ns();
        struct rusage before,after;
        if(pg_sina_profile) getrusage(RUSAGE_THREAD,&before);
        if(ioctl(receiver.fd,FEMU_CACHE_RX_ACQUIRE,slot)==0) {
            PG_TRY();
            { if(IsTransactionState() && !QueryCancelPending && !ProcDiePending) fill_slot(slot); }
            PG_CATCH();
            { ioctl(receiver.fd,FEMU_CACHE_RX_RELEASE,slot); PG_RE_THROW(); }
            PG_END_TRY();
        }
        unsigned flags=slot|(pg_sina_hpcp_enabled()?FEMU_CACHE_RELEASE_HPCP:0);
        if(ioctl(receiver.fd,FEMU_CACHE_RX_RELEASE,flags)<0)
            ereport(ERROR,(errmsg("cannot release DCP receive slot: %m")));
        receiver.ns+=now_ns()-start; ++receiver.blocks;
        if(pg_sina_profile) {
            getrusage(RUSAGE_THREAD,&after);
            receiver.minor_faults+=after.ru_minflt-before.ru_minflt;
            receiver.major_faults+=after.ru_majflt-before.ru_majflt;
        }
        return; /* Bound foreground work to one 4MiB slot per scheduling turn. */
    }
}
