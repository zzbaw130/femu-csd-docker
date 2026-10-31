/* PG backend reads the prepared schema/extent mapping directly. */
#include "pg_internal.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "access/relation.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/rel.h"
#include "utils/memutils.h"
#include "csd-cache.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
/* The same query memory context owns the extent array and this registration.
 * Closing the descriptor removes only its own mappings, including on ERROR. */
struct cache_registration { MemoryContextCallback cleanup; int fd; };
static void close_cache_registration(void *arg)
{
    struct cache_registration *r=arg;
    if(r->fd>=0) { close(r->fd); r->fd=-1; }
}
static void register_cache(const struct qs_request *request,const struct qs_extent *extents)
{
    pg_sina_dcp_register(request,extents);
    if(!pg_sina_hpcp_enabled() || !request->nextents) return;
    struct cache_registration *r=palloc0(sizeof(*r));
    r->fd=open("/dev/femu-cache",O_RDWR|O_CLOEXEC);
    if(r->fd<0) ereport(ERROR,(errmsg("cannot open HPCP device: %m")));
    r->cleanup=(MemoryContextCallback){.func=close_cache_registration,.arg=r};
    MemoryContextRegisterResetCallback(CurrentMemoryContext,&r->cleanup);
    Relation relation=relation_open(request->plan.relid,AccessShareLock);
    char *base=relpathperm(relation->rd_node,MAIN_FORKNUM);
    int input=-1; uint64 previous=UINT64_MAX;
    const uint64 segment_bytes=(uint64)RELSEG_SIZE*BLCKSZ;
    for(unsigned i=0;i<request->nextents;i++) {
        const struct qs_extent *e=&extents[i];
        uint64 segment=e->logical/segment_bytes;
        if(segment!=previous) {
            if(input>=0) CloseTransientFile(input);
            char *path=segment?psprintf("%s.%llu",base,(unsigned long long)segment):base;
            input=OpenTransientFile(path,O_RDONLY|PG_BINARY);
            if(input<0) ereport(ERROR,(errmsg("cannot open relation for HPCP: %m")));
            if(segment) pfree(path);
            previous=segment;
        }
        struct femu_cache_mapping mapping={.fd=input,.nsid=request->nsid,
            .lba_bytes=e->lba*request->lba_bytes,.file_offset=e->logical%segment_bytes,.bytes=e->bytes};
        if(ioctl(r->fd,FEMU_CACHE_REGISTER,&mapping))
            ereport(ERROR,(errmsg("cannot register relation HPCP: %m")));
    }
    if(input>=0) CloseTransientFile(input);
    pfree(base); relation_close(relation,AccessShareLock);
}
struct qs_extent *pg_sina_mapping(struct qs_request *request)
{
    FILE *file=fopen("/proc/sys/kernel/random/boot_id","r"); char boot[80];
    if(!file || !fgets(boot,sizeof(boot),file)) elog(ERROR,"cannot read Guest boot ID");
    fclose(file); boot[strcspn(boot,"\n")]=0;
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    Oid types[]={OIDOID,OIDOID,TEXTOID}; Datum values[]={ObjectIdGetDatum(request->plan.relid),ObjectIdGetDatum(request->plan.relfilenode),CStringGetTextDatum(boot)};
    int status=SPI_execute_with_args("SELECT r.device,r.namespace_id,r.lba_bytes,r.bytes,e.logical_offset,e.start_lba,e.bytes "
        "FROM sina_csd.relations r LEFT JOIN sina_csd.extents e ON e.relid=r.relid "
        "WHERE r.relid=$1 AND r.relfilenode=$2 AND r.boot_id=$3 ORDER BY e.logical_offset",3,types,values,NULL,true,0);
    if(status!=SPI_OK_SELECT || !SPI_processed) ereport(ERROR,(errmsg("CSD mapping is missing or stale; prepare the relation again")));
    bool isnull; TupleDesc desc=SPI_tuptable->tupdesc; HeapTuple first=SPI_tuptable->vals[0];
    strlcpy(request->device,SPI_getvalue(first,desc,1),sizeof(request->device));
    request->nsid=DatumGetInt32(SPI_getbinval(first,desc,2,&isnull));
    request->lba_bytes=DatumGetInt32(SPI_getbinval(first,desc,3,&isnull));
    request->bytes=DatumGetInt64(SPI_getbinval(first,desc,4,&isnull));
    request->nextents=request->bytes?SPI_processed:0;
    struct qs_extent *extents=SPI_palloc(Max(request->nextents,1U)*sizeof(*extents)); uint64 covered=0;
    for(unsigned i=0;i<request->nextents;++i) {
        HeapTuple tuple=SPI_tuptable->vals[i];
        extents[i].logical=DatumGetInt64(SPI_getbinval(tuple,desc,5,&isnull));
        extents[i].lba=DatumGetInt64(SPI_getbinval(tuple,desc,6,&isnull));
        extents[i].bytes=DatumGetInt64(SPI_getbinval(tuple,desc,7,&isnull));
        if(extents[i].logical!=covered || !extents[i].bytes || extents[i].bytes>request->bytes-covered)
            elog(ERROR,"incomplete CSD extent mapping");
        covered+=extents[i].bytes;
    }
    if(covered!=request->bytes) elog(ERROR,"incomplete CSD extent mapping");
    SPI_finish();
    register_cache(request,extents);
    return extents;
}
