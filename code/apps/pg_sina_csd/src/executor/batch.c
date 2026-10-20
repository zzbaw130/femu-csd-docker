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
enum { SCAN,FILTER,SINK,SOURCE,SORT,LIMIT,FUNCTIONS };
struct program { const char *file,*symbol; CS_CSF_ID function; unsigned index; int loaded,active; };
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
    memset(b,0,sizeof(*b)+6*sizeof(CsComputeArg)); b->ReqType=CS_QUEUE_COMPUTE;
    b->u.Compute.CSFId=function; b->u.Compute.NumArgs=count;
    for(unsigned i=0;i<count;++i) csHelperSetComputeArg(&b->u.Compute.Args[i],CS_AFDM_TYPE,memories[i],0UL);
    if(counters) {
        csHelperSetComputeArg(&b->u.Compute.Args[count],CS_AFDM_TYPE,counters,0UL);
        csHelperSetComputeArg(&b->u.Compute.Args[count+1],CS_64BIT_VALUE_TYPE,(u64)1);
        b->u.Compute.NumArgs+=2;
    }
}
#define TRY(call) do { CS_STATUS status=(call); if(status!=CS_SUCCESS) { \
    result->status=status; snprintf(result->error,sizeof(result->error),"%s: status=%d",#call,status); goto cleanup; } } while(0)
void pg_sina_execute(struct qs_request *request,struct qs_extent *extents,struct qs_result *result,bool profiling)
{
    uint64_t start=profiling?clock_ns():0,phase=start,cleanup_start=0;
    struct qs_profile stats={0};
    CHECK_FOR_INTERRUPTS();
    /* Do not longjmp through SNIA C++ frames or its batch threads. Poll PG's
     * pending cancel at batch boundaries, then release resources before PG
     * reports the interrupt. No PG allocations/errors occur while held. */
    HOLD_INTERRUPTS();
    memset(result,0,sizeof(*result));
    result->status=1; strcpy(result->error,"CSD execution interrupted or initialization failed");
    CS_DEV_HANDLE device=NULL; CS_BATCH_HANDLE batch=NULL;
    CS_MEM_HANDLE plan_mem=NULL,out_mem=NULL,group_mem=NULL;
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
        [LIMIT]={.file="pg-sina-limit.so",.symbol="pg_sina_limit",.index=506}};
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
    while((uint64_t)workers*(chunk+((qs_chunk_bytes(chunk/24,request->plan.ncols)+4095)&~(size_t)4095)+4096+(profiling?4096:0))+
          sizeof(struct qs_result)+sizeof(struct qs_plan)+8192>capacity) {
        if(workers>1) --workers;
        else if(chunk>QS_PAGE) chunk/=2;
        else { result->status=1; strcpy(result->error,"insufficient FDM"); goto cleanup; }
    }
    uint64_t nchunks=(request->bytes+chunk-1)/chunk;
    if(nchunks && workers>nchunks) workers=nchunks;
    group.count=workers;
    for(unsigned i=0;i<FUNCTIONS;++i) {
        if((i==FILTER && request->plan.predicate<0) || (i==SORT && !request->plan.norder) ||
            (i==LIMIT && request->plan.limit<0 && !request->plan.offset)) continue;
        TRY(load_program(device,&programs[i]));
    }
    CsMemFlags flags={.FDMId=1};
    TRY(csAllocMem(device,sizeof(request->plan),&flags,&plan_mem,NULL));
    TRY(copy(plan_mem,0,&request->plan,sizeof(request->plan),1));
    TRY(csAllocMem(device,sizeof(*result),&flags,&out_mem,NULL));
    TRY(csAllocMem(device,sizeof(group),&flags,&group_mem,NULL));
    for(unsigned i=0;i<workers;++i) {
        TRY(csAllocMem(device,chunk,&flags,&input[i],NULL));
        TRY(csAllocMem(device,qs_chunk_bytes(chunk/24,request->plan.ncols),&flags,&vectors[i],NULL));
        struct qs_chunk header={.capacity=chunk/24,.ncols=request->plan.ncols}; TRY(copy(vectors[i],0,&header,sizeof(header),1));
        flags.Flags=CS_FDM_CLEAR; TRY(csAllocMem(device,sizeof(struct qs_task),&flags,&tasks[i],NULL)); flags.Flags=0;
        if(profiling) {
            flags.Flags=CS_FDM_CLEAR;
            TRY(csAllocMem(device,sizeof(stats),&flags,&counters[i],NULL)); flags.Flags=0;
        }
    }
    load=calloc(1,sizeof(*load)+(request->nextents+1)*sizeof(CSBlockRange));
    compute=calloc(1,sizeof(*compute)+6*sizeof(CsComputeArg));
    if(!load || !compute) { result->status=1; strcpy(result->error,"host request allocation failed"); goto cleanup; }
    CS_BATCH_INDEX nodes[QS_WORKERS][4]={{0}}; unsigned nnodes[QS_WORKERS]={0};
    TRY(csAllocBatchRequest(CS_BATCH_HYBRID,4*workers,&batch));
    uint64_t cursor=0; unsigned round=0;
    if(profiling) { uint64_t now=clock_ns(); stats.setup_ns=now-phase; phase=now; }
    do {
        unsigned active=0;
        for(unsigned i=0;i<workers;++i) {
            if(cursor>=request->bytes && (round || i)) {
                for(unsigned j=nnodes[i];j>0;--j) TRY(csConfigureBatchEntry(batch,CS_BATCH_DELETE,NULL,nodes[i][j-1],0,NULL));
                nnodes[i]=0; continue;
            }
            ++active;
            unsigned bytes=request->bytes-cursor<chunk?request->bytes-cursor:chunk;
            TRY(copy(tasks[i],offsetof(struct qs_task,input_bytes),&bytes,sizeof(bytes),1));
            if(bytes) {
                memset(load,0,sizeof(*load)); load->ReqType=CS_STORAGE_IO;
                load->u.StorageIo=(CsStorageRequest){.Mode=CS_STORAGE_BLOCK_IO,.DevHandle=device,
                    .u.BlockIo={.Type=CS_STORAGE_LOAD_TYPE,.DevMem={input[i],0}}};
                CsBlockIo *io=&load->u.StorageIo.u.BlockIo; uint64_t at=cursor,end=cursor+bytes;
                for(unsigned j=0;j<request->nextents && at<end;++j) {
                    const struct qs_extent *e=&extents[j]; if(e->logical+e->bytes<=at) continue;
                    uint64_t length=(e->logical+e->bytes<end?e->logical+e->bytes:end)-at;
                    io->Range[io->NumRanges++]=(CSBlockRange){request->nsid,e->lba+(at-e->logical)/request->lba_bytes,length/request->lba_bytes}; at+=length;
                }
                if(!round) { TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,load,0,0,&nodes[i][nnodes[i]++])); }
                else TRY(csConfigureBatchEntry(batch,CS_BATCH_RECONFIG,load,nodes[i][0],0,NULL));
            }
            if(!round) {
                CS_MEM_HANDLE memory[]={input[i],plan_mem,vectors[i],tasks[i]};
                compute_request(compute,programs[SCAN].function,memory,4,counters[i]);
                CS_BATCH_INDEX previous=nnodes[i]?nodes[i][nnodes[i]-1]:0;
                TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,previous,&nodes[i][nnodes[i]++]));
                previous=nodes[i][nnodes[i]-1];
                if(request->plan.predicate>=0) {
                    CS_MEM_HANDLE filter[]={vectors[i],plan_mem}; compute_request(compute,programs[FILTER].function,filter,2,counters[i]);
                    TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,previous,&nodes[i][nnodes[i]++])); previous=nodes[i][nnodes[i]-1];
                }
                CS_MEM_HANDLE aggregate[]={vectors[i],plan_mem,tasks[i]}; compute_request(compute,programs[SINK].function,aggregate,3,counters[i]);
                TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,previous,&nodes[i][nnodes[i]++]));
            }
            cursor+=bytes;
        }
        if(QueryCancelPending || ProcDiePending) goto cleanup;
        if(active) TRY(csQueueBatchRequest(batch,NULL,NULL,NULL,NULL,NULL));
        ++round;
    } while(cursor<request->bytes);
    if(profiling) { uint64_t now=clock_ns(); stats.pipeline_ns[0]=now-phase; phase=now; }
    TRY(csFreeBatchRequest(batch)); batch=NULL;
    /* Empty input needs one aggregate state for the ungrouped NULL/count row. */
    if(!request->bytes) group.count=1;
    for(unsigned i=0;i<group.count;++i) { struct qs_task task; TRY(copy(tasks[i],0,&task,sizeof(task),0)); group.states[i]=task.state; }
    TRY(copy(group_mem,0,&group,sizeof(group),1)); group_ready=1;
    if(QueryCancelPending || ProcDiePending) goto cleanup;
    TRY(csAllocBatchRequest(CS_BATCH_SERIAL,3,&batch));
    CS_BATCH_INDEX previous=0,current;
    CS_MEM_HANDLE source[]={group_mem,out_mem}; compute_request(compute,programs[SOURCE].function,source,2,counters[0]);
    TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,0,&previous));
    if(request->plan.norder) {
        CS_MEM_HANDLE order[]={out_mem,plan_mem}; compute_request(compute,programs[SORT].function,order,2,counters[0]);
        TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,previous,&current)); previous=current;
    }
    if(request->plan.limit>=0 || request->plan.offset) {
        CS_MEM_HANDLE limit[]={out_mem,plan_mem}; compute_request(compute,programs[LIMIT].function,limit,2,counters[0]);
        TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,previous,&current));
    }
    TRY(csQueueBatchRequest(batch,NULL,NULL,NULL,NULL,NULL));
    TRY(copy(out_mem,0,result,sizeof(*result),0));
    result->metrics.fdm_bytes=((sizeof(request->plan)+4095)&~(size_t)4095)+
        ((sizeof(*result)+4095)&~(size_t)4095)+4096+
        workers*(chunk+((qs_chunk_bytes(chunk/24,request->plan.ncols)+4095)&~(size_t)4095)+4096+(profiling?4096:0));
    if(profiling) {
        cleanup_start=clock_ns(); stats.pipeline_ns[1]=cleanup_start-phase;
        for(unsigned i=0;i<workers;++i) {
            struct qs_profile partial; TRY(copy(counters[i],0,&partial,sizeof(partial),0));
            for(unsigned j=0;j<QS_OPERATORS;++j) {
                struct qs_timing *t=&stats.operators[j],*p=&partial.operators[j];
                t->calls+=p->calls; t->wall_ns+=p->wall_ns; t->cpu_ns+=p->cpu_ns;
                if(p->max_wall_ns>t->max_wall_ns) t->max_wall_ns=p->max_wall_ns;
            }
        }
    }
cleanup:
    if(profiling && !cleanup_start) cleanup_start=clock_ns();
    if(batch) csFreeBatchRequest(batch);
    if(device && group_mem && compute && programs[SOURCE].active) {
        if(!group_ready) {
            group.count=workers;
            for(unsigned i=0;i<workers;++i) if(tasks[i]) { struct qs_task task={0}; copy(tasks[i],0,&task,sizeof(task),0); group.states[i]=task.state; }
            copy(group_mem,0,&group,sizeof(group),1);
        }
        CS_MEM_HANDLE memory[]={group_mem}; compute_request(compute,programs[SOURCE].function,memory,1,NULL);
        csQueueComputeRequest(&compute->u.Compute,NULL,NULL,NULL,NULL,NULL);
    }
    CsMemFlags release={.FDMId=1};
    for(unsigned i=0;i<QS_WORKERS;++i) { if(input[i])csFreeMem(input[i],&release); if(vectors[i])csFreeMem(vectors[i],&release); if(tasks[i])csFreeMem(tasks[i],&release); if(counters[i])csFreeMem(counters[i],&release); }
    if(plan_mem)csFreeMem(plan_mem,&release);
    if(out_mem)csFreeMem(out_mem,&release);
    if(group_mem)csFreeMem(group_mem,&release);
    for(unsigned i=0;i<FUNCTIONS;++i) unload_program(device,&programs[i]);
    if(device)csCloseCSx(device);
    if(lock_fd>=0)close(lock_fd);
    free(caps);free(fdm);free(load);free(compute);
    if(profiling) {
        uint64_t now=clock_ns(); stats.cleanup_ns=now-cleanup_start; stats.total_ns=now-start;
        result->profile=stats;
    }
    RESUME_INTERRUPTS();
    CHECK_FOR_INTERRUPTS();
    if(result->status) ereport(ERROR,(errmsg("CSD execution failed: %s",result->error)));
}
