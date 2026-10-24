/* One direct SNIA session for the selected PG tree; no per-subplan locks. */
#include "pg_internal.h"
#include "cs.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>

typedef struct PlanStateCsd PlanStateCsd;
struct plan_slot {
    struct qp_control control;
    CS_BATCH_HANDLE batch, resume;
    CS_BATCH_INDEX previous,scan_load;
    CS_REQ_HANDLE pending; /* PG-owned marker for the whole callback chain. */
    atomic_bool completed;
    CS_STATUS completion_status;
    u64 completion_value;
    bool profiling;
    uint64_t callback_ns;
    bool ready, done;
    unsigned scan_node;
    bool need_input;
    uint64_t queued_ns;
    PlanStateCsd *owner;
    unsigned worker;
    pthread_mutex_t mutex;
    bool mutex_ready;
    CS_REQ_HANDLE inflight;
    uint64_t auto_submits,auto_gap_ns,auto_gap_max_ns,auto_pending_ns;
    uint64_t scan_loads;
    uint64_t host_gap_ns,host_gap_max_ns,host_requeues;
};
struct PlanStateCsd {
    CustomScanState css;
    struct qp_plan *plan;
    struct plan_slot slots[QS_WORKERS+1];
    unsigned workers, epoch, device_cus;
    atomic_uint_fast64_t next_scan_offset;
    atomic_bool stopping;
    bool parallel;
    uint64_t load_ns, load_calls;
    /* Host-thread wall times: queue/load preparation are subsets of submit;
     * pending time is summed across concurrent requests, not query latency. */
    uint64_t poll_ns, completion_copy_ns, submit_ns, queue_ns, control_write_ns, idle_ns, pending_ns;
    uint64_t submits, polls, completions;
    uint64_t completion_reads, control_writes;
    uint64_t notice_ns, notice_max_ns;
    uint64_t barrier_started,barrier_ns,barriers;
    uint64_t setup_ns, cleanup_ns;
    uint64_t fdm_bytes;
    struct qs_request files[QP_NODES];
    struct qs_extent *extents[QP_NODES];
    struct qs_request indexes[QP_NODES];
    struct qs_extent *index_extents[QP_NODES];
    struct qs_result *result;
    CS_DEV_HANDLE device;
    CS_MEM_HANDLE memory[5];
    CS_MEM_HANDLE worker_io[QS_WORKERS];
    CS_CSF_ID function;
    unsigned program_index;
    unsigned row;
    bool started, explain_only, loaded, active, profiling;
    uint64_t start_ns;
    MemoryContextCallback cleanup;
};
static const CustomExecMethods methods;
static uint64_t clock_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }
static CS_STATUS queue_scan(struct plan_slot *slot);
/* A drained input with no output can reuse its buffers immediately. This
 * callback uses only libc, immutable mappings and SNIA, never PostgreSQL APIs.
 * Shared-table growth/spill still runs on the coordinator after workers stop. */
static void completed(void *context,CS_STATUS status,u64 value)
{
    struct plan_slot *slot=context;
    uint64_t arrived=slot->profiling?clock_ns():0;
    pthread_mutex_lock(&slot->mutex);
    if(status==CS_SUCCESS && value==QP_INPUT_DRAINED && slot->scan_node &&
       !atomic_load_explicit(&slot->owner->stopping,memory_order_acquire)) {
        uint64_t previous=slot->queued_ns;
        CS_STATUS next=queue_scan(slot);
        if(next==CS_QUEUED) {
            if(slot->profiling) {
                uint64_t gap=slot->queued_ns-arrived;
                ++slot->auto_submits; slot->auto_gap_ns+=gap;
                slot->auto_gap_max_ns=Max(slot->auto_gap_max_ns,gap);
                slot->auto_pending_ns+=arrived-previous;
            }
            pthread_mutex_unlock(&slot->mutex);
            return;
        }
        status=next;
    }
    slot->callback_ns=arrived;
    slot->completion_status=status; slot->completion_value=value;
    pthread_mutex_unlock(&slot->mutex);
    atomic_store_explicit(&slot->completed,true,memory_order_release);
}
static CS_STATUS transfer(CS_MEM_HANDLE memory,size_t offset,void *data,size_t bytes,bool write)
{
    for(size_t done=0;done<bytes;) {
        size_t n=Min((size_t)QS_SORT_IO_BYTES,bytes-done);
        CsCopyMemRequest request={.Type=write?CS_COPY_TO_DEVICE:CS_COPY_FROM_DEVICE,.DevMem={memory,offset+done},
            .u.HostVAddress=(char *)data+done,.Bytes=n};
        CS_STATUS status=csQueueCopyMemRequest(&request,NULL,NULL,NULL,NULL,NULL);
        if(status!=CS_SUCCESS) return status;
        done+=n;
    }
    return CS_SUCCESS;
}
static void checked(CS_STATUS status,const char *operation)
{ if(status!=CS_SUCCESS) ereport(ERROR,(errmsg("CSD PG Plan %s failed: status=%d",operation,status))); }
static Node *create(CustomScan *scan)
{
    PlanStateCsd *s=(void *)newNode(sizeof(*s),T_CustomScanState);
    s->css.methods=&methods; return (Node *)s;
}
static void close_state(void *arg)
{
    PlanStateCsd *s=arg;
    uint64_t started=s->profiling?clock_ns():0;
    HOLD_INTERRUPTS();
    atomic_store_explicit(&s->stopping,true,memory_order_release);
    for(unsigned w=0;w<=s->workers;++w) if(s->slots[w].pending) {
        pthread_mutex_lock(&s->slots[w].mutex);
        if(!atomic_load_explicit(&s->slots[w].completed,memory_order_acquire)) csAbortRequest(s->slots[w].inflight);
        pthread_mutex_unlock(&s->slots[w].mutex);
        while(!atomic_load_explicit(&s->slots[w].completed,memory_order_acquire)) pg_usleep(50L);
        s->slots[w].pending=NULL;
    }
    if(s->device && s->slots[0].batch && s->memory[1] && s->active) {
        unsigned close=1;
        if(transfer(s->memory[1],offsetof(struct qp_control,close),&close,sizeof(close),true)==CS_SUCCESS)
            csQueueBatchRequest(s->slots[0].batch,NULL,NULL,NULL,NULL,NULL);
    }
    for(unsigned w=0;w<=s->workers;++w) {
        if(s->slots[w].batch) { csFreeBatchRequest(s->slots[w].batch); s->slots[w].batch=NULL; }
        if(s->slots[w].resume) { csFreeBatchRequest(s->slots[w].resume); s->slots[w].resume=NULL; }
        if(s->slots[w].mutex_ready) { pthread_mutex_destroy(&s->slots[w].mutex); s->slots[w].mutex_ready=false; }
    }
    CsMemFlags flags={.FDMId=1};
    for(unsigned i=0;i<5;++i) if(s->memory[i]) { csFreeMem(s->memory[i],&flags); s->memory[i]=NULL; }
    for(unsigned w=0;w<s->workers;++w) if(s->worker_io[w]) {
        csFreeMem(s->worker_io[w],&flags); s->worker_io[w]=NULL;
    }
    if(s->active) {
        CsConfigInfo c={.Type=CS_CSF_ACTIVATE,.u.CSFActivateInfo={.CSFId=s->program_index,.EEIId=1,.State=CS_INACTIVE_STATE}};
        CsConfigData data; int size=sizeof(data); csConfig(s->device,&size,&c,&data); s->active=false;
    }
    if(s->loaded) {
        CsCSFDownloadInfo d={.CSEEId=1,.Index=s->program_index,.common.Unload=1}; u32 id;
        csCSFDownload(s->device,&d,&id); s->loaded=false;
    }
    if(s->device) { csCloseCSx(s->device); s->device=NULL; }
    if(s->profiling) s->cleanup_ns+=clock_ns()-started;
    RESUME_INTERRUPTS();
}
static void begin(CustomScanState *node,EState *estate,int flags)
{
    PlanStateCsd *s=(void *)node; CustomScan *scan=(void *)node->ss.ps.plan;
    Const *constant=linitial_node(Const,scan->custom_private); bytea *bytes=DatumGetByteaP(constant->constvalue);
    if(VARSIZE_ANY_EXHDR(bytes)!=sizeof(*s->plan)) elog(ERROR,"invalid CSD PG Plan");
    s->plan=palloc(sizeof(*s->plan)); memcpy(s->plan,VARDATA_ANY(bytes),sizeof(*s->plan));
    s->result=(void *)TYPEALIGN(16,(uintptr_t)palloc0(sizeof(*s->result)+15));
    s->explain_only=flags&EXEC_FLAG_EXPLAIN_ONLY;
    atomic_init(&s->stopping,false); atomic_init(&s->next_scan_offset,0);
    s->cleanup.func=close_state; s->cleanup.arg=s;
    MemoryContextRegisterResetCallback(estate->es_query_cxt,&s->cleanup);
}
/* Compute offsets without allocating, so a larger I/O block can be tried
 * before reducing CU concurrency. Small relations need only their own bytes. */
static size_t layout_buffers(PlanStateCsd *s,unsigned entries,unsigned data,const bool *indexed)
{
    size_t arena=(qs_dict_bytes(entries,data)+4095)&~(size_t)4095;
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_SCAN && !indexed[i]) {
        struct qp_node *n=&s->plan->nodes[i]; unsigned bytes=n->ordered_index?QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE:qp_scan_buffer_bytes(s->plan,n);
        n->input_offset=arena; arena+=bytes; n->vector_offset=arena;
        arena+=(qs_chunk_bytes(n->ordered_index?256:qp_scan_capacity(s->plan,n),n->expr.ncols)+4095)&~(size_t)4095;
    }
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_INDEX_JOIN) {
        struct qp_node *n=&s->plan->nodes[i]; n->input_offset=arena;
        arena+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE; n->vector_offset=arena;
        arena+=4096+((qs_chunk_bytes(256,s->plan->nodes[n->right].expr.ncols+2)+4095)&~(size_t)4095);
    }
    size_t stride=0;
    if(s->workers>1) for(unsigned root=0;root<s->plan->count;++root) if(s->plan->nodes[root].parallel) {
        size_t end=arena;
        for(int id=root;id>=0;id=s->plan->nodes[id].left) {
            struct qp_node *n=&s->plan->nodes[id];
            if(n->kind==QP_SCAN) {
                unsigned bytes=n->ordered_index?QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE:qp_scan_buffer_bytes(s->plan,n);
                n->parallel_input=end; end+=bytes; n->parallel_vector=end;
                end+=(qs_chunk_bytes(n->ordered_index?256:qp_scan_capacity(s->plan,n),n->expr.ncols)+4095)&~(size_t)4095; break;
            }
            if(n->kind==QP_INDEX_JOIN) {
                n->parallel_input=end; end+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE; n->parallel_vector=end;
                end+=4096+((qs_chunk_bytes(256,s->plan->nodes[n->right].expr.ncols+2)+4095)&~(size_t)4095);
            }
        }
        stride=Max(stride,end-arena);
    }
    s->plan->parallel_stride=stride; s->plan->workers=s->workers?s->workers:1;
    return arena+(s->workers?s->workers-1:0)*stride;
}
static void start(PlanStateCsd *s)
{
    const char *device=NULL;
    for(unsigned i=0;i<s->plan->count;++i) {
        struct qp_node *n=&s->plan->nodes[i];
        if(n->kind!=QP_SCAN) continue;
        struct qs_request *r=&s->files[i]; r->plan=n->expr;
        s->extents[i]=pg_sina_mapping(r); n->bytes=r->bytes;
        if(device && strcmp(device,r->device)) elog(ERROR,"CSD PG Plan requires one namespace");
        device=r->device;
        if(n->expr.join.indexid) {
            s->indexes[i].plan.relid=n->expr.join.indexid;
            s->indexes[i].plan.relfilenode=n->expr.join.indexnode;
            s->index_extents[i]=pg_sina_mapping(&s->indexes[i]);
            n->index_bytes=s->indexes[i].bytes;
        }
    }
    if(!device) elog(ERROR,"CSD PG Plan has no prepared relation");
    pg_sina_read_resources(device);
    s->profiling=pg_sina_profile; s->start_ns=clock_ns(); s->load_ns=s->load_calls=0;
    atomic_store(&s->stopping,false);
    s->poll_ns=s->completion_copy_ns=s->submit_ns=s->queue_ns=s->control_write_ns=0;
    s->idle_ns=s->pending_ns=s->submits=s->polls=s->completions=0;
    s->completion_reads=s->control_writes=0;
    s->notice_ns=s->notice_max_ns=0;
    s->barrier_started=s->barrier_ns=s->barriers=0;
    s->setup_ns=s->cleanup_ns=0;
    memset(s->plan->pipelines,0,sizeof(s->plan->pipelines));
    s->plan->nodes[s->plan->root].expr.work_mem_bytes=pg_sina_query_memory();
    s->program_index=pg_sina_program_index(510);
    checked(csOpenCSx(device,NULL,&s->device),"open");
    unsigned entries,data;
    qp_dictionary_size(s->plan,&entries,&data);
    bool indexed[QP_NODES]={0};
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_INDEX_JOIN) indexed[s->plan->nodes[i].right]=true;
    qp_parallel_boundaries(s->plan,s->plan->root);
    for(unsigned i=0;i<s->plan->ninit;++i) qp_parallel_boundaries(s->plan,s->plan->init_nodes[i]);
    bool parallel=false;
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].parallel) parallel=true;
    s->plan->workers=1; s->workers=0;
    s->device_cus=pg_sina_resources.cus;
    if(parallel) s->workers=Min(QS_WORKERS,s->device_cus);
    uint64_t capacity=pg_sina_resources.fdm_bytes;
    if(s->workers<=1) s->workers=0;
    s->plan->scan_bytes=QS_CHUNK;
    size_t arena;
    for(;;) {
        arena=layout_buffers(s,entries,data,indexed);
        size_t needed=arena+sizeof(*s->plan)+(s->workers+1)*(sizeof(struct qp_control)+sizeof(*s->result)+QS_SORT_IO_BYTES)+65536;
        if(capacity && needed>capacity) {
            if(s->plan->scan_bytes>QP_SCAN_BYTES) { s->plan->scan_bytes/=2; continue; }
            if(s->workers>1) { if(--s->workers==1) s->workers=0; continue; }
        }
        break;
    }
    CsMemFlags flags={.FDMId=1};
    size_t sizes[]={sizeof(*s->plan),(s->workers+1)*sizeof(struct qp_control),(s->workers+1)*sizeof(*s->result),arena,QS_SORT_IO_BYTES};
    s->fdm_bytes=s->workers*QS_SORT_IO_BYTES;
    for(unsigned i=0;i<5;++i) s->fdm_bytes+=sizes[i];
    for(unsigned i=0;i<5;++i) checked(csAllocMem(s->device,sizes[i],&flags,&s->memory[i],NULL),"allocate AFDM");
    for(unsigned w=0;w<s->workers;++w)
        checked(csAllocMem(s->device,QS_SORT_IO_BYTES,&flags,&s->worker_io[w],NULL),"allocate worker spill AFDM");
    checked(transfer(s->memory[0],0,s->plan,sizeof(*s->plan),true),"copy plan");
    checked(transfer(s->memory[1],0,&s->slots[0].control,sizeof(s->slots[0].control),true),"initialize control");
    struct qs_dict dictionary={.entries=entries,.capacity=entries,.data=data};
    checked(transfer(s->memory[3],0,&dictionary,sizeof(dictionary),true),"initialize strings");
    char zeros[QS_SORT_IO_BYTES]={0};
    size_t offset=sizeof(dictionary)+(size_t)entries*sizeof(struct qs_dict_entry);
    for(size_t i=0;i<(size_t)entries*4;i+=sizeof(zeros))
        checked(transfer(s->memory[3],offset+i,zeros,Min(sizeof(zeros),(size_t)entries*4-i),true),"clear string buckets");
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_SCAN && !indexed[i]) {
        struct qp_node *n=&s->plan->nodes[i];
        struct qs_chunk header={.capacity=n->ordered_index?256:qp_scan_capacity(s->plan,n),.ncols=n->expr.ncols};
        checked(transfer(s->memory[3],n->vector_offset,&header,sizeof(header),true),"initialize scan");
    }
    for(unsigned w=0;w<s->workers;++w)
        checked(transfer(s->memory[1],(w+1)*sizeof(struct qp_control),&s->slots[w+1].control,sizeof(struct qp_control),true),"initialize worker control");
    char descriptor[]="pg-sina-plan.so\0pg_sina_plan";
    CsCSFDownloadInfo download={.Type=CS_CSF_VENDOR_SPECIFIC,.SubType=CS_SUBTYPE_VENDOR_SPECIFIC,.CSEEId=1,.Index=s->program_index};
    strcpy(download.common.UniqueName,"pg-sina-plan"); download.common.DataBuffer=descriptor; download.common.Length=sizeof(descriptor);
    u32 id; checked(csCSFDownload(s->device,&download,&id),"download"); s->loaded=true;
    CsConfigInfo config={.Type=CS_CSF_ACTIVATE,.u.CSFActivateInfo={.CSFId=s->program_index,.EEIId=1,.State=CS_ACTIVE_STATE}};
    CsConfigData result; int length=sizeof(result); checked(csConfig(s->device,&length,&config,&result),"activate"); s->active=true;
    length=0; int count=0; CS_STATUS status=csGetCSFId(s->device,"pg-sina-plan",0,&length,&count,NULL);
    if(status!=CS_LENGTH_RETURNED) checked(status,"lookup function length");
    CSFIdInfo *functions=palloc(length); checked(csGetCSFId(s->device,"pg-sina-plan",0,&length,&count,functions),"lookup function");
    s->function=functions[0].CSFId; pfree(functions);
    size_t size=sizeof(CsBatchRequest)+6*sizeof(CsComputeArg); CsBatchRequest *request=palloc0(size);
    request->ReqType=CS_QUEUE_COMPUTE; request->u.Compute.CSFId=s->function; request->u.Compute.NumArgs=6;
    for(unsigned i=0;i<5;++i) csHelperSetComputeArg(&request->u.Compute.Args[i],CS_AFDM_TYPE,s->memory[i],0UL);
    for(unsigned w=0;w<=s->workers;++w) {
        struct plan_slot *slot=&s->slots[w];
        if(pthread_mutex_init(&slot->mutex,NULL)) elog(ERROR,"cannot initialize CSD submission lock");
        slot->mutex_ready=true; slot->owner=s; slot->worker=w;
        /* FEMU compute ranges start at zero; each CU needs its own spill AFDM. */
        csHelperSetComputeArg(&request->u.Compute.Args[4],CS_AFDM_TYPE,w?s->worker_io[w-1]:s->memory[4],0UL);
        csHelperSetComputeArg(&request->u.Compute.Args[5],CS_64BIT_VALUE_TYPE,(u64)(s->profiling|(w<<1)));
        checked(csAllocBatchRequest(CS_BATCH_SERIAL,1,&s->slots[w].batch),"allocate batch");
        CS_BATCH_INDEX index; checked(csConfigureBatchEntry(s->slots[w].batch,CS_BATCH_ADD,request,0,0,&index),"configure batch");
        atomic_init(&s->slots[w].completed,false); s->slots[w].profiling=s->profiling;
    }
    pfree(request); s->started=true;
    if(s->profiling) s->setup_ns=clock_ns()-s->start_ns;
}
static CS_STATUS append_ranges(CsBlockIo *io,const struct qs_request *file,const struct qs_extent *extents,uint64_t at,uint64_t end)
{
    for(unsigned e=0;e<file->nextents && at<end;++e) {
        if(extents[e].logical+extents[e].bytes<=at) continue;
        if(extents[e].logical>at) return CS_INVALID_ARG;
        uint64_t n=Min(end,extents[e].logical+extents[e].bytes)-at;
        CSBlockRange range={file->nsid,extents[e].lba+(at-extents[e].logical)/file->lba_bytes,n/file->lba_bytes};
        CSBlockRange *last=io->NumRanges?&io->Range[io->NumRanges-1]:NULL;
        if(last && last->NamespaceId==range.NamespaceId && last->StartLba+last->NumBlocks==range.StartLba) last->NumBlocks+=range.NumBlocks;
        else io->Range[io->NumRanges++]=range;
        at+=n;
    }
    return at==end?CS_SUCCESS:CS_INVALID_ARG;
}
/* The caller holds slot->mutex. Every block has its own request snapshot;
 * mappings and device handles remain alive until all callbacks are drained. */
static CS_STATUS queue_scan(struct plan_slot *slot)
{
    PlanStateCsd *s=slot->owner;
    unsigned node=slot->scan_node-1,worker=slot->worker;
    const struct qp_node *n=&s->plan->nodes[node];
    const struct qs_request *file=&s->files[node];
    uint64_t offset=atomic_fetch_add_explicit(&s->next_scan_offset,s->plan->scan_bytes,memory_order_relaxed);
    if(offset>n->bytes) offset=n->bytes;
    uint64_t bytes=Min((uint64_t)s->plan->scan_bytes,n->bytes-offset);
    size_t size=sizeof(CsBatchRequest)+(file->nextents+1)*sizeof(CSBlockRange);
    CsBatchRequest *load=calloc(1,size);
    CsBatchRequest *compute=calloc(1,sizeof(*compute)+6*sizeof(CsComputeArg));
    CS_STATUS status=CS_OUT_OF_RESOURCES;
    if(!load || !compute) goto done;
    bool reuse=bytes && slot->resume;
    if(slot->resume && !reuse) {
        status=csFreeBatchRequest(slot->resume); if(status!=CS_SUCCESS) goto done;
        slot->resume=NULL;
    }
    if(!reuse) {
        status=csAllocBatchRequest(CS_BATCH_SERIAL,bytes?2:1,&slot->resume);
        if(status!=CS_SUCCESS) goto done;
    }
    CS_BATCH_INDEX previous=0,current;
    if(bytes) {
        size_t input=worker>1?n->parallel_input+(worker-2)*s->plan->parallel_stride:n->input_offset;
        load->ReqType=CS_STORAGE_IO;
        load->u.StorageIo=(CsStorageRequest){.Mode=CS_STORAGE_BLOCK_IO,.DevHandle=s->device,
            .u.BlockIo={.Type=CS_STORAGE_LOAD_TYPE,.DevMem={s->memory[3],input}}};
        status=append_ranges(&load->u.StorageIo.u.BlockIo,file,s->extents[node],offset,offset+bytes);
        if(status!=CS_SUCCESS) goto done;
        status=csConfigureBatchEntry(slot->resume,reuse?CS_BATCH_RECONFIG:CS_BATCH_ADD,load,
            reuse?slot->scan_load:0,0,reuse?NULL:&slot->scan_load);
        if(status!=CS_SUCCESS) goto done;
        previous=slot->scan_load;
    }
    compute->ReqType=CS_QUEUE_COMPUTE; compute->u.Compute.CSFId=s->function; compute->u.Compute.NumArgs=6;
    for(unsigned i=0;i<5;++i) csHelperSetComputeArg(&compute->u.Compute.Args[i],CS_AFDM_TYPE,s->memory[i],0UL);
    csHelperSetComputeArg(&compute->u.Compute.Args[4],CS_AFDM_TYPE,s->worker_io[worker-1],0UL);
    uint64_t parameter=s->profiling|(worker<<1)|QP_SCAN_INPUT|((offset/QS_PAGE)<<QP_SCAN_PAGE_SHIFT);
    csHelperSetComputeArg(&compute->u.Compute.Args[5],CS_64BIT_VALUE_TYPE,(u64)parameter);
    status=csConfigureBatchEntry(slot->resume,reuse?CS_BATCH_RECONFIG:CS_BATCH_ADD,compute,
        reuse?slot->previous:0,reuse?0:previous,reuse?NULL:&current);
    if(status!=CS_SUCCESS) goto done;
    if(!reuse) slot->previous=current;
    slot->control.bytes=slot->control.index_nreads=0; slot->need_input=false;
    if(s->profiling) slot->queued_ns=clock_ns();
    status=csQueueBatchRequest(slot->resume,slot,completed,NULL,&slot->inflight,NULL);
    if(status==CS_QUEUED && bytes) ++slot->scan_loads;
done:
    free(compute); free(load); return status;
}
static void configure_load(PlanStateCsd *s,unsigned worker,CsStorageRequest *load)
{
    struct plan_slot *slot=&s->slots[worker];
    if(!slot->resume) { checked(csQueueStorageRequest(load,NULL,NULL,NULL,NULL,NULL),"internal LOAD"); return; }
    size_t bytes=sizeof(CsBatchRequest)+load->u.BlockIo.NumRanges*sizeof(CSBlockRange);
    CsBatchRequest *request=palloc0(bytes); request->ReqType=CS_STORAGE_IO;
    memcpy(&request->u.StorageIo,load,sizeof(*load)+load->u.BlockIo.NumRanges*sizeof(CSBlockRange));
    CS_BATCH_INDEX current;
    checked(csConfigureBatchEntry(slot->resume,CS_BATCH_ADD,request,0,slot->previous,&current),"configure LOAD");
    slot->previous=current; pfree(request);
}
static void load_pages(PlanStateCsd *s,unsigned worker)
{
    struct qp_control *c=&s->slots[worker].control;
    unsigned node=c->node; uint64_t started=clock_ns();
    if(node>=s->plan->count) elog(ERROR,"invalid CSD page request");
    const struct qp_node *n=&s->plan->nodes[node];
    size_t input=worker>1?n->parallel_input+(worker-2)*s->plan->parallel_stride:n->input_offset;
    if(c->index_nreads) {
        if(c->index_nreads>QS_INDEX_READS || (n->kind!=QP_INDEX_JOIN && !n->ordered_index)) elog(ERROR,"invalid CSD index request");
        unsigned leaf=n->ordered_index?node:(unsigned)n->right,max_extents=Max(s->files[leaf].nextents,s->indexes[leaf].nextents);
        for(unsigned j=0;j<c->index_nreads;) {
            unsigned first=j++; const struct qs_index_read *read=&c->index_reads[first];
            while(j<c->index_nreads && c->index_reads[j].slot==read->slot+j-first) ++j;
            CsStorageRequest *load=palloc0(sizeof(*load)+((j-first)*(max_extents+1))*sizeof(CSBlockRange));
            load->Mode=CS_STORAGE_BLOCK_IO; load->DevHandle=s->device;
            CsBlockIo *io=&load->u.BlockIo; io->Type=CS_STORAGE_LOAD_TYPE;
            io->DevMem=(CsDevAFDM){s->memory[3],input+QS_SORT_IO_BYTES+(size_t)read->slot*QS_PAGE};
            for(unsigned k=first;k<j;++k) {
                read=&c->index_reads[k]; struct qs_request *file=read->heap?&s->files[leaf]:&s->indexes[leaf];
                struct qs_extent *extents=read->heap?s->extents[leaf]:s->index_extents[leaf];
                uint64_t at=(uint64_t)read->block*QS_PAGE,end=at+QS_PAGE;
                if(read->heap>1 || read->slot>=QS_INDEX_CACHE || end>file->bytes) elog(ERROR,"invalid CSD index page");
                checked(append_ranges(io,file,extents,at,end),"map index pages");
            }
            configure_load(s,worker,load); ++s->load_calls; pfree(load);
        }
    } else {
        if(c->offset>s->files[node].bytes || c->bytes>s->files[node].bytes-c->offset) elog(ERROR,"invalid CSD page request");
        struct qs_request *file=&s->files[node];
        CsStorageRequest *load=palloc0(sizeof(*load)+(file->nextents+1)*sizeof(CSBlockRange));
        load->Mode=CS_STORAGE_BLOCK_IO; load->DevHandle=s->device;
        CsBlockIo *io=&load->u.BlockIo; io->Type=CS_STORAGE_LOAD_TYPE; io->DevMem=(CsDevAFDM){s->memory[3],input};
        checked(append_ranges(io,file,s->extents[node],c->offset,c->offset+c->bytes),"map heap pages");
        configure_load(s,worker,load); ++s->load_calls; pfree(load);
    }
    c->ready=1; s->load_ns+=clock_ns()-started;
}
static void submit(PlanStateCsd *s,unsigned worker)
{
    uint64_t started=s->profiling?clock_ns():0;
    struct plan_slot *slot=&s->slots[worker];
    uint64_t parameter=s->profiling|(worker<<1);
    bool assigned=slot->scan_node && slot->need_input;
    if(assigned) {
        atomic_store_explicit(&slot->completed,false,memory_order_relaxed);
        pthread_mutex_lock(&slot->mutex);
        CS_STATUS status=queue_scan(slot);
        slot->pending=status==CS_QUEUED?slot->inflight:NULL;
        if(s->profiling) {
            if(status==CS_QUEUED && slot->callback_ns) {
                uint64_t gap=slot->queued_ns-slot->callback_ns;
                slot->host_gap_ns+=gap; slot->host_gap_max_ns=Max(slot->host_gap_max_ns,gap); ++slot->host_requeues;
            }
            slot->callback_ns=0;
            s->queue_ns+=clock_ns()-slot->queued_ns; s->submit_ns+=clock_ns()-started; ++s->submits;
        }
        pthread_mutex_unlock(&slot->mutex);
        if(status!=CS_QUEUED) checked(status,"queue scan block");
        return;
    }
    if((slot->control.bytes || slot->control.index_nreads) && !slot->control.ready) {
        unsigned groups=1;
        for(unsigned j=1;j<slot->control.index_nreads;++j)
            groups+=slot->control.index_reads[j].slot!=slot->control.index_reads[j-1].slot+1;
        if(groups<64) checked(csAllocBatchRequest(CS_BATCH_SERIAL,groups+1,&slot->resume),"allocate LOAD batch");
        slot->previous=0; load_pages(s,worker);
        uint64_t written=s->profiling?clock_ns():0;
        checked(transfer(s->memory[1],worker*sizeof(struct qp_control)+offsetof(struct qp_control,ready),&slot->control.ready,sizeof(slot->control.ready),true),"resume LOAD");
        if(s->profiling) { s->control_write_ns+=clock_ns()-written; ++s->control_writes; }
    }
    if(slot->resume) {
        CsBatchRequest *request=palloc0(sizeof(*request)+6*sizeof(CsComputeArg));
        request->ReqType=CS_QUEUE_COMPUTE; request->u.Compute.CSFId=s->function; request->u.Compute.NumArgs=6;
        for(unsigned i=0;i<5;++i) csHelperSetComputeArg(&request->u.Compute.Args[i],CS_AFDM_TYPE,s->memory[i],0UL);
        csHelperSetComputeArg(&request->u.Compute.Args[4],CS_AFDM_TYPE,worker?s->worker_io[worker-1]:s->memory[4],0UL);
        csHelperSetComputeArg(&request->u.Compute.Args[5],CS_64BIT_VALUE_TYPE,(u64)parameter);
        CS_BATCH_INDEX current;
        checked(csConfigureBatchEntry(slot->resume,CS_BATCH_ADD,request,0,slot->previous,&current),"configure resumed compute"); pfree(request);
    }
    if(!worker && slot->control.available) {
        uint64_t written=s->profiling?clock_ns():0;
        checked(transfer(s->memory[1],offsetof(struct qp_control,available),&slot->control.available,sizeof(slot->control.available),true),"provide worker rows");
        if(s->profiling) { s->control_write_ns+=clock_ns()-written; ++s->control_writes; }
    }
    if(s->profiling) slot->queued_ns=clock_ns();
    atomic_store_explicit(&slot->completed,false,memory_order_relaxed);
    pthread_mutex_lock(&slot->mutex);
    CS_STATUS status=csQueueBatchRequest(slot->resume?slot->resume:slot->batch,slot,completed,NULL,&slot->inflight,NULL);
    slot->pending=status==CS_QUEUED?slot->inflight:NULL;
    if(s->profiling) {
        uint64_t now=clock_ns();
        s->queue_ns+=now-slot->queued_ns; s->submit_ns+=now-started; ++s->submits;
    }
    pthread_mutex_unlock(&slot->mutex);
    if(status!=CS_QUEUED) checked(status,"queue tree");
}
static void fetch(PlanStateCsd *s)
{
    /* Result buffers remain in AFDM: the final pipeline consumes worker rows
     * on a CU. PG only polls completions and assigns the next ready CSF. */
    for(;;) {
        CHECK_FOR_INTERRUPTS(); bool progress=false;
        for(unsigned w=0;w<=s->workers;++w) {
            struct plan_slot *slot=&s->slots[w];
            if(!slot->pending) continue;
            uint64_t polled=s->profiling?clock_ns():0;
            HOLD_INTERRUPTS();
            bool done=atomic_load_explicit(&slot->completed,memory_order_acquire);
            CS_STATUS status=done?slot->completion_status:CS_NOT_DONE;
            u64 value=done?slot->completion_value:0;
            uint64_t observed=s->profiling?clock_ns():0;
            if(s->profiling) { s->poll_ns+=observed-polled; ++s->polls; }
            if(status==CS_NOT_DONE) { RESUME_INTERRUPTS(); continue; }
            if(s->profiling) {
                uint64_t notice=observed-slot->callback_ns;
                s->notice_ns+=notice; s->notice_max_ns=Max(s->notice_max_ns,notice);
                s->pending_ns+=observed-slot->queued_ns; ++s->completions;
            }
            slot->pending=NULL;
            if(w && slot->scan_node && status==CS_SUCCESS) {
                slot->control.output_rows=(value&QP_OUTPUT_READY)!=0;
                slot->control.finished=(value&QP_WORKER_FINISHED)!=0;
                slot->need_input=(value&QP_INPUT_DRAINED)!=0;
            } else {
                CS_STATUS copied=transfer(s->memory[1],w*sizeof(struct qp_control),&slot->control,sizeof(slot->control),false);
                if(status==CS_SUCCESS) status=copied;
                if(s->profiling) ++s->completion_reads;
            }
            struct qs_result header;
            if(!w) {
                memset(&header,0,offsetof(struct qs_result,data));
                if(status==CS_SUCCESS && (slot->control.output_rows || slot->control.finished))
                    status=transfer(s->memory[2],0,&header,offsetof(struct qs_result,data),false);
            }
            if(s->profiling) s->completion_copy_ns+=clock_ns()-observed;
            RESUME_INTERRUPTS(); CHECK_FOR_INTERRUPTS();
            if(status!=CS_SUCCESS && slot->control.error)
                ereport(ERROR,(errmsg("CSD PG Plan node %u failed: device=%d, status=%d",slot->control.error_node,slot->control.error,status)));
            checked(status,"execute tree"); progress=true;
            if(slot->resume) { checked(csFreeBatchRequest(slot->resume),"release LOAD batch"); slot->resume=NULL; }
            if(w) { slot->ready=slot->control.output_rows || slot->control.finished; slot->done=slot->control.finished; continue; }
            memcpy(s->result,&header,offsetof(struct qs_result,data));
            for(unsigned i=1;i<=s->workers;++i) if(slot->control.drained&(UINT64_C(1)<<(i-1))) s->slots[i].ready=false;
            s->parallel=slot->control.parallel;
            if(slot->control.epoch!=s->epoch) {
                s->epoch=slot->control.epoch;
                atomic_store(&s->next_scan_offset,0);
                for(unsigned i=1;i<=s->workers;++i) {
                    struct plan_slot *worker=&s->slots[i];
                    worker->ready=worker->done=false;
                    worker->scan_node=slot->control.scan_node;
                    worker->need_input=worker->scan_node!=0;
                    if(worker->scan_node) memset(&worker->control,0,sizeof(worker->control));
                    else checked(transfer(s->memory[1],i*sizeof(struct qp_control),&worker->control,sizeof(worker->control),false),"read worker state");
                }
            }
            if(s->result->rows>QS_RESULT_ROWS) elog(ERROR,"invalid CSD result page");
            if(s->result->rows) {
                checked(transfer(s->memory[2],offsetof(struct qs_result,data),s->result->data,s->result->rows*sizeof(struct qs_row),false),"read result");
                checked(transfer(s->memory[2],offsetof(struct qs_result,str_bytes),&s->result->str_bytes,
                    sizeof(s->result->str_bytes)+sizeof(s->result->str_data),false),"read result strings");
            }
            if(s->result->finished && s->profiling)
                checked(transfer(s->memory[0],offsetof(struct qp_plan,pipelines),s->plan->pipelines,
                    sizeof(s->plan->pipelines),false),"read pipeline profile");
            if(s->result->rows || s->result->finished) { s->row=0; return; }
        }
        if(s->parallel && !(s->slots[0].control.parallel==2 && s->slots[0].pending)) for(unsigned w=1;w<=s->workers;++w) {
            struct plan_slot *slot=&s->slots[w];
            if(!slot->pending && !slot->ready && !slot->done) { HOLD_INTERRUPTS(); submit(s,w); RESUME_INTERRUPTS(); progress=true; }
        }
        struct plan_slot *main=&s->slots[0];
        if(!main->pending) {
            uint64_t ready=0;
            for(unsigned w=1;w<=s->workers;++w) if(s->slots[w].ready) ready|=UINT64_C(1)<<(w-1);
            /* Direct build/aggregate sinks share a bounded table. Its owner
             * may resize or spill only after every CU in the wave is idle. */
            bool idle=true;
            if(main->control.parallel==2)
                for(unsigned w=1;w<=s->workers;++w) if(s->slots[w].pending) idle=false;
            if(s->profiling && main->control.parallel==2) {
                bool output=false;
                for(unsigned w=1;w<=s->workers;++w)
                    if(s->slots[w].ready && s->slots[w].control.output_rows) output=true;
                if(!idle && output && !s->barrier_started) { s->barrier_started=clock_ns(); ++s->barriers; }
                if(idle && s->barrier_started) { s->barrier_ns+=clock_ns()-s->barrier_started; s->barrier_started=0; }
            }
            if(idle && (!s->parallel || ready || !main->control.parallel)) {
                main->control.available=ready;
                HOLD_INTERRUPTS(); submit(s,0); RESUME_INTERRUPTS(); progress=true;
            }
        }
        if(!progress) {
            uint64_t idle=s->profiling?clock_ns():0;
            pg_usleep(50L);
            if(s->profiling) s->idle_ns+=clock_ns()-idle;
        }
    }
}
extern Datum pg_sina_decimal(struct qs_cell);
static TupleTableSlot *execute(CustomScanState *node)
{
    PlanStateCsd *s=(void *)node; TupleTableSlot *slot=node->ss.ss_ScanTupleSlot;
    if(s->explain_only) return ExecClearTuple(slot);
    if(!s->started) { start(s); fetch(s); }
    if(s->row==s->result->rows && !s->result->finished) fetch(s);
    if(s->row==s->result->rows) { close_state(s); return ExecClearTuple(slot); }
    ResetExprContext(node->ss.ps.ps_ExprContext); ExecClearTuple(slot);
    const struct qs_plan *root=&s->plan->nodes[s->plan->root].expr;
    const struct qs_cell *row=s->result->data[s->row++].cells;
    for(unsigned i=0;i<root->nout;++i) {
        struct qs_cell v=row[i]; slot->tts_isnull[i]=v.isnull; if(v.isnull) continue;
        Oid type=root->outputs[i].type_oid;
        if(type==NUMERICOID) slot->tts_values[i]=pg_sina_decimal(v);
        else if(type==TEXTOID || type==VARCHAROID || type==BPCHAROID) {
            char buffer[256]; unsigned kind=root->exprs[root->outputs[i].index].type,length;
            if(kind==Q_CHAR) { buffer[0]=(char)v.value; length=1; }
            else {
                length=qs_cell_string(&v);
                const char *bytes=qs_cell_is_string_ref(&v)?s->result->str_data+(unsigned)v.value:qs_cell_string_bytes(&v);
                if(qs_cell_is_string_ref(&v) && (unsigned)v.value+length>s->result->str_bytes) elog(ERROR,"invalid CSD string result");
                memcpy(buffer,bytes,length);
            }
            buffer[length]=0;
            slot->tts_values[i]=type==BPCHAROID?DirectFunctionCall3(bpcharin,CStringGetDatum(buffer),ObjectIdGetDatum(InvalidOid),Int32GetDatum(root->outputs[i].typmod)):CStringGetTextDatum(buffer);
        } else if(type==FLOAT8OID) slot->tts_values[i]=Float8GetDatum((double)v.value);
        else if(type==BOOLOID) slot->tts_values[i]=BoolGetDatum(v.value!=0);
        else if(type==INT2OID) slot->tts_values[i]=Int16GetDatum(v.value);
        else if(type==INT4OID || type==DATEOID) slot->tts_values[i]=Int32GetDatum(v.value);
        else if(type==INT8OID) {
            if(v.value>PG_INT64_MAX || v.value<PG_INT64_MIN) elog(ERROR,"CSD bigint overflow");
            slot->tts_values[i]=Int64GetDatum(v.value);
        } else elog(ERROR,"unsupported CSD output type");
    }
    ExecStoreVirtualTuple(slot); node->ss.ps.ps_ExprContext->ecxt_scantuple=slot;
    return node->ss.ps.ps_ProjInfo?ExecProject(node->ss.ps.ps_ProjInfo):slot;
}
static void end(CustomScanState *node) { close_state(node); }
static void rescan(CustomScanState *node)
{
    PlanStateCsd *s=(void *)node; close_state(s);
    memset(s->slots,0,sizeof(s->slots)); s->parallel=false; s->epoch=0; memset(s->result,0,sizeof(*s->result)); s->started=false; s->row=0;
}
static void explain(CustomScanState *node,List *ancestors,ExplainState *es)
{
    PlanStateCsd *s=(void *)node;
    ExplainPropertyText("Execution Path","csd",es);
    ExplainPropertyInteger("CSD Device CUs",NULL,pg_sina_resources.cus,es);
    ExplainPropertyInteger("CSD Device Workmem","bytes",pg_sina_resources.workmem_bytes,es);
    ExplainPropertyInteger("CSD Query Memory Limit","bytes",s->plan->nodes[s->plan->root].expr.work_mem_bytes,es);
    ExplainPropertyText("CSD Plan Source","PostgreSQL selected Plan; original join tree",es);
    ExplainPropertyText("CSD Intermediate Data","device workmem and internal spill; 128-bit values",es);
    static const char *names[]={"Scan","Projection","Merge Join","Aggregate","Sort","Limit","Index Join","CTE Scan","CTE materialization","IN hash set"};
    StringInfoData tree; initStringInfo(&tree);
    bool used[QP_NODES]={0}; used[s->plan->root]=true;
    for(unsigned i=0;i<s->plan->ninit;++i) used[s->plan->init_nodes[i]]=true;
    for(unsigned pass=0;pass<s->plan->count;++pass)
        for(unsigned i=0;i<s->plan->count;++i) if(used[i]) {
            const struct qp_node *n=&s->plan->nodes[i];
            if(n->left>=0) used[n->left]=true;
            if(n->right>=0) used[n->right]=true;
        }
    for(unsigned i=0;i<s->plan->count;++i) {
        if(!used[i]) continue;
        const struct qp_node *n=&s->plan->nodes[i];
        if(i) appendStringInfoString(&tree,"; ");
        appendStringInfo(&tree,"%u:%s",i,n->hash && n->kind==QP_AGGREGATE?"Hash Aggregate":n->kind==QP_JOIN && n->hash?"Hash Join":names[n->kind]);
        if(n->kind==QP_SCAN) appendStringInfo(&tree,"(%s%s)",get_rel_name(n->expr.relid),n->ordered_index?(n->ordered_index&4?", index only":n->ordered_index&2?", index ordered, heap read-ahead":", index ordered"):"");
        if(n->kind==QP_SORT) {
            appendStringInfoString(&tree,"[");
            for(unsigned k=0;k<n->expr.norder;++k)
                appendStringInfo(&tree,"%s%u %s",k?",":"",n->expr.order[k].output,n->expr.order[k].descending?"desc":"asc");
            appendStringInfoString(&tree,"]");
        }
        if(n->left>=0) appendStringInfo(&tree," <- %d",n->left);
        if(n->right>=0) appendStringInfo(&tree,",%d",n->right);
    }
    ExplainPropertyText("CSD Plan",tree.data,es); pfree(tree.data);
    if(s->started) {
        unsigned entries,data; qp_dictionary_size(s->plan,&entries,&data);
        ExplainPropertyInteger("CSD Dictionary AFDM","bytes",qs_dict_bytes(entries,data),es);
        ExplainPropertyInteger("CSD Heap Rows",NULL,s->result->metrics.scanned,es);
        ExplainPropertyInteger("CSD Internal Read","bytes",s->result->metrics.input_bytes,es);
        ExplainPropertyInteger("CSD Index/Heap Page Loads",NULL,s->result->metrics.index_pages,es);
        ExplainPropertyInteger("CSD Indexed Heap Tuples",NULL,s->result->metrics.index_tuples,es);
        ExplainPropertyInteger("CSD Workmem Peak","bytes",s->result->metrics.work_peak,es);
        ExplainPropertyInteger("CSD Workers",NULL,s->result->metrics.workers,es);
        ExplainPropertyInteger("CSD Parallel Pipelines",NULL,s->slots[0].control.epoch,es);
        ExplainPropertyInteger("CSD Scan Block","bytes",s->plan->scan_bytes,es);
        ExplainPropertyInteger("CSD Scan Vector Capacity","rows",QP_VECTOR_ROWS,es);
        ExplainPropertyInteger("CSD AFDM","bytes",s->fdm_bytes,es);
        ExplainPropertyInteger("CSD Spill Read","bytes",s->result->metrics.spill_read_bytes,es);
        ExplainPropertyInteger("CSD Spill Write","bytes",s->result->metrics.spill_write_bytes,es);
        ExplainPropertyInteger("CSD Runs",NULL,s->result->metrics.runs,es);
        if(s->profiling) {
            const struct qs_timing *t=&s->result->profile.operators[QS_JOIN_PROBE];
            ExplainPropertyFloat("CSD Tree Wall","ms",(clock_ns()-s->start_ns)/1e6,3,es);
            ExplainPropertyFloat("CSD Setup Wall","ms",s->setup_ns/1e6,3,es);
            ExplainPropertyFloat("CSD Cleanup Wall","ms",s->cleanup_ns/1e6,3,es);
            ExplainPropertyFloat("CSD CSF Wall","ms",t->wall_ns/1e6,3,es);
            ExplainPropertyFloat("CSD CSF CPU","ms",t->cpu_ns/1e6,3,es);
            ExplainPropertyInteger("CSD CSF Calls",NULL,t->calls,es);
            ExplainPropertyInteger("CSD Active CUs Peak",NULL,s->slots[0].control.peak_cus,es);
            static const unsigned ops[]={QS_SCAN,QS_FILTER,QS_SORT_SINK,QS_SORT_SOURCE,QS_JOIN_BUILD,QS_AGG_SINK,QS_AGG_SOURCE};
            static const char *labels[]={"CSD Scan Decode CPU","CSD Vector Filter CPU","CSD Sort Sink CPU","CSD Sort Source CPU",
                "CSD Hash Build Coordinator CPU","CSD Hash Aggregate Coordinator CPU","CSD Aggregate Merge Coordinator CPU"};
            for(unsigned i=0;i<lengthof(ops);++i) ExplainPropertyFloat(labels[i],"ms",s->result->profile.operators[ops[i]].cpu_ns/1e6,3,es);
            for(unsigned i=0;i<s->plan->count;++i) {
                const struct qp_pipeline_profile *p=&s->plan->pipelines[i];
                if(!p->rounds) continue;
                double average=(double)(p->coordinator_ns+p->worker_ns)/p->wall_ns;
                char label[64],value[256]; snprintf(label,sizeof(label),"CSD Pipeline %u",i);
                snprintf(value,sizeof(value),"rounds=%llu wall=%.3f ms coordinator=%.3f ms workers=%.3f ms avg_cus=%.2f cpu_util=%.1f%% sink=%.3f ms sink_rows=%llu",
                    (unsigned long long)p->rounds,p->wall_ns/1e6,p->coordinator_ns/1e6,p->worker_ns/1e6,
                    average,100*average/Max(1,s->device_cus),p->sink_ns/1e6,(unsigned long long)p->sink_rows);
                ExplainPropertyText(label,value,es);
            }
            ExplainPropertyFloat("CSD LOAD Preparation","ms",s->load_ns/1e6,3,es);
            ExplainPropertyInteger("CSD Host LOAD Requests",NULL,s->load_calls,es);
            ExplainPropertyFloat("CSD Host Poll","ms",s->poll_ns/1e6,3,es);
            ExplainPropertyFloat("CSD Host Completion Copy","ms",s->completion_copy_ns/1e6,3,es);
            ExplainPropertyFloat("CSD Host Submit","ms",s->submit_ns/1e6,3,es);
            ExplainPropertyFloat("CSD Host Queue (within Submit)","ms",s->queue_ns/1e6,3,es);
            ExplainPropertyFloat("CSD Host Control Write (within Submit)","ms",s->control_write_ns/1e6,3,es);
            ExplainPropertyFloat("CSD Host Idle Sleep","ms",s->idle_ns/1e6,3,es);
            ExplainPropertyInteger("CSD Host Submissions",NULL,s->submits,es);
            ExplainPropertyInteger("CSD Host Polls",NULL,s->polls,es);
            ExplainPropertyInteger("CSD Host Completions",NULL,s->completions,es);
            uint64_t automatic=0,gap=0,maximum=0,pending=0,loads=0,host_gap=0,host_max=0,requeues=0;
            for(unsigned w=1;w<=s->workers;++w) {
                struct plan_slot *slot=&s->slots[w];
                if(slot->mutex_ready) pthread_mutex_lock(&slot->mutex);
                automatic+=slot->auto_submits; gap+=slot->auto_gap_ns; maximum=Max(maximum,slot->auto_gap_max_ns);
                pending+=slot->auto_pending_ns; loads+=slot->scan_loads;
                host_gap+=slot->host_gap_ns; host_max=Max(host_max,slot->host_gap_max_ns); requeues+=slot->host_requeues;
                if(slot->mutex_ready) pthread_mutex_unlock(&slot->mutex);
            }
            ExplainPropertyFloat("CSD Pending Requests Sum","ms",(s->pending_ns+pending)/1e6,3,es);
            ExplainPropertyInteger("CSD Host Assigned LOADs",NULL,loads,es);
            ExplainPropertyInteger("CSD Callback Submissions",NULL,automatic,es);
            ExplainPropertyFloat("CSD Callback Requeue Average","us",automatic?gap/1e3/automatic:0,3,es);
            ExplainPropertyFloat("CSD Callback Requeue Maximum","us",maximum/1e3,3,es);
            ExplainPropertyFloat("CSD Callback Completed Requests Sum","ms",pending/1e6,3,es);
            ExplainPropertyInteger("CSD PG Scan Requeues",NULL,requeues,es);
            ExplainPropertyFloat("CSD PG Scan Requeue Average","us",requeues?host_gap/1e3/requeues:0,3,es);
            ExplainPropertyFloat("CSD PG Scan Requeue Maximum","us",host_max/1e3,3,es);
            ExplainPropertyInteger("CSD Shared Sink Waits",NULL,s->barriers,es);
            ExplainPropertyFloat("CSD Shared Sink Wait","ms",s->barrier_ns/1e6,3,es);
            ExplainPropertyInteger("CSD Host Completion Reads",NULL,s->completion_reads,es);
            ExplainPropertyInteger("CSD Host Control Writes",NULL,s->control_writes,es);
            ExplainPropertyFloat("CSD Completion Notice Sum","ms",s->notice_ns/1e6,3,es);
            ExplainPropertyFloat("CSD Completion Notice Average","us",s->completions?s->notice_ns/1e3/s->completions:0,3,es);
            ExplainPropertyFloat("CSD Completion Notice Maximum","us",s->notice_max_ns/1e3,3,es);
        }
    }
}
static const CustomExecMethods methods={.CustomName="SniaCsdPlan",.BeginCustomScan=begin,.ExecCustomScan=execute,
    .EndCustomScan=end,.ReScanCustomScan=rescan,.ExplainCustomScan=explain};
const CustomScanMethods pg_sina_plan_methods={.CustomName="SniaCsdPlan",.CreateCustomScanState=create};
