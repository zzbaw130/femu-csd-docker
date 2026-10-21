/* PG backend submits SNIA batches directly; all SQL data operators run in CSFs. */
#include "pg_internal.h"
#include "miscadmin.h"
#include "cs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>
#include <time.h>
enum { SCAN,FILTER,SINK,SOURCE,SORT,LIMIT,JOIN_BUILD,JOIN_LOAD,JOIN_CLOSE,FUNCTIONS };
/* One SERIAL batch and one reusable set of buffers per execution slot. */
struct slot { CS_BATCH_HANDLE batch,resume; CS_REQ_HANDLE request; CS_BATCH_INDEX load; unsigned bytes; };
struct program { const char *file,*symbol; CS_CSF_ID function; unsigned index; int loaded,active; };
struct pg_sina_stream {
    CS_DEV_HANDLE device;
    CS_BATCH_HANDLE batch;
    CS_MEM_HANDLE output,group,io,counter,dict;
    struct program source;
    int lock_fd;
    bool profiling;
    uint64_t start,fdm_bytes;
    struct qs_profile profile,last_counter;
    struct qs_metrics join_metrics;
};
static uint64_t clock_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static CS_STATUS copy(CS_MEM_HANDLE memory,unsigned offset,void *data,int bytes,int to_device)
{
    CsCopyMemRequest request={.Type=to_device?CS_COPY_TO_DEVICE:CS_COPY_FROM_DEVICE,.DevMem={memory,offset},.u.HostVAddress=data,.Bytes=bytes};
    return csQueueCopyMemRequest(&request,NULL,NULL,NULL,NULL,NULL);
}
static CS_STATUS load_program(CS_DEV_HANDLE device,struct program *p)
{
    char descriptor[128]; size_t size=strlen(p->file)+1;
    memcpy(descriptor,p->file,size); strcpy(descriptor+size,p->symbol);
    CsCSFDownloadInfo download={.Type=CS_CSF_VENDOR_SPECIFIC,.SubType=CS_SUBTYPE_VENDOR_SPECIFIC,.CSEEId=1,.Index=p->index};
    snprintf(download.common.UniqueName,sizeof(download.common.UniqueName),"pg-sina-%u",p->index);
    download.common.DataBuffer=descriptor; download.common.Length=size+strlen(p->symbol)+1;
    u32 id; CS_STATUS s=csCSFDownload(device,&download,&id); if(s!=CS_SUCCESS) return s; p->loaded=1;
    CsConfigInfo config={.Type=CS_CSF_ACTIVATE,.u.CSFActivateInfo={.CSFId=p->index,.EEIId=1,.State=CS_ACTIVE_STATE}};
    CsConfigData result; int length=sizeof(result); s=csConfig(device,&length,&config,&result); if(s!=CS_SUCCESS) return s; p->active=1;
    int count=0; length=0; s=csGetCSFId(device,download.common.UniqueName,0,&length,&count,NULL); if(s!=CS_LENGTH_RETURNED) return s;
    CSFIdInfo *functions=malloc(length); if(!functions) return CS_OUT_OF_RESOURCES;
    s=csGetCSFId(device,download.common.UniqueName,0,&length,&count,functions); if(s==CS_SUCCESS) p->function=functions[0].CSFId;
    free(functions); return s;
}
static void unload_program(CS_DEV_HANDLE device,struct program *p)
{
    if(p->active) { CsConfigInfo c={.Type=CS_CSF_ACTIVATE,.u.CSFActivateInfo={.CSFId=p->index,.EEIId=1,.State=CS_INACTIVE_STATE}}; CsConfigData r; int len=sizeof(r); csConfig(device,&len,&c,&r); }
    if(p->loaded) { CsCSFDownloadInfo d={.CSEEId=1,.Index=p->index,.common.Unload=1};u32 id;csCSFDownload(device,&d,&id); }
}
static CsProperties *properties(CS_DEV_HANDLE device,CS_RESOURCE_TYPE type)
{
    int length=0; if(csQueryDeviceProperties(device,type,&length,NULL)!=CS_LENGTH_RETURNED) return NULL;
    CsProperties *p=malloc(length); if(!p) return NULL;
    if(csQueryDeviceProperties(device,type,&length,p)!=CS_SUCCESS) { free(p); return NULL; } return p;
}
static void compute_request(CsBatchRequest *b,CS_CSF_ID function,CS_MEM_HANDLE *memories,unsigned count,CS_MEM_HANDLE counters)
{
    memset(b,0,sizeof(*b)+8*sizeof(CsComputeArg)); b->ReqType=CS_QUEUE_COMPUTE;
    b->u.Compute.CSFId=function; b->u.Compute.NumArgs=count;
    for(unsigned i=0;i<count;++i) csHelperSetComputeArg(&b->u.Compute.Args[i],CS_AFDM_TYPE,memories[i],0UL);
    if(counters) {
        csHelperSetComputeArg(&b->u.Compute.Args[count],CS_AFDM_TYPE,counters,0UL);
        csHelperSetComputeArg(&b->u.Compute.Args[count+1],CS_64BIT_VALUE_TYPE,(u64)1);
        b->u.Compute.NumArgs+=2;
    }
}
/* The batch scheduler uses the last AFDM for spill I/O. Put the optional
 * profiling buffer before it; the device entry wrapper removes that buffer. */
static void compute_io_request(CsBatchRequest *b,CS_CSF_ID function,CS_MEM_HANDLE *memory,unsigned count,CS_MEM_HANDLE counter)
{
    CS_MEM_HANDLE args[8]; unsigned n=0;
    for(unsigned i=0;i<count;++i) {
        if(i+1==count && counter) args[n++]=counter;
        args[n++]=memory[i];
    }
    compute_request(b,function,args,n,NULL);
    if(counter) {
        csHelperSetComputeArg(&b->u.Compute.Args[n],CS_64BIT_VALUE_TYPE,(u64)1);
        ++b->u.Compute.NumArgs;
    }
}
static uint64_t stage_build_bytes(const struct qs_plan *p,unsigned stage)
{
    const struct qs_join *j=qs_join_at(p,stage);
    unsigned base=qs_probe_attrs(p);
    for(unsigned i=0;i<stage;++i) base+=qs_join_at(p,i)->nattrs;
    unsigned cols=0;
    for(unsigned i=base;i<base+j->nattrs;++i) cols+=p->cols[i].slot>=0;
    unsigned stride=(16+cols*9+7)&~7U;
    uint64_t rows=j->rows?j->rows:1,capacity=256;
    while(capacity<rows && capacity<UINT32_MAX/2) capacity*=2;
    return capacity*(stride+8)+1024*1024;
}
static uint64_t stage_limit_bytes(const struct qs_plan *p,unsigned stage,unsigned batches)
{
    const struct qs_join *j=qs_join_at(p,stage);
    unsigned base=qs_probe_attrs(p); for(unsigned i=0;i<stage;++i) base+=qs_join_at(p,i)->nattrs;
    unsigned cols=0; for(unsigned i=base;i<base+j->nattrs;++i) cols+=p->cols[i].slot>=0;
    unsigned stride=(16+cols*9+7)&~7U;
    uint64_t rows=j->rows?j->rows:1,target=(rows+batches-1)/batches,capacity=256;
    while(capacity<target && capacity<UINT32_MAX/2) capacity*=2;
    return capacity*(stride+8)+1024*1024;
}
/* Physical side scans only project that relation's required columns. */
static void join_scan_plan(const struct qs_plan *combined,struct qs_plan *scan,bool build)
{
    *scan=*combined; unsigned base=build?combined->nattrs-combined->join.nattrs:0;
    scan->nattrs=build?combined->join.nattrs:qs_probe_attrs(combined); scan->ncols=0;
    if(build) { scan->relid=combined->join.relid; scan->relfilenode=combined->join.relfilenode; }
    scan->predicate=build?combined->join.predicate:combined->predicate;
    scan->njoins=0; scan->join.relid=0; scan->join.residual=0;
    for(unsigned i=0;i<scan->nattrs;++i) {
        scan->cols[i]=combined->cols[base+i];
        if(scan->cols[i].slot>=0) scan->cols[i].slot=scan->ncols++;
    }
    for(unsigned i=0;i<scan->nexprs;++i) if(scan->exprs[i].op==Q_COLUMN) {
        int col=scan->exprs[i].column;
        scan->exprs[i].column=col>=(int)base && col<(int)(base+scan->nattrs)?col-(int)base:-1;
    }
    for(unsigned i=0;i<scan->nstrings;++i) {
        unsigned col=scan->strings[i].column;
        scan->strings[i].column=col>=base && col<base+scan->nattrs?col-base:255;
    }
}
/* Append physical ranges in destination order; adjacent pages become one
 * internal read rather than one NVMe command per 8KiB page. */
static void append_ranges(CsBlockIo *io,const struct qs_request *request,const struct qs_extent *extents,uint64_t cursor,unsigned bytes)
{
    uint64_t at=cursor,end=cursor+bytes;
    for(unsigned j=0;j<request->nextents && at<end;++j) {
        const struct qs_extent *e=&extents[j]; if(e->logical+e->bytes<=at) continue;
        uint64_t length=(e->logical+e->bytes<end?e->logical+e->bytes:end)-at;
        CSBlockRange range={request->nsid,e->lba+(at-e->logical)/request->lba_bytes,length/request->lba_bytes};
        CSBlockRange *last=io->NumRanges?&io->Range[io->NumRanges-1]:NULL;
        if(last && last->NamespaceId==range.NamespaceId && last->StartLba+last->NumBlocks==range.StartLba) last->NumBlocks+=range.NumBlocks;
        else io->Range[io->NumRanges++]=range;
        at+=length;
    }
}
static void build_storage_request(CsBatchRequest *load,CS_DEV_HANDLE device,CS_MEM_HANDLE memory,
    const struct qs_request *request,const struct qs_extent *extents,uint64_t cursor,unsigned bytes)
{
    memset(load,0,sizeof(*load)); load->ReqType=CS_STORAGE_IO;
    load->u.StorageIo=(CsStorageRequest){.Mode=CS_STORAGE_BLOCK_IO,.DevHandle=device,
        .u.BlockIo={.Type=CS_STORAGE_LOAD_TYPE,.DevMem={memory,0}}};
    append_ranges(&load->u.StorageIo.u.BlockIo,request,extents,cursor,bytes);
}
static void drop_source(CS_CSF_ID source,CS_MEM_HANDLE group,CsBatchRequest *compute)
{
    CS_MEM_HANDLE memory[]={group}; compute_request(compute,source,memory,1,NULL);
    csQueueComputeRequest(&compute->u.Compute,NULL,NULL,NULL,NULL,NULL);
}
/* Aggregate.source refreshes its own counters on every result page. Retain
 * build/probe totals separately and add build spill to those aggregate totals. */
static void join_metrics(struct qs_metrics *out,const struct qs_metrics *join)
{
    if(!join->build_batches) return;
    out->scanned=join->scanned; out->matched=join->matched; out->chunks=join->chunks; out->input_bytes=join->input_bytes;
    out->index_pages=join->index_pages; out->index_tuples=join->index_tuples;
    out->build_rows=join->build_rows; out->build_batches=join->build_batches; out->join_rows=join->join_rows;
    out->spill_write_bytes+=join->spill_write_bytes; out->spill_read_bytes+=join->spill_read_bytes;
}
#define TRY(call) do { CS_STATUS status=(call); if(status!=CS_SUCCESS) { \
    result->status=status; snprintf(result->error,sizeof(result->error),"%s: status=%d",#call,status); goto cleanup; } } while(0)
void pg_sina_execute(struct qs_request *request,struct qs_extent *extents,struct qs_result *result,bool profiling,struct pg_sina_stream **stream)
{
    uint64_t start=profiling?clock_ns():0,phase=start,cleanup_start=0;
    struct qs_profile stats={0};
    CHECK_FOR_INTERRUPTS();
    bool joining=request->plan.join.relid!=0;
    unsigned njoins=qs_join_count(&request->plan);
    bool indexed=njoins && qs_join_at(&request->plan,njoins-1)->indexid;
    struct qs_request builds[QS_JOINS]={0}; struct qs_extent *build_extents[QS_JOINS]={0};
    struct qs_request index_request={0}; struct qs_extent *index_extents=NULL;
    unsigned max_extents=request->nextents; uint64_t build_bytes=0;
    struct qs_plan probe_plan={0},build_plan={0};
    for(unsigned i=0;i<njoins;++i) {
        const struct qs_join *j=qs_join_at(&request->plan,i); struct qs_request *b=&builds[i];
        b->plan.relid=j->relid; b->plan.relfilenode=j->relfilenode;
        build_extents[i]=pg_sina_mapping(b); max_extents=Max(max_extents,b->nextents); if(!j->indexid) build_bytes+=b->bytes;
        if(strcmp(b->device,request->device) || b->nsid!=request->nsid || b->lba_bytes!=request->lba_bytes)
            elog(ERROR,"CSD joins require all relations in the same namespace");
    }
    if(indexed) {
        const struct qs_join *j=qs_join_at(&request->plan,njoins-1);
        index_request.plan.relid=j->indexid; index_request.plan.relfilenode=j->indexnode;
        index_extents=pg_sina_mapping(&index_request); max_extents=Max(max_extents,index_request.nextents);
        if(strcmp(index_request.device,request->device) || index_request.nsid!=request->nsid || index_request.lba_bytes!=request->lba_bytes) elog(ERROR,"CSD index and heap must share a namespace");
    }
    bool streaming=request->plan.ordered_rows || request->plan.external_aggregate;
    struct pg_sina_stream *saved=streaming?calloc(1,sizeof(*saved)):NULL;
    if(streaming && !saved) elog(ERROR,"cannot allocate CSD result stream");
    *stream=NULL;
    /* Do not longjmp through SNIA C++ frames or its batch threads. Poll PG's
     * pending cancel between event polls, then release resources before PG
     * reports the interrupt. No PG allocations/errors occur while held. */
    HOLD_INTERRUPTS();
    memset(result,0,sizeof(*result));
    result->status=1; strcpy(result->error,"CSD execution interrupted or initialization failed");
    CS_DEV_HANDLE device=NULL; CS_BATCH_HANDLE batch=NULL,build_batch=NULL,load_batch=NULL; CS_EVT_HANDLE event=NULL;
    struct slot slots[QS_WORKERS]={{0}}; unsigned inflight=0;
    CS_MEM_HANDLE plan_mem=NULL,out_mem=NULL,group_mem=NULL,scan_mem=NULL,join_mem=NULL,build_mem=NULL,dict_mem=NULL;
    CS_MEM_HANDLE joined[QS_WORKERS]={0},controls_mem[QS_JOINS]={0};
    struct qs_join_control join_control[QS_JOINS]={{0}};
    struct qs_metrics join_totals={0};
    unsigned build_chunks=0;
    CS_MEM_HANDLE input[QS_WORKERS]={0},vectors[QS_WORKERS]={0},tasks[QS_WORKERS]={0},counters[QS_WORKERS]={0};
    CsProperties *caps=NULL,*fdm=NULL;
    CsBatchRequest *load=NULL,*compute=NULL;
    struct qs_group group={0}; int group_ready=0,lock_fd=-1;
    unsigned workers=0,chunk=QS_CHUNK;
    struct program programs[FUNCTIONS]={
        [SCAN]={.file="pg-sina-scan.so",.symbol="pg_sina_scan",.index=501},
        [FILTER]={.file="pg-sina-filter.so",.symbol="pg_sina_filter",.index=502},
        [SINK]={.file="pg-sina-aggregate.so",.symbol="pg_sina_aggregate_sink",.index=503},
        [SOURCE]={.file="pg-sina-aggregate.so",.symbol="pg_sina_aggregate_source",.index=504},
        [SORT]={.file="pg-sina-sort.so",.symbol="pg_sina_sort",.index=505},
        [LIMIT]={.file="pg-sina-limit.so",.symbol="pg_sina_limit",.index=506},
        [JOIN_BUILD]={.file="pg-sina-join.so",.symbol="pg_sina_join_build",.index=507},
        [JOIN_LOAD]={.file="pg-sina-join.so",.symbol="pg_sina_join_load",.index=508},
        [JOIN_CLOSE]={.file="pg-sina-join.so",.symbol="pg_sina_join_close",.index=509}};
    if(joining) { programs[SINK].file="pg-sina-join.so"; programs[SINK].symbol="pg_sina_join_probe"; }
    unsigned scan_cols=request->plan.ncols,joined_bytes=njoins*qs_join_chunk_stride(&request->plan);
    if(joining) {
        join_scan_plan(&request->plan,&probe_plan,false); scan_cols=probe_plan.ncols;
        unsigned base=qs_probe_attrs(&request->plan);
        for(unsigned i=0;i<njoins;++i) {
            struct qs_plan view=request->plan; view.join=*qs_join_at(&request->plan,i); view.njoins=1;
            base+=view.join.nattrs; view.nattrs=base;
            join_scan_plan(&view,&build_plan,true); scan_cols=Max(scan_cols,build_plan.ncols);
        }
    }
    if(request->plan.ordered_rows) {
        programs[SINK].file=programs[SOURCE].file="pg-sina-sort.so";
        programs[SINK].symbol="pg_sina_sort_sink"; programs[SOURCE].symbol="pg_sina_sort_source";
    }
    lock_fd=open(request->device,O_RDONLY|O_CLOEXEC);
    if(lock_fd<0) { result->status=1; snprintf(result->error,sizeof(result->error),"cannot lock namespace: %s",strerror(errno)); goto cleanup; }
    while(flock(lock_fd,LOCK_EX|LOCK_NB)) {
        if((errno!=EWOULDBLOCK && errno!=EINTR) || QueryCancelPending || ProcDiePending) goto cleanup;
        pg_usleep(100000L);
    }
    TRY(csOpenCSx(request->device,NULL,&device));
    caps=properties(device,CS_CSE_TYPE); fdm=properties(device,CS_FDM_TYPE);
    if(!caps || !fdm) { result->status=1; strcpy(result->error,"resource query failed"); goto cleanup; }
    workers=caps->CSEDetails[0].CSE[0].NumCRs;
    if(workers>QS_WORKERS) workers=QS_WORKERS;
    if(!workers) goto cleanup;
    uint64_t capacity=fdm->FDMDetails[0].FDM[0].FDMSize;
    unsigned dict_entries=request->plan.dict_big?QS_DICT_BIG_ENTRIES:QS_DICT_SMALL_ENTRIES;
    unsigned dict_data=request->plan.dict_big?QS_DICT_BIG_DATA:QS_DICT_SMALL_DATA;
    size_t dict_size=request->plan.runtime_dict?qs_dict_bytes(dict_entries,dict_data):0;
    while((uint64_t)workers*(chunk+((qs_chunk_bytes(chunk/24,scan_cols)+4095)&~(size_t)4095)+4096+(profiling?4096:0)+((joined_bytes+4095)&~(size_t)4095))+
          ((sizeof(struct qs_result)+4095)&~(size_t)4095)+((sizeof(struct qs_plan)+4095)&~(size_t)4095)+4096+
          ((dict_size+4095)&~(size_t)4095)+
          (joining?2*((sizeof(struct qs_plan)+4095)&~(size_t)4095)+(njoins+1)*4096:0)>capacity) {
        /* Large dictionaries dominate the FDM. Shrink the per-slot column
         * vectors before dropping CU parallelism; more workers still win for
         * the scan/join pipeline. Small dictionaries reduce workers first. */
        if(dict_size>(size_t)capacity/4 && chunk>1024*1024U) chunk/=2;
        else if(workers>1) --workers;
        else if(chunk>QS_PAGE) chunk/=2;
        else { result->status=1; strcpy(result->error,"insufficient FDM"); goto cleanup; }
    }
    if(indexed && chunk<QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE) { strcpy(result->error,"insufficient AFDM for index pages"); goto cleanup; }
    uint64_t nchunks=(request->bytes+chunk-1)/chunk;
    /* Keep room for grouped state and spill buffers (1 MiB), or the larger
     * expression scratch retained alongside a residual join filter (2 MiB). */
    if(joining && (request->plan.external_aggregate || request->plan.join.residual))
        while(workers>1 && (uint64_t)pg_sina_max_memory*1024/2/workers<(request->plan.join.residual?2:1)*1024*1024) --workers;
    if(nchunks && workers>nchunks) workers=nchunks;
    /* One budget shared by all concurrent slots, including Filter scratch.
     * CSFs also clamp each share to the physical workmem pool capacity. */
    uint64_t query_budget=(uint64_t)pg_sina_max_memory*1024;
    uint64_t build_budget=joining?query_budget/2:0;
    if(joining) {
        uint64_t estimated=0;
        for(unsigned i=0;i<njoins;++i) if(!qs_join_at(&request->plan,i)->indexid)
            estimated+=stage_build_bytes(&request->plan,i);
        if(estimated>build_budget) {
            uint64_t reserve=Max((uint64_t)4*1024*1024,query_budget/16);
            uint64_t candidate=query_budget>reserve?query_budget-reserve:query_budget/2;
            if(candidate>build_budget) build_budget=Min(candidate,estimated);
        }
    }
    if(build_budget>query_budget) build_budget=query_budget;
    request->plan.join.work_mem_bytes=build_budget;
    request->plan.work_mem_bytes=(query_budget-build_budget)/workers;
    if(joining) {
        join_scan_plan(&request->plan,&probe_plan,false);
    }
    group.count=workers;
    bool has_filter=request->plan.predicate>=0;
    for(unsigned i=0;i<njoins;++i) has_filter=has_filter || qs_join_at(&request->plan,i)->predicate>=0;
    for(unsigned i=0;i<FUNCTIONS;++i) {
        if((!joining && i>=JOIN_BUILD) || (streaming && (i==SORT || i==LIMIT)) ||
            (i==FILTER && !has_filter) || (i==SORT && !request->plan.norder) ||
            (i==LIMIT && request->plan.limit<0 && !request->plan.offset)) continue;
        TRY(load_program(device,&programs[i]));
    }
    CsMemFlags flags={.FDMId=1};
    TRY(csAllocMem(device,sizeof(request->plan),&flags,&plan_mem,NULL));
    TRY(copy(plan_mem,0,&request->plan,sizeof(request->plan),1));
    if(joining) {
        TRY(csAllocMem(device,sizeof(request->plan),&flags,&scan_mem,NULL));
        TRY(csAllocMem(device,sizeof(request->plan),&flags,&build_mem,NULL));
        flags.Flags=CS_FDM_CLEAR; TRY(csAllocMem(device,njoins*sizeof(join_control[0]),&flags,&join_mem,NULL));
        for(unsigned i=0;i<njoins;++i) TRY(csAllocMem(device,sizeof(join_control[0]),&flags,&controls_mem[i],NULL));
        flags.Flags=0;
    }
    TRY(csAllocMem(device,sizeof(*result),&flags,&out_mem,NULL));
    TRY(csAllocMem(device,sizeof(group),&flags,&group_mem,NULL));
    if(request->plan.runtime_dict) {
        flags.Flags=0;
        TRY(csAllocMem(device,dict_size,&flags,&dict_mem,NULL));
        struct qs_dict header={.lock=0,.count=0,.bytes=0,.entries=dict_entries,.capacity=dict_entries,.data=dict_data};
        TRY(copy(dict_mem,0,&header,sizeof(header),1));
        /* Only the open-addressing bucket array must start zeroed. Use small
         * host copies instead of a large FDM fill command; the 1MiB transport
         * chunk path is not reliable for this multi-megabyte initialization. */
        size_t bucket_offset=sizeof(struct qs_dict)+(size_t)dict_entries*sizeof(struct qs_dict_entry);
        size_t bucket_bytes=(size_t)dict_entries*sizeof(uint32_t), zero_chunk=64U*1024U;
        void *zeros=calloc(1,zero_chunk);
        if(!zeros) { result->status=1; strcpy(result->error,"cannot allocate dictionary zero buffer"); goto cleanup; }
        for(size_t at=0;at<bucket_bytes;at+=zero_chunk) {
            size_t n=bucket_bytes-at<zero_chunk?bucket_bytes-at:zero_chunk;
            TRY(copy(dict_mem,bucket_offset+at,zeros,n,1));
        }
        free(zeros);
    }
    for(unsigned i=0;i<workers;++i) {
        TRY(csAllocMem(device,chunk,&flags,&input[i],NULL));
        TRY(csAllocMem(device,qs_chunk_bytes(chunk/24,scan_cols),&flags,&vectors[i],NULL));
        struct qs_chunk header={.capacity=chunk/24,.ncols=joining?probe_plan.ncols:request->plan.ncols}; TRY(copy(vectors[i],0,&header,sizeof(header),1));
        flags.Flags=CS_FDM_CLEAR; TRY(csAllocMem(device,sizeof(struct qs_task),&flags,&tasks[i],NULL)); flags.Flags=0;
        struct qs_task task={.workers=workers,.slot=i}; TRY(copy(tasks[i],0,&task,sizeof(task),1));
        if(joining) {
            TRY(csAllocMem(device,joined_bytes,&flags,&joined[i],NULL));
            struct qs_chunk header={.capacity=256,.ncols=request->plan.ncols}; for(unsigned j=0;j<njoins;++j) TRY(copy(joined[i],j*qs_join_chunk_stride(&request->plan),&header,sizeof(header),1));
        }
        if(profiling) {
            flags.Flags=CS_FDM_CLEAR;
            TRY(csAllocMem(device,sizeof(stats),&flags,&counters[i],NULL)); flags.Flags=0;
        }
    }
    load=calloc(1,sizeof(*load)+(max_extents+1)*QS_INDEX_READS*sizeof(CSBlockRange));
    compute=calloc(1,sizeof(*compute)+8*sizeof(CsComputeArg));
    if(!load || !compute) { result->status=1; strcpy(result->error,"host request allocation failed"); goto cleanup; }
    TRY(csCreateEvent(&event));
    uint64_t cursor=0; bool empty_pending=!request->bytes;
    if(profiling) { uint64_t now=clock_ns(); stats.setup_ns=now-phase; phase=now; }
    if(joining) {
        uint64_t weights[QS_JOINS]={0},weight_sum=0,build_budget=request->plan.join.work_mem_bytes;
        unsigned indexed_stages=0;
        for(unsigned i=0;i<njoins;++i) {
            if(qs_join_at(&request->plan,i)->indexid) { ++indexed_stages; continue; }
            weights[i]=stage_build_bytes(&request->plan,i); weight_sum+=weights[i];
        }
        uint64_t index_budget=indexed_stages?QS_INDEX_WORK_BYTES*indexed_stages:0;
        uint64_t hash_budget=build_budget>index_budget?build_budget-index_budget:0;
        uint64_t limits[QS_JOINS]={0};
        bool tuned=false;
        for(unsigned batches=1;batches<=8 && !tuned;++batches) {
            uint64_t total=0;
            for(unsigned i=0;i<njoins;++i) if(!qs_join_at(&request->plan,i)->indexid)
                total+=stage_limit_bytes(&request->plan,i,batches);
            if(total<=hash_budget) {
                for(unsigned i=0;i<njoins;++i) if(!qs_join_at(&request->plan,i)->indexid)
                    limits[i]=stage_limit_bytes(&request->plan,i,batches);
                tuned=true;
            }
        }
        uint64_t assigned=0;
        unsigned base=qs_probe_attrs(&request->plan);
        for(unsigned stage=0;stage<njoins;++stage) {
            struct qs_request *build=&builds[stage];
            struct qs_plan view=request->plan; view.join=*qs_join_at(&request->plan,stage); view.njoins=njoins;
            base+=view.join.nattrs; view.nattrs=base; view.join.residual=0;
            if(view.join.indexid) view.join.work_mem_bytes=QS_INDEX_WORK_BYTES;
            else if(tuned) view.join.work_mem_bytes=limits[stage];
            else if(weight_sum) {
                uint64_t share=hash_budget*weights[stage]/weight_sum;
                if(share<1024*1024U) share=hash_budget?Min((uint64_t)(1024*1024U),hash_budget):0;
                if(assigned+share>hash_budget) share=hash_budget>assigned?hash_budget-assigned:0;
                view.join.work_mem_bytes=share; assigned+=share;
            } else view.join.work_mem_bytes=njoins?hash_budget/njoins:hash_budget;
            if(view.join.indexid) {
                TRY(copy(build_mem,0,&view,sizeof(view),1));
                CS_MEM_HANDLE memory[]={vectors[0],build_mem,controls_mem[stage],input[0]};
                compute_io_request(compute,programs[JOIN_BUILD].function,memory,4,counters[0]);
                TRY(csQueueComputeRequest(&compute->u.Compute,NULL,NULL,NULL,NULL,NULL));
                continue;
            }
            join_scan_plan(&view,&build_plan,true);
            TRY(copy(build_mem,0,&view,sizeof(view),1)); TRY(copy(scan_mem,0,&build_plan,sizeof(build_plan),1));
            for(unsigned i=0;i<workers;++i) {
                struct qs_chunk header={.capacity=chunk/24,.ncols=build_plan.ncols}; TRY(copy(vectors[i],0,&header,sizeof(header),1));
            }
            /* Parallel scan/filter slots feed one build writer. Other slots may
             * compute while it appends or spills, but cannot mutate the hash state. */
            TRY(csAllocBatchRequest(CS_BATCH_SERIAL,1,&build_batch));
            CS_BATCH_INDEX current=0,append_index=0;
            uint64_t at=0; bool empty_build=!build->bytes,first_build=true;
            for(;;) {
                if(QueryCancelPending || ProcDiePending) goto cleanup;
                for(unsigned i=0;i<workers;++i) {
                    struct slot *slot=&slots[i];
                    if(slot->request || (at>=build->bytes && !empty_build)) continue;
                    empty_build=false;
                    unsigned bytes=build->bytes-at<chunk?build->bytes-at:chunk;
                    if(bytes!=slot->bytes) {
                        TRY(copy(tasks[i],offsetof(struct qs_task,input_bytes),&bytes,sizeof(bytes),1));
                        slot->bytes=bytes;
                    }
                    bool first=!slot->batch;
                    if(first) TRY(csAllocBatchRequest(CS_BATCH_SERIAL,3,&slot->batch));
                    if(bytes) {
                        build_storage_request(load,device,input[i],build,build_extents[stage],at,bytes);
                        if(first) TRY(csConfigureBatchEntry(slot->batch,CS_BATCH_ADD,load,0,0,&slot->load));
                        else TRY(csConfigureBatchEntry(slot->batch,CS_BATCH_RECONFIG,load,slot->load,0,NULL));
                    }
                    if(first) {
                        CS_BATCH_INDEX previous=slot->load;
                        CS_MEM_HANDLE scan[]={input[i],scan_mem,vectors[i],tasks[i],dict_mem}; compute_request(compute,programs[SCAN].function,scan,dict_mem?5:4,counters[i]);
                        TRY(csConfigureBatchEntry(slot->batch,CS_BATCH_ADD,compute,0,previous,&current)); previous=current;
                        if(build_plan.predicate>=0) {
                            CS_MEM_HANDLE filter[]={vectors[i],scan_mem,input[i]}; compute_request(compute,programs[FILTER].function,filter,3,counters[i]);
                            TRY(csConfigureBatchEntry(slot->batch,CS_BATCH_ADD,compute,0,previous,&current));
                        }
                    }
                    CS_STATUS status=csQueueBatchRequest(slot->batch,slot,NULL,event,&slot->request,NULL);
                    if(status!=CS_QUEUED) {
                        result->status=status==CS_SUCCESS?CS_ERROR_IN_EXECUTION:status;
                        snprintf(result->error,sizeof(result->error),"csQueueBatchRequest(build scan): status=%d",status); goto cleanup;
                    }
                    ++inflight; at+=bytes;
                }
                if(!inflight) break;
                void *completed=NULL;
                CS_STATUS status=csPollEvent(event,&completed,NULL);
                if(status==CS_NOT_DONE) { pg_usleep(50L); continue; }
                --inflight; struct slot *slot=completed; slot->request=NULL;
                if(status!=CS_SUCCESS) {
                    result->status=status; snprintf(result->error,sizeof(result->error),"csPollEvent(build scan): status=%d",status); goto cleanup;
                }
                unsigned i=slot-slots;
                CS_MEM_HANDLE memory[]={vectors[i],build_mem,controls_mem[stage],input[i]}; compute_io_request(compute,programs[JOIN_BUILD].function,memory,4,counters[i]);
                /* A one-entry batch retains SNIA's automatic NEED_STORE handling. */
                if(first_build) TRY(csConfigureBatchEntry(build_batch,CS_BATCH_ADD,compute,0,0,&append_index));
                else TRY(csConfigureBatchEntry(build_batch,CS_BATCH_RECONFIG,compute,append_index,0,NULL));
                first_build=false;
                TRY(csQueueBatchRequest(build_batch,NULL,NULL,NULL,NULL,NULL)); ++build_chunks;
            }
            TRY(csFreeBatchRequest(build_batch)); build_batch=NULL;
            for(unsigned i=0;i<workers;++i) {
                if(slots[i].batch) TRY(csFreeBatchRequest(slots[i].batch));
                slots[i]=(struct slot){0};
                unsigned bytes=0; TRY(copy(tasks[i],offsetof(struct qs_task,input_bytes),&bytes,sizeof(bytes),1));
            }
        } /* Every build uses its own workmem share and control record. */
        TRY(copy(scan_mem,0,&probe_plan,sizeof(probe_plan),1));
        for(unsigned i=0;i<workers;++i) {
            struct qs_chunk header={.capacity=chunk/24,.ncols=probe_plan.ncols}; TRY(copy(vectors[i],0,&header,sizeof(header),1));
        }
        TRY(csAllocBatchRequest(CS_BATCH_SERIAL,1,&load_batch));
        CS_MEM_HANDLE memory[]={controls_mem[0],input[0]}; compute_io_request(compute,programs[JOIN_LOAD].function,memory,2,counters[0]);
        CS_BATCH_INDEX current;
        TRY(csConfigureBatchEntry(load_batch,CS_BATCH_ADD,compute,0,0,&current));
        if(profiling) { uint64_t now=clock_ns(); stats.build_ns=now-phase; phase=now; }
    }
    uint64_t passes=1;
    if(joining) {
        /* Finalize/load every first build batch before computing the product.
         * Later combinations only replace tables whose batch index changed. */
        for(unsigned i=0;i<njoins;++i) {
            CS_MEM_HANDLE memory[]={controls_mem[i],input[0]}; compute_io_request(compute,programs[JOIN_LOAD].function,memory,2,counters[0]);
            TRY(csConfigureBatchEntry(load_batch,CS_BATCH_RECONFIG,compute,1,0,NULL));
            TRY(csQueueBatchRequest(load_batch,NULL,NULL,NULL,NULL,NULL));
            TRY(copy(controls_mem[i],0,&join_control[i],sizeof(join_control[0]),0));
        }
        TRY(copy(join_mem,0,join_control,njoins*sizeof(join_control[0]),1));
        for(unsigned i=0;i<njoins;++i) {
            unsigned n=join_control[i].batches;
            if(!n || passes>UINT64_MAX/n) { result->status=1; strcpy(result->error,"invalid CSD build batch product"); goto cleanup; }
            passes*=n;
        }
    }
    for(uint64_t pass=0;pass<passes;++pass) {
        cursor=0; empty_pending=!request->bytes;
        if(joining && pass) {
            uint64_t divisor=1;
            for(unsigned i=0;i<njoins;++i) {
                unsigned current=(pass/divisor)%join_control[i].batches; divisor*=join_control[i].batches;
                if(current==join_control[i].current) continue;
                join_control[i].current=current;
                TRY(copy(controls_mem[i],offsetof(struct qs_join_control,current),&current,sizeof(current),1));
                CS_MEM_HANDLE memory[]={controls_mem[i],input[0]}; compute_io_request(compute,programs[JOIN_LOAD].function,memory,2,counters[0]);
                TRY(csConfigureBatchEntry(load_batch,CS_BATCH_RECONFIG,compute,1,0,NULL));
                TRY(csQueueBatchRequest(load_batch,NULL,NULL,NULL,NULL,NULL));
                TRY(copy(controls_mem[i],0,&join_control[i],sizeof(join_control[0]),0));
            }
            TRY(copy(join_mem,0,join_control,njoins*sizeof(join_control[0]),1));
        }
    /* Like DuckDB's parallel scan tasks, each completed slot claims the next
     * unassigned range. There is no four-chunk round barrier. */
    for(;;) {
        if(QueryCancelPending || ProcDiePending) goto cleanup;
        for(unsigned i=0;i<workers;++i) {
            struct slot *slot=&slots[i];
            if(slot->request || (cursor>=request->bytes && !empty_pending)) continue;
            empty_pending=false;
            unsigned bytes=request->bytes-cursor<chunk?request->bytes-cursor:chunk;
            if(bytes!=slot->bytes) {
                TRY(copy(tasks[i],offsetof(struct qs_task,input_bytes),&bytes,sizeof(bytes),1));
                slot->bytes=bytes;
            }
            bool first=!slot->batch;
            if(first) TRY(csAllocBatchRequest(CS_BATCH_SERIAL,4,&slot->batch));
            if(bytes) {
                build_storage_request(load,device,input[i],request,extents,cursor,bytes);
                if(first) TRY(csConfigureBatchEntry(slot->batch,CS_BATCH_ADD,load,0,0,&slot->load));
                else TRY(csConfigureBatchEntry(slot->batch,CS_BATCH_RECONFIG,load,slot->load,0,NULL));
            }
            if(first) {
                CS_BATCH_INDEX previous=slot->load,current;
                CS_MEM_HANDLE memory[]={input[i],joining?scan_mem:plan_mem,vectors[i],tasks[i],dict_mem};
                compute_request(compute,programs[SCAN].function,memory,dict_mem?5:4,counters[i]);
                TRY(csConfigureBatchEntry(slot->batch,CS_BATCH_ADD,compute,0,previous,&current)); previous=current;
                if(request->plan.predicate>=0) {
                    CS_MEM_HANDLE filter[]={vectors[i],joining?scan_mem:plan_mem,input[i]}; compute_request(compute,programs[FILTER].function,filter,3,counters[i]);
                    TRY(csConfigureBatchEntry(slot->batch,CS_BATCH_ADD,compute,0,previous,&current)); previous=current;
                }
                CS_MEM_HANDLE aggregate[]={vectors[i],plan_mem,tasks[i],input[i]};
                if(joining) {
                    CS_MEM_HANDLE memory[]={vectors[i],plan_mem,tasks[i],join_mem,joined[i],input[i]};
                    if(streaming || indexed) compute_io_request(compute,programs[SINK].function,memory,6,counters[i]);
                    else compute_request(compute,programs[SINK].function,memory,5,counters[i]);
                } else if(streaming) compute_io_request(compute,programs[SINK].function,aggregate,4,counters[i]);
                else compute_request(compute,programs[SINK].function,aggregate,3,counters[i]);
                TRY(csConfigureBatchEntry(slot->batch,CS_BATCH_ADD,compute,0,previous,&current));
            }
            CS_STATUS status=csQueueBatchRequest(slot->batch,slot,NULL,event,&slot->request,NULL);
            if(status!=CS_QUEUED) {
                result->status=status==CS_SUCCESS?CS_ERROR_IN_EXECUTION:status;
                snprintf(result->error,sizeof(result->error),"csQueueBatchRequest: status=%d",status); goto cleanup;
            }
            ++inflight; cursor+=bytes;
        }
        if(!inflight) break;
        void *completed=NULL;
        CS_STATUS status=csPollEvent(event,&completed,NULL);
        if(status==CS_NOT_DONE) { pg_usleep(50L); continue; }
        --inflight; ((struct slot *)completed)->request=NULL;
        if(status!=CS_SUCCESS) {
            result->status=status; snprintf(result->error,sizeof(result->error),"csPollEvent: status=%d",status); goto cleanup;
        }
        if(indexed) {
            struct slot *slot=completed; unsigned i=slot-slots; struct qs_task task;
            TRY(copy(tasks[i],0,&task,sizeof(task),0));
            if(slot->resume) { TRY(csFreeBatchRequest(slot->resume)); slot->resume=NULL; }
            if(task.index_pending) {
                if(!task.index_nreads || task.index_nreads>QS_INDEX_READS) { strcpy(result->error,"invalid CSD index page request"); goto cleanup; }
                TRY(csAllocBatchRequest(CS_BATCH_SERIAL,task.index_nreads+1,&slot->resume));
                CS_BATCH_INDEX previous=0,current;
                /* Only addresses cross to PG. Each LOAD uses the existing
                 * NVM->AFDM command; index/heap bytes never visit Guest RAM. */
                for(unsigned j=0;j<task.index_nreads;) {
                    unsigned first=j;
                    memset(load,0,sizeof(*load)); load->ReqType=CS_STORAGE_IO;
                    load->u.StorageIo=(CsStorageRequest){.Mode=CS_STORAGE_BLOCK_IO,.DevHandle=device,
                        .u.BlockIo={.Type=CS_STORAGE_LOAD_TYPE,.DevMem={input[i],QS_SORT_IO_BYTES+task.index_reads[j].slot*QS_PAGE}}};
                    do {
                        const struct qs_index_read *read=&task.index_reads[j];
                        const struct qs_request *file=read->heap?&builds[njoins-1]:&index_request;
                        const struct qs_extent *map=read->heap?build_extents[njoins-1]:index_extents;
                        uint64_t at=(uint64_t)read->block*QS_PAGE;
                        if(read->heap>1 || read->slot>=QS_INDEX_CACHE || at>file->bytes || QS_PAGE>file->bytes-at) { strcpy(result->error,"CSD index page is outside its prepared file"); goto cleanup; }
                        append_ranges(&load->u.StorageIo.u.BlockIo,file,map,at,QS_PAGE); ++j;
                    } while(j<task.index_nreads && task.index_reads[j].slot==task.index_reads[first].slot+j-first);
                    TRY(csConfigureBatchEntry(slot->resume,CS_BATCH_ADD,load,0,previous,&current)); previous=current;
                }
                CS_MEM_HANDLE memory[]={vectors[i],plan_mem,tasks[i],join_mem,joined[i],input[i]};
                compute_io_request(compute,programs[SINK].function,memory,6,counters[i]);
                TRY(csConfigureBatchEntry(slot->resume,CS_BATCH_ADD,compute,0,previous,&current));
                status=csQueueBatchRequest(slot->resume,slot,NULL,event,&slot->request,NULL);
                if(status!=CS_QUEUED) { result->status=status; snprintf(result->error,sizeof(result->error),"index continuation: status=%d",status); goto cleanup; }
                ++inflight;
            }
        }
    }
    } /* All probe batches finish before loading the next build batch. */
    if(load_batch) { TRY(csFreeBatchRequest(load_batch)); load_batch=NULL; }
    if(joining) {
        /* No source reads the hash table. Release it before grouped source
         * uses that reservation for final-result sorting. */
        for(unsigned i=0;i<njoins;++i) {
            CS_MEM_HANDLE memory[]={controls_mem[i]}; compute_request(compute,programs[JOIN_CLOSE].function,memory,1,NULL);
            TRY(csQueueComputeRequest(&compute->u.Compute,NULL,NULL,NULL,NULL,NULL));
        }
    }
    if(profiling) { uint64_t now=clock_ns(); stats.pipeline_ns[0]=now-phase; phase=now; }
    for(unsigned i=0;i<workers;++i) if(slots[i].batch) { TRY(csFreeBatchRequest(slots[i].batch)); slots[i].batch=NULL; }
    TRY(csDeleteEvent(event)); event=NULL;
    /* Empty input needs one aggregate state for the ungrouped NULL/count row. */
    if(!request->bytes) group.count=1;
    for(unsigned i=0;i<group.count;++i) { struct qs_task task; TRY(copy(tasks[i],0,&task,sizeof(task),0)); group.states[i]=task.state; }
    TRY(copy(group_mem,0,&group,sizeof(group),1)); group_ready=1;
    if(QueryCancelPending || ProcDiePending) goto cleanup;
    TRY(csAllocBatchRequest(CS_BATCH_SERIAL,3,&batch));
    CS_BATCH_INDEX previous=0,current;
    if(streaming && dict_mem) {
        CS_MEM_HANDLE source[]={group_mem,out_mem,dict_mem,input[0]};
        compute_io_request(compute,programs[SOURCE].function,source,4,counters[0]);
    } else if(streaming) {
        CS_MEM_HANDLE source[]={group_mem,out_mem,input[0]};
        compute_io_request(compute,programs[SOURCE].function,source,3,counters[0]);
    } else {
        CS_MEM_HANDLE source[]={group_mem,out_mem};
        compute_request(compute,programs[SOURCE].function,source,2,counters[0]);
    }
    TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,0,&previous));
    if(request->plan.norder && !streaming) {
        CS_MEM_HANDLE order[]={out_mem,plan_mem}; compute_request(compute,programs[SORT].function,order,2,counters[0]);
        TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,previous,&current)); previous=current;
    }
    if(!streaming && (request->plan.limit>=0 || request->plan.offset)) {
        CS_MEM_HANDLE limit[]={out_mem,plan_mem}; compute_request(compute,programs[LIMIT].function,limit,2,counters[0]);
        TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,previous,&current));
    }
    TRY(csQueueBatchRequest(batch,NULL,NULL,NULL,NULL,NULL));
    TRY(copy(out_mem,0,result,sizeof(*result),0));
    result->metrics.fdm_bytes=((sizeof(request->plan)+4095)&~(size_t)4095)+
        ((sizeof(*result)+4095)&~(size_t)4095)+4096+
        workers*(chunk+((qs_chunk_bytes(chunk/24,scan_cols)+4095)&~(size_t)4095)+4096+(profiling?4096:0)+((joined_bytes+4095)&~(size_t)4095))+
        (joining?2*((sizeof(request->plan)+4095)&~(size_t)4095)+(njoins+1)*4096:0);
    if(joining) {
        TRY(copy(join_mem,0,join_control,njoins*sizeof(join_control[0]),0));
        for(unsigned i=0;i<group.count;++i) {
            struct qs_task task; TRY(copy(tasks[i],0,&task,sizeof(task),0));
            join_totals.scanned+=task.scanned; join_totals.matched+=task.matched; join_totals.join_rows+=task.joined;
            join_totals.index_pages+=task.index_pages; join_totals.index_tuples+=task.index_tuples;
        }
        join_totals.build_batches=passes;
        join_totals.input_bytes=request->bytes*passes+build_bytes+join_totals.index_pages*QS_PAGE;
        join_totals.chunks=nchunks*passes+build_chunks;
        for(unsigned i=0;i<njoins;++i) {
            join_totals.build_rows+=join_control[i].rows;
            join_totals.spill_write_bytes+=join_control[i].spill_write_bytes;
            join_totals.spill_read_bytes+=join_control[i].spill_read_bytes;
        }
        join_metrics(&result->metrics,&join_totals);
    }
    if(profiling) {
        cleanup_start=clock_ns(); stats.pipeline_ns[1]=cleanup_start-phase;
        for(unsigned i=0;i<workers;++i) {
            struct qs_profile partial; TRY(copy(counters[i],0,&partial,sizeof(partial),0));
            for(unsigned j=0;j<QS_OPERATORS;++j) {
                struct qs_timing *t=&stats.operators[j],*p=&partial.operators[j];
                t->calls+=p->calls; t->wall_ns+=p->wall_ns; t->cpu_ns+=p->cpu_ns;
                if(p->max_wall_ns>t->max_wall_ns) t->max_wall_ns=p->max_wall_ns;
            }
            if(!i && saved) saved->last_counter=partial;
        }
    }
    if(saved && !result->finished) {
        saved->device=device; saved->batch=batch; batch=NULL;
        saved->output=out_mem; out_mem=NULL;
        saved->group=group_mem; group_mem=NULL;
        saved->dict=dict_mem; dict_mem=NULL;
        saved->io=input[0]; input[0]=NULL;
        saved->counter=counters[0]; counters[0]=NULL;
        saved->source=programs[SOURCE]; programs[SOURCE].active=programs[SOURCE].loaded=0;
        saved->lock_fd=lock_fd; lock_fd=-1;
        saved->profiling=profiling; saved->start=start; saved->profile=stats;
        saved->fdm_bytes=result->metrics.fdm_bytes;
        saved->join_metrics=join_totals;
        *stream=saved; saved=NULL;
    }
cleanup:
    if(profiling && !cleanup_start) cleanup_start=clock_ns();
    /* Requests already executing cannot be forcibly aborted by this backend.
     * Stop refilling, abort queued requests where possible, and drain every
     * completion before freeing buffers or reading/freeing aggregate states. */
    if(event) {
        for(unsigned i=0;i<workers;++i) if(slots[i].request) csAbortRequest(slots[i].request);
        while(inflight) {
            CS_STATUS status=csPollEvent(event,NULL,NULL);
            if(status==CS_NOT_DONE) pg_usleep(50L);
            else --inflight;
        }
        csDeleteEvent(event);
    }
    for(unsigned i=0;i<workers;++i) { if(slots[i].batch) csFreeBatchRequest(slots[i].batch); if(slots[i].resume) csFreeBatchRequest(slots[i].resume); }
    if(batch) csFreeBatchRequest(batch);
    if(build_batch) csFreeBatchRequest(build_batch);
    if(load_batch) csFreeBatchRequest(load_batch);
    if(device && group_mem && compute && programs[SOURCE].active) {
        if(!group_ready) {
            group.count=workers;
            for(unsigned i=0;i<workers;++i) if(tasks[i]) { struct qs_task task={0}; copy(tasks[i],0,&task,sizeof(task),0); group.states[i]=task.state; }
            copy(group_mem,0,&group,sizeof(group),1);
        }
        if(streaming) drop_source(programs[SOURCE].function,group_mem,compute);
        else {
            CS_MEM_HANDLE memory[]={group_mem}; compute_request(compute,programs[SOURCE].function,memory,1,NULL);
            csQueueComputeRequest(&compute->u.Compute,NULL,NULL,NULL,NULL,NULL);
        }
    }
    if(join_mem && compute && programs[JOIN_CLOSE].active) {
        for(unsigned i=0;i<njoins;++i) if(controls_mem[i]) {
            CS_MEM_HANDLE memory[]={controls_mem[i]}; compute_request(compute,programs[JOIN_CLOSE].function,memory,1,NULL);
            csQueueComputeRequest(&compute->u.Compute,NULL,NULL,NULL,NULL,NULL);
        }
    }
    CsMemFlags release={.FDMId=1};
    for(unsigned i=0;i<QS_WORKERS;++i) { if(input[i])csFreeMem(input[i],&release); if(vectors[i])csFreeMem(vectors[i],&release); if(tasks[i])csFreeMem(tasks[i],&release); if(counters[i])csFreeMem(counters[i],&release); if(joined[i])csFreeMem(joined[i],&release); }
    if(scan_mem)csFreeMem(scan_mem,&release);
    if(build_mem)csFreeMem(build_mem,&release);
    if(join_mem)csFreeMem(join_mem,&release);
    for(unsigned i=0;i<njoins;++i) if(controls_mem[i]) csFreeMem(controls_mem[i],&release);
    if(plan_mem)csFreeMem(plan_mem,&release);
    if(out_mem)csFreeMem(out_mem,&release);
    if(group_mem)csFreeMem(group_mem,&release);
    if(dict_mem)csFreeMem(dict_mem,&release);
    for(unsigned i=0;i<FUNCTIONS;++i) unload_program(device,&programs[i]);
    if(device && !*stream)csCloseCSx(device);
    if(lock_fd>=0)close(lock_fd);
    free(caps);free(fdm);free(load);free(compute);
    free(saved);
    if(profiling) {
        uint64_t now=clock_ns(); stats.cleanup_ns=now-cleanup_start; stats.total_ns=now-start;
        result->profile=stats;
        if(*stream) (*stream)->profile=stats;
    }
    RESUME_INTERRUPTS();
    CHECK_FOR_INTERRUPTS();
    if(result->status) ereport(ERROR,(errmsg("CSD execution failed: %s",result->error)));
}

void pg_sina_close(struct pg_sina_stream *s)
{
    if(!s) return;
    HOLD_INTERRUPTS();
    unsigned char storage[sizeof(CsBatchRequest)+8*sizeof(CsComputeArg)] __attribute__((aligned(16)));
    CsBatchRequest *compute=(void *)storage;
    if(s->batch) csFreeBatchRequest(s->batch);
    drop_source(s->source.function,s->group,compute);
    CsMemFlags flags={.FDMId=1};
    csFreeMem(s->output,&flags); csFreeMem(s->group,&flags); csFreeMem(s->io,&flags);
    if(s->dict) csFreeMem(s->dict,&flags);
    if(s->counter) csFreeMem(s->counter,&flags);
    unload_program(s->device,&s->source); csCloseCSx(s->device);
    if(s->lock_fd>=0) close(s->lock_fd);
    free(s); RESUME_INTERRUPTS();
}

void pg_sina_fetch(struct pg_sina_stream **cursor,struct qs_result *result)
{
    struct pg_sina_stream *s=*cursor;
    HOLD_INTERRUPTS();
    uint64_t start=s->profiling?clock_ns():0;
    CS_STATUS status=csQueueBatchRequest(s->batch,NULL,NULL,NULL,NULL,NULL);
    if(status==CS_SUCCESS) status=copy(s->output,0,result,sizeof(*result),0);
    if(status==CS_SUCCESS && s->profiling) {
        struct qs_profile current;
        status=copy(s->counter,0,&current,sizeof(current),0);
        if(status==CS_SUCCESS) {
            for(unsigned i=0;i<QS_OPERATORS;++i) {
                struct qs_timing *t=&s->profile.operators[i],*c=&current.operators[i],*old=&s->last_counter.operators[i];
                t->calls+=c->calls-old->calls; t->cpu_ns+=c->cpu_ns-old->cpu_ns; t->wall_ns+=c->wall_ns-old->wall_ns;
                if(c->max_wall_ns>t->max_wall_ns) t->max_wall_ns=c->max_wall_ns;
            }
            s->last_counter=current;
            s->profile.pipeline_ns[1]+=clock_ns()-start;
        }
    }
    if(status==CS_SUCCESS) { result->metrics.fdm_bytes=s->fdm_bytes; join_metrics(&result->metrics,&s->join_metrics); }
    bool closing=status!=CS_SUCCESS || result->status || result->finished || QueryCancelPending || ProcDiePending;
    if(s->profiling) {
        result->profile=s->profile;
        result->profile.total_ns=clock_ns()-s->start;
    }
    if(closing) {
        uint64_t cleanup=s->profiling?clock_ns():0;
        bool profiling=s->profiling; *cursor=NULL; pg_sina_close(s);
        if(profiling) {
            uint64_t elapsed=clock_ns()-cleanup;
            result->profile.cleanup_ns+=elapsed; result->profile.total_ns+=elapsed;
            /* Time waiting for the PG consumer is outside active pipeline work. */
            result->profile.total_ns=result->profile.setup_ns+result->profile.build_ns+result->profile.pipeline_ns[0]+
                result->profile.pipeline_ns[1]+result->profile.cleanup_ns;
        }
    }
    RESUME_INTERRUPTS(); CHECK_FOR_INTERRUPTS();
    if(status!=CS_SUCCESS || result->status) ereport(ERROR,(errmsg("CSD result fetch failed: status=%d",status)));
}
