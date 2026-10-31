/* Guest-only assertion: compare every recorded page with independent PG and
 * mincore observations, without refreshing the extension's snapshot first. */
#include "pg_internal.h"
#include "fmgr.h"
#include "executor/executor.h"
#include "access/relation.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(pg_sina_cache_check);
static RelFileNode checked_node;
static Oid checked_relid;
static bool armed;
static unsigned ending;
static ExecutorEnd_hook_type previous_end;
static bool resident_bit(const unsigned char *bits,uint64 i) { return (bits[i/8]>>(i%8))&1; }
static void check_pages(const char *phase)
{
    const struct pg_sina_cache_entry *entry=pg_sina_cache_lookup(checked_node);
    if(!entry || !entry->file_known) elog(ERROR,"cache snapshot is missing or incomplete");
    unsigned char *actual=palloc0(Max((entry->blocks+7)/8,1));
    uint64 shared=0,file=0,shared_mismatch=0,file_mismatch=0;
    for(int i=0;i<NBuffers;++i) {
        BufferDesc *buffer=GetBufferDescriptor(i);
        uint32 state=LockBufHdr(buffer); BufferTag tag=buffer->tag;
        UnlockBufHdr(buffer,state);
        if((state&(BM_VALID|BM_TAG_VALID))==(BM_VALID|BM_TAG_VALID) &&
           tag.forkNum==MAIN_FORKNUM && RelFileNodeEquals(tag.rnode,checked_node) && tag.blockNum<entry->blocks)
            actual[tag.blockNum/8]|=1U<<(tag.blockNum%8);
    }
    for(uint64 i=0;i<entry->blocks;++i) {
        bool resident=resident_bit(actual,i); shared+=resident;
        shared_mismatch+=resident!=resident_bit(entry->shared_bits,i);
    }
    long os_page=sysconf(_SC_PAGESIZE);
    char *base=relpathperm(checked_node,MAIN_FORKNUM);
    uint64 total=entry->blocks*BLCKSZ,segment_bytes=(uint64)RELSEG_SIZE*BLCKSZ;
    for(uint64 at=0;at<total;) {
        unsigned segment=at/segment_bytes;
        char *path=segment?psprintf("%s.%u",base,segment):base;
        int fd=OpenTransientFile(path,O_RDONLY|PG_BINARY);
        if(fd<0) elog(ERROR,"cannot open cache-check relation: %m");
        size_t bytes=Min(total-at,segment_bytes),pages=(bytes+os_page-1)/os_page;
        unsigned char *vector=palloc(pages);
        void *map=mmap(NULL,bytes,PROT_NONE,MAP_SHARED,fd,0);
        if(map==MAP_FAILED || mincore(map,bytes,vector)) elog(ERROR,"cannot observe cache-check pages: %m");
        for(size_t i=0;i<pages;++i) {
            bool resident=vector[i]&1; file+=resident;
            file_mismatch+=resident!=resident_bit(entry->file_bits,at/os_page+i);
        }
        munmap(map,bytes); CloseTransientFile(fd); pfree(vector);
        if(segment) pfree(path);
        at+=bytes;
    }
    if(shared_mismatch || file_mismatch || shared!=entry->shared_pages || file!=entry->file_pages)
        elog(ERROR,"cache bitmap mismatch: shared=%llu file=%llu counts=%d",
            (unsigned long long)shared_mismatch,(unsigned long long)file_mismatch,
            shared!=entry->shared_pages || file!=entry->file_pages);
    ereport(NOTICE,(errmsg("CACHE_CHECK phase=%s generation=%u shared_pages=%llu file_pages=%llu shared_mismatches=0 file_mismatches=0",
        phase,entry->generation,(unsigned long long)shared,(unsigned long long)file)));
    pfree(actual); pfree(base);
}

static void check_end(QueryDesc *query)
{
    bool target=false;
    ListCell *cell;
    foreach(cell,query->plannedstmt->rtable) {
        RangeTblEntry *rte=lfirst(cell);
        if(rte->rtekind==RTE_RELATION && rte->relid==checked_relid) target=true;
    }
    bool outer=!ending && target;
    if(outer && armed && pg_sina_dcp &&
       (pg_sina_exe_path==PG_SINA_CSD || pg_sina_exe_path==PG_SINA_HPCP)) check_pages("before_end");
    ++ending;
    if(previous_end) previous_end(query); else standard_ExecutorEnd(query);
    --ending;
    if(outer && armed) check_pages("after_end");
}
void _PG_init(void);
void _PG_init(void)
{
    previous_end=ExecutorEnd_hook; ExecutorEnd_hook=check_end;
}
Datum pg_sina_cache_check(PG_FUNCTION_ARGS)
{
    Relation rel=relation_open(PG_GETARG_OID(0),AccessShareLock);
    checked_node=rel->rd_node; checked_relid=RelationGetRelid(rel); armed=true;
    relation_close(rel,AccessShareLock);
    PG_RETURN_TEXT_P(cstring_to_text("armed"));
}
