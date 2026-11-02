/* The backend owns PG snapshots and DCP updates. A worker observes file pages
 * in C-owned storage; the backend publishes completed results without waiting. */
#include "pg_internal.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/ipc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>

static struct {
    MemoryContext context;
    HTAB *entries;
    BufferTag *tags; /* buffer_id -> last observed page, including other forks */
    unsigned generation;
    bool valid;
} snapshot;
static unsigned depth;
static bool refreshing;
static long os_page;
static char io_stat_path[MAXPGPATH];
static uint64 dcp_updates,dcp_evictions;
static ExecutorStart_hook_type previous_start;
static ExecutorRun_hook_type previous_run;
static ExecutorFinish_hook_type previous_finish;
static ExecutorEnd_hook_type previous_end;

static bool bit_change(unsigned char *bits,uint64 bit,bool set,uint64 *count)
{
    unsigned char mask=1U<<(bit%8);
    bool present=(bits[bit/8]&mask)!=0;
    if(present==set) return false;
    if(set) { bits[bit/8]|=mask; ++*count; }
    else { bits[bit/8]&=~mask; --*count; }
    return true;
}
const struct pg_sina_cache_entry *pg_sina_cache_lookup(RelFileNode node)
{
    return snapshot.valid?hash_search(snapshot.entries,&node,HASH_FIND,NULL):NULL;
}
void pg_sina_cache_buffer(int buffer)
{
    if(!snapshot.valid || buffer<=0) return;
    BufferTag tag,old=snapshot.tags[buffer-1];
    BufferGetTag(buffer,&tag.rnode,&tag.forkNum,&tag.blockNum);
    if(BUFFERTAGS_EQUAL(old,tag)) return;
    struct pg_sina_cache_entry *before=hash_search(snapshot.entries,&old.rnode,HASH_FIND,NULL);
    if(before && old.forkNum==MAIN_FORKNUM && old.blockNum<before->blocks)
        if(bit_change(before->shared_bits,old.blockNum,false,&before->shared_pages)) ++dcp_evictions;
    struct pg_sina_cache_entry *after=RelFileNodeEquals(old.rnode,tag.rnode)?before:
        hash_search(snapshot.entries,&tag.rnode,HASH_FIND,NULL);
    if(after && tag.forkNum==MAIN_FORKNUM && tag.blockNum<after->blocks)
        bit_change(after->shared_bits,tag.blockNum,true,&after->shared_pages);
    snapshot.tags[buffer-1]=tag; ++dcp_updates;
}
/* The worker owns ordinary C allocations only. It never follows a PG hash
 * entry, BufferDesc, MemoryContext or ResourceOwner pointer. */
struct file_result {
    RelFileNode node;
    uint64 blocks,total,pages;
    unsigned char *bits;
    bool known;
    char base[MAXPGPATH];
};
struct file_job {
    unsigned generation,count;
    uint64 submitted_ns,started_ns,finished_ns;
    struct file_result files[];
};
static _Atomic(struct file_job *) queued_files,completed_files;
static atomic_bool file_stopping=ATOMIC_VAR_INIT(false);
static atomic_bool file_observing=ATOMIC_VAR_INIT(false);
static sem_t file_wake;
static pthread_t file_worker;
static bool worker_started;
static uint64 coalesced_files;
static uint64 monotonic_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64)t.tv_sec*1000000000+t.tv_nsec;
}
static void free_files(struct file_job *job)
{
    if(!job) return;
    for(unsigned i=0;i<job->count;++i) free(job->files[i].bits);
    free(job);
}
static struct file_job *make_files(void)
{
    unsigned count=hash_get_num_entries(snapshot.entries);
    struct file_job *job=calloc(1,sizeof(*job)+(size_t)count*sizeof(job->files[0]));
    if(!job) elog(ERROR,"cannot allocate cache file observation");
    job->count=count; job->generation=snapshot.generation;
    HASH_SEQ_STATUS scan; hash_seq_init(&scan,snapshot.entries);
    volatile bool scanning=true;
    PG_TRY();
    {
        struct pg_sina_cache_entry *entry; unsigned i=0;
        while((entry=hash_seq_search(&scan))) {
            struct file_result *f=&job->files[i++];
            f->node=entry->node; f->blocks=entry->blocks; f->total=entry->file_total;
            char *base=relpathperm(entry->node,MAIN_FORKNUM);
            strlcpy(f->base,base,sizeof(f->base)); pfree(base);
            f->bits=calloc(Max((f->total+7)/8,1),1);
            if(!f->bits) elog(ERROR,"cannot allocate file residency bits");
        }
        scanning=false;
    }
    PG_CATCH();
    { if(scanning) hash_seq_term(&scan); free_files(job); PG_RE_THROW(); }
    PG_END_TRY();
    return job;
}
static void observe_files(struct file_job *job)
{
    job->started_ns=monotonic_ns();
    for(unsigned i=0;i<job->count && !atomic_load_explicit(&file_stopping,memory_order_relaxed);++i) {
        struct file_result *f=&job->files[i];
        uint64 total=f->blocks*BLCKSZ,segment_bytes=(uint64)RELSEG_SIZE*BLCKSZ;
        f->known=true;
        for(uint64 at=0;at<total;) {
            if(atomic_load_explicit(&file_stopping,memory_order_relaxed)) break;
            unsigned segment=at/segment_bytes;
            char path[MAXPGPATH+32];
            if(segment) snprintf(path,sizeof(path),"%s.%u",f->base,segment);
            else snprintf(path,sizeof(path),"%s",f->base);
            int fd=open(path,O_RDONLY|O_CLOEXEC);
            if(fd<0) { f->known=false; break; }
            size_t bytes=Min(total-at,segment_bytes),pages=(bytes+os_page-1)/os_page;
            unsigned char *vector=malloc(pages);
            if(!vector) { close(fd); f->known=false; break; }
            void *map=mmap(NULL,bytes,PROT_NONE,MAP_SHARED,fd,0);
            if(map==MAP_FAILED) f->known=false;
            else {
                if(mincore(map,bytes,vector)) f->known=false;
                else {
                    size_t j=0;
                    /* mincore has one byte per page; pack eight resident
                     * bits at a time, including fully resident segments. */
                    for(;j+8<=pages;j+=8) {
                        uint64 lanes=0;
                        for(unsigned k=0;k<8;++k) lanes|=(uint64)(vector[j+k]&1)<<(8*k);
                        unsigned char packed=((lanes&UINT64_C(0x0101010101010101))*UINT64_C(0x0102040810204080))>>56;
                        f->bits[(at/os_page+j)/8]=packed;
                        f->pages+=__builtin_popcount(packed);
                    }
                    for(;j<pages;++j) if(vector[j]&1) {
                        uint64 bit=at/os_page+j;
                        f->bits[bit/8]|=1U<<(bit%8); ++f->pages;
                    }
                }
                munmap(map,bytes);
            }
            free(vector); close(fd); at+=bytes;
        }
    }
    job->finished_ns=monotonic_ns();
}
static void publish_files(struct file_job *job)
{
    unsigned applied=0,generation=job->generation;
    uint64 submitted=job->submitted_ns,finished=job->finished_ns,started=job->started_ns;
    for(unsigned i=0;i<job->count;++i) {
        const struct file_result *f=&job->files[i];
        struct pg_sina_cache_entry *entry=hash_search(snapshot.entries,&f->node,HASH_FIND,NULL);
        if(!entry || entry->blocks!=f->blocks || entry->file_total!=f->total || entry->file_generation>generation) continue;
        memcpy(entry->file_bits,f->bits,(f->total+7)/8);
        entry->file_pages=f->pages; entry->file_known=f->known;
        entry->file_generation=generation; ++applied;
    }
    free_files(job);
    if(pg_sina_profile) ereport(NOTICE,(errmsg("SQLerate cache file publish generation=%u shared_generation=%u relations=%u observe_ms=%.3f submitted_ns=%llu finished_ns=%llu published_ns=%llu",
        generation,snapshot.generation,applied,(finished-started)/1e6,
        (unsigned long long)submitted,(unsigned long long)finished,(unsigned long long)monotonic_ns())));
}
void pg_sina_cache_poll(void)
{
    if(!snapshot.entries || !atomic_load_explicit(&completed_files,memory_order_acquire)) return;
    struct file_job *job=atomic_exchange_explicit(&completed_files,NULL,memory_order_acq_rel);
    if(job) publish_files(job);
}
static void *observe_loop(void *unused)
{
    struct file_job *metadata=NULL;
    for(;;) {
        int result;
        if(metadata && atomic_load_explicit(&file_observing,memory_order_relaxed)) {
            struct timespec deadline; clock_gettime(CLOCK_REALTIME,&deadline); ++deadline.tv_sec;
            do { result=sem_timedwait(&file_wake,&deadline); } while(result<0 && errno==EINTR);
        } else do { result=sem_wait(&file_wake); } while(result<0 && errno==EINTR);
        if(atomic_load_explicit(&file_stopping,memory_order_relaxed)) break;
        struct file_job *job=atomic_exchange_explicit(&queued_files,NULL,memory_order_acq_rel);
        if(job) {
            free(metadata);
            size_t bytes=sizeof(*job)+(size_t)job->count*sizeof(job->files[0]);
            metadata=malloc(bytes);
            if(metadata) {
                memcpy(metadata,job,bytes);
                for(unsigned i=0;i<metadata->count;++i) metadata->files[i].bits=NULL;
            }
        } else if(result<0 && metadata && atomic_load_explicit(&file_observing,memory_order_relaxed)) {
            size_t bytes=sizeof(*metadata)+(size_t)metadata->count*sizeof(metadata->files[0]);
            job=malloc(bytes);
            if(!job) continue;
            memcpy(job,metadata,bytes);
            bool allocated=true;
            for(unsigned i=0;i<job->count;++i) {
                struct file_result *f=&job->files[i]; f->pages=0;
                f->bits=calloc(Max((f->total+7)/8,1),1);
                allocated&=f->bits!=NULL;
            }
            if(!allocated) { free_files(job); continue; }
            job->submitted_ns=monotonic_ns();
        }
        if(!job) continue;
        observe_files(job);
        if(atomic_load_explicit(&file_stopping,memory_order_relaxed)) { free_files(job); break; }
        free_files(atomic_exchange_explicit(&completed_files,job,memory_order_acq_rel));
    }
    free(metadata);
    return NULL;
}
static void stop_files(int code,Datum arg)
{
    if(!worker_started) return;
    atomic_store_explicit(&file_stopping,true,memory_order_relaxed);
    sem_post(&file_wake); pthread_join(file_worker,NULL);
    free_files(atomic_exchange_explicit(&queued_files,NULL,memory_order_acq_rel));
    free_files(atomic_exchange_explicit(&completed_files,NULL,memory_order_acq_rel));
    sem_destroy(&file_wake); worker_started=false;
}
static void submit_files(struct file_job *job)
{
    job->submitted_ns=monotonic_ns();
    if(!worker_started) { observe_files(job); publish_files(job); return; }
    /* One running job plus the latest queued job. Pointer exchange and wakeup
     * never wait for file observation; superseded metadata can be discarded. */
    struct file_job *old=atomic_exchange_explicit(&queued_files,job,memory_order_acq_rel);
    if(old) { ++coalesced_files; free_files(old); }
    else sem_post(&file_wake);
}
static HTAB *build_snapshot(MemoryContext context,BufferTag *tags,unsigned generation)
{
    HASHCTL control={.keysize=sizeof(RelFileNode),.entrysize=sizeof(struct pg_sina_cache_entry),.hcxt=context};
    HTAB *entries=hash_create("SQLerate cache page bitmaps",128,&control,HASH_ELEM|HASH_BLOBS|HASH_CONTEXT);
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"cannot enumerate cache relations");
    int status=SPI_execute("SELECT c.oid FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE c.relkind IN ('r','i','m') AND c.relpersistence='p' "
        "AND n.nspname !~ '^pg_' AND n.nspname <> 'information_schema' ORDER BY c.oid",true,0);
    if(status!=SPI_OK_SELECT) elog(ERROR,"cannot enumerate cache relations");
    for(uint64 i=0;i<SPI_processed;++i) {
        bool isnull; Oid oid=DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,1,&isnull));
        Relation rel=relation_open(oid,AccessShareLock);
        struct pg_sina_cache_entry *entry=hash_search(entries,&rel->rd_node,HASH_ENTER,NULL);
        *entry=(struct pg_sina_cache_entry){.node=rel->rd_node,.relid=oid,
            .blocks=RelationGetNumberOfBlocks(rel),.generation=generation};
        entry->file_total=(entry->blocks*BLCKSZ+os_page-1)/os_page;
        entry->shared_bits=MemoryContextAllocZero(context,Max((entry->blocks+7)/8,1));
        entry->file_bits=MemoryContextAllocZero(context,Max((entry->file_total+7)/8,1));
        const struct pg_sina_cache_entry *old=snapshot.entries?hash_search(snapshot.entries,&entry->node,HASH_FIND,NULL):NULL;
        if(old && old->blocks==entry->blocks) {
            memcpy(entry->file_bits,old->file_bits,(entry->file_total+7)/8);
            entry->file_pages=old->file_pages; entry->file_known=old->file_known;
            entry->file_generation=old->file_generation;
        }
        relation_close(rel,AccessShareLock);
    }
    SPI_finish();
    struct pg_sina_cache_entry *entry;
    /* Metadata access above may itself replace buffers. Observe descriptors
     * last so the published snapshot includes those replacements too. */
    for(int i=0;i<NBuffers;++i) {
        BufferDesc *buffer=GetBufferDescriptor(i);
        if((pg_atomic_read_u32(&buffer->state)&(BM_VALID|BM_TAG_VALID))!=(BM_VALID|BM_TAG_VALID)) continue;
        uint32 state=LockBufHdr(buffer); BufferTag tag=buffer->tag;
        UnlockBufHdr(buffer,state);
        if((state&(BM_VALID|BM_TAG_VALID))==(BM_VALID|BM_TAG_VALID)) {
            tags[i]=tag;
            entry=hash_search(entries,&tag.rnode,HASH_FIND,NULL);
            if(entry && tag.forkNum==MAIN_FORKNUM && tag.blockNum<entry->blocks)
                bit_change(entry->shared_bits,tag.blockNum,true,&entry->shared_pages);
        }
        if(!(i%1024)) CHECK_FOR_INTERRUPTS();
    }
    return entries;
}
static uint64 differences(const unsigned char *a,uint64 na,const unsigned char *b,uint64 nb)
{
    uint64 changed=0;
    for(uint64 i=0;i<Max((na+7)/8,(nb+7)/8);++i)
        changed+=__builtin_popcount((i<(na+7)/8?a[i]:0)^(i<(nb+7)/8?b[i]:0));
    return changed;
}
static void refresh(const char *reason)
{
    instr_time begin,end; INSTR_TIME_SET_CURRENT(begin);
    pg_sina_cache_poll();
    /* Relation/file metadata already belongs to this connection. Start the
     * asynchronous observation before collecting PG buffer tags, so HOST
     * prewarming is visible sooner without making the next planner wait. */
    bool early_files=worker_started && snapshot.valid;
    if(early_files) {
        struct file_job *files=make_files();
        files->generation=snapshot.generation+1;
        submit_files(files);
    }
    MemoryContext context=AllocSetContextCreate(TopMemoryContext,"SQLerate cache residency",ALLOCSET_DEFAULT_SIZES);
    MemoryContext previous=CurrentMemoryContext;
    BufferTag *tags=MemoryContextAllocZero(context,(Size)NBuffers*sizeof(BufferTag));
    HTAB *volatile entries=NULL;
    bool compiling=pg_sina_compiling_plan;
    bool own_snapshot=!ActiveSnapshotSet();
    refreshing=true; pg_sina_compiling_plan=true;
    PG_TRY();
    {
        if(own_snapshot) PushActiveSnapshot(GetTransactionSnapshot());
        entries=build_snapshot(context,tags,snapshot.generation+1);
        if(own_snapshot) PopActiveSnapshot();
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(previous); MemoryContextDelete(context);
        refreshing=false; pg_sina_compiling_plan=compiling; snapshot.valid=false; PG_RE_THROW();
    }
    PG_END_TRY();
    pg_sina_compiling_plan=compiling; refreshing=false;
    if(pg_sina_profile) {
        HASH_SEQ_STATUS scan; hash_seq_init(&scan,(HTAB *)entries);
        struct pg_sina_cache_entry *entry;
        while((entry=hash_seq_search(&scan))) {
            const struct pg_sina_cache_entry *old=snapshot.entries?hash_search(snapshot.entries,&entry->node,HASH_FIND,NULL):NULL;
            uint64 delta=old?differences(old->shared_bits,old->blocks,entry->shared_bits,entry->blocks):entry->shared_pages;
            ereport(NOTICE,(errmsg("SQLerate cache ledger relation generation=%u relid=%u blocks=%llu shared_pages=%llu file_pages=%llu file_total=%llu file_known=%d file_generation=%u shared_delta_pages=%llu",
                entry->generation,entry->relid,(unsigned long long)entry->blocks,(unsigned long long)entry->shared_pages,
                (unsigned long long)entry->file_pages,(unsigned long long)entry->file_total,entry->file_known,entry->file_generation,(unsigned long long)delta)));
        }
    }
    if(snapshot.context) MemoryContextDelete(snapshot.context);
    snapshot.context=context; snapshot.entries=(HTAB *)entries; snapshot.tags=tags;
    snapshot.valid=true; ++snapshot.generation;
    if(!early_files) submit_files(make_files());
    INSTR_TIME_SET_CURRENT(end); INSTR_TIME_SUBTRACT(end,begin);
    if(pg_sina_profile) ereport(NOTICE,(errmsg("SQLerate cache ledger refresh reason=%s generation=%u wall_ms=%.3f relations=%ld dcp_updates=%llu dcp_evictions=%llu coalesced=%llu returned_ns=%llu",
        reason,snapshot.generation,INSTR_TIME_GET_MILLISEC(end),hash_get_num_entries(snapshot.entries),
        (unsigned long long)dcp_updates,(unsigned long long)dcp_evictions,
        (unsigned long long)coalesced_files,(unsigned long long)monotonic_ns())));
    dcp_updates=dcp_evictions=0;
}
static void start(QueryDesc *query,int flags)
{
    ++depth;
    PG_TRY(); { if(previous_start) previous_start(query,flags); else standard_ExecutorStart(query,flags); }
    PG_CATCH(); { --depth; PG_RE_THROW(); } PG_END_TRY();
    --depth;
}
static bool host_reads(Plan *plan)
{
    if(!plan) return false;
    if(IsA(plan,SeqScan) || IsA(plan,IndexScan) || IsA(plan,IndexOnlyScan) || IsA(plan,BitmapHeapScan)) return true;
    List *children=NIL; ListCell *c;
    if(IsA(plan,CustomScan)) children=((CustomScan *)plan)->custom_plans;
    else if(IsA(plan,Append)) children=((Append *)plan)->appendplans;
    else if(IsA(plan,MergeAppend)) children=((MergeAppend *)plan)->mergeplans;
    else if(IsA(plan,SubqueryScan) && host_reads(((SubqueryScan *)plan)->subplan)) return true;
    foreach(c,children) if(host_reads(lfirst(c))) return true;
    return host_reads(plan->lefttree) || host_reads(plan->righttree);
}
/* Optional query profiling only. Namespace counters include all PG workers;
 * sample in the leader once, never sum duplicated per-worker disk totals.
 * CSD internal LOAD/spill remain in the existing device metrics. */
struct query_io {
    unsigned long long reads,sectors,free,file,shmem,scan,steal,refault;
    bool valid;
};
static struct query_io read_query_io(void)
{
    struct query_io out={0}; unsigned long long merged;
    FILE *f=fopen(io_stat_path,"r");
    if(f) {
        out.valid=fscanf(f,"%llu %llu %llu",&out.reads,&merged,&out.sectors)==3;
        fclose(f);
    }
    f=fopen("/proc/vmstat","r");
    if(f) {
        char name[80]; unsigned long long value;
        while(fscanf(f,"%79s %llu",name,&value)==2) {
            if(!strcmp(name,"nr_free_pages")) out.free=value;
            else if(!strcmp(name,"nr_file_pages")) out.file=value;
            else if(!strcmp(name,"nr_shmem")) out.shmem=value;
            else if(!strcmp(name,"pgscan_kswapd") || !strcmp(name,"pgscan_direct")) out.scan+=value;
            else if(!strcmp(name,"pgsteal_kswapd") || !strcmp(name,"pgsteal_direct")) out.steal+=value;
            else if(!strcmp(name,"workingset_refault_file")) out.refault=value;
        }
        fclose(f);
    }
    return out;
}
static void run(QueryDesc *query,ScanDirection direction,uint64 count,bool once)
{
    bool outer=!depth && !refreshing && !pg_sina_compiling_plan && !IsParallelWorker() && query->plannedstmt->rtable!=NIL;
    volatile bool measure=outer && pg_sina_profile;
    struct query_io before={0}; if(measure) before=read_query_io();
    volatile bool observe=outer && snapshot.valid;
    if(observe) {
        bool reads=host_reads(query->plannedstmt->planTree); ListCell *c;
        foreach(c,query->plannedstmt->subplans) reads|=host_reads(lfirst(c));
        observe=reads;
    }
    if(observe) {
        /* Reuse this worker while HOST scans are changing residency. A
         * one-second cadence avoids an entire-query lag after prewarming;
         * the planner still never waits for an observation. */
        atomic_store_explicit(&file_observing,true,memory_order_relaxed);
        submit_files(make_files());
    }
    ++depth;
    PG_TRY(); { if(previous_run) previous_run(query,direction,count,once); else standard_ExecutorRun(query,direction,count,once); }
    PG_CATCH(); { --depth; if(observe) atomic_store(&file_observing,false); PG_RE_THROW(); } PG_END_TRY();
    --depth;
    if(observe) atomic_store_explicit(&file_observing,false,memory_order_relaxed);
    if(measure) {
        struct query_io after=read_query_io();
        ereport(NOTICE,(errmsg("SQLerate query IO available=%d device_reads=%llu device_read_bytes=%llu reclaim_scan_pages=%llu reclaim_steal_pages=%llu refault_file=%llu free_before_bytes=%llu free_after_bytes=%llu file_before_bytes=%llu file_after_bytes=%llu shmem_before_bytes=%llu shmem_after_bytes=%llu",
            before.valid && after.valid,after.reads-before.reads,(after.sectors-before.sectors)*512,
            after.scan-before.scan,after.steal-before.steal,after.refault-before.refault,
            before.free*os_page,after.free*os_page,before.file*os_page,after.file*os_page,before.shmem*os_page,after.shmem*os_page)));
    }
}
static void finish(QueryDesc *query)
{
    ++depth;
    PG_TRY(); { if(previous_finish) previous_finish(query); else standard_ExecutorFinish(query); }
    PG_CATCH(); { --depth; PG_RE_THROW(); } PG_END_TRY();
    --depth;
}
static void end(QueryDesc *query)
{
    bool outer=!depth && !refreshing && !pg_sina_compiling_plan && !IsParallelWorker();
    ++depth;
    PG_TRY(); { if(previous_end) previous_end(query); else standard_ExecutorEnd(query); }
    PG_CATCH(); { --depth; PG_RE_THROW(); } PG_END_TRY();
    --depth;
    if(outer) refresh("executor_end");
}
static void transaction_end(XactEvent event,void *arg)
{
    if(event==XACT_EVENT_ABORT || event==XACT_EVENT_PARALLEL_ABORT) snapshot.valid=false;
}
void pg_sina_cache_init(void)
{
    os_page=sysconf(_SC_PAGESIZE);
    const char *device=pg_sina_device_path(),*base=strrchr(device,'/');
    snprintf(io_stat_path,sizeof(io_stat_path),"/sys/class/block/%s/stat",base?base+1:device);
    bool own_transaction=!IsTransactionState();
    if(own_transaction) StartTransactionCommand();
    bool own_snapshot=!ActiveSnapshotSet();
    if(own_snapshot) PushActiveSnapshot(GetTransactionSnapshot());
    refresh("connection_init");
    if(own_snapshot) PopActiveSnapshot();
    if(own_transaction) CommitTransactionCommand();
    if(sem_init(&file_wake,0,0)) elog(ERROR,"cannot initialize cache observer: %m");
    on_proc_exit(stop_files,0);
    sigset_t blocked,previous;
    sigfillset(&blocked); pthread_sigmask(SIG_BLOCK,&blocked,&previous);
    int error=pthread_create(&file_worker,NULL,observe_loop,NULL);
    if(!error) worker_started=true;
    pthread_sigmask(SIG_SETMASK,&previous,NULL);
    if(error) { sem_destroy(&file_wake); elog(ERROR,"cannot start cache observer: %s",strerror(error)); }
    RegisterXactCallback(transaction_end,NULL);
    previous_start=ExecutorStart_hook; ExecutorStart_hook=start;
    previous_run=ExecutorRun_hook; ExecutorRun_hook=run;
    previous_finish=ExecutorFinish_hook; ExecutorFinish_hook=finish;
    previous_end=ExecutorEnd_hook; ExecutorEnd_hook=end;
}
