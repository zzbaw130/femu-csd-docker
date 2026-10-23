/* One direct SNIA session for the selected PG tree; no per-subplan locks. */
#include "pg_internal.h"
#include "cs.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include <fcntl.h>
#include <errno.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

struct plan_slot {
    struct qp_control control;
    CS_BATCH_HANDLE batch, resume;
    CS_BATCH_INDEX previous;
    CS_EVT_HANDLE event;
    CS_REQ_HANDLE pending;
    bool ready, done;
};
typedef struct {
    CustomScanState css;
    struct qp_plan *plan;
    struct plan_slot slots[QS_WORKERS+1];
    unsigned workers, epoch;
    bool parallel;
    uint64_t load_ns, load_calls;
    struct qs_request files[QP_NODES];
    struct qs_extent *extents[QP_NODES];
    struct qs_request indexes[QP_NODES];
    struct qs_extent *index_extents[QP_NODES];
    struct qs_result *result;
    CS_DEV_HANDLE device;
    CS_MEM_HANDLE memory[5];
    CS_CSF_ID function;
    int lock_fd;
    unsigned row;
    bool started, explain_only, loaded, active, profiling;
    uint64_t start_ns;
    MemoryContextCallback cleanup;
} PlanStateCsd;
static const CustomExecMethods methods;
static uint64_t clock_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }
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
    s->css.methods=&methods; s->lock_fd=-1; return (Node *)s;
}
static void close_state(void *arg)
{
    PlanStateCsd *s=arg;
    HOLD_INTERRUPTS();
    for(unsigned w=0;w<=s->workers;++w) if(s->slots[w].pending) {
        csAbortRequest(s->slots[w].pending);
        while(csPollEvent(s->slots[w].event,NULL,NULL)==CS_NOT_DONE) pg_usleep(50L);
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
        if(s->slots[w].event) { csDeleteEvent(s->slots[w].event); s->slots[w].event=NULL; }
    }
    CsMemFlags flags={.FDMId=1};
    for(unsigned i=0;i<5;++i) if(s->memory[i]) { csFreeMem(s->memory[i],&flags); s->memory[i]=NULL; }
    if(s->active) {
        CsConfigInfo c={.Type=CS_CSF_ACTIVATE,.u.CSFActivateInfo={.CSFId=510,.EEIId=1,.State=CS_INACTIVE_STATE}};
        CsConfigData data; int size=sizeof(data); csConfig(s->device,&size,&c,&data); s->active=false;
    }
    if(s->loaded) {
        CsCSFDownloadInfo d={.CSEEId=1,.Index=510,.common.Unload=1}; u32 id;
        csCSFDownload(s->device,&d,&id); s->loaded=false;
    }
    if(s->device) { csCloseCSx(s->device); s->device=NULL; }
    if(s->lock_fd>=0) { close(s->lock_fd); s->lock_fd=-1; }
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
        arena+=(qs_chunk_bytes(n->ordered_index?256:bytes/24,n->expr.ncols)+4095)&~(size_t)4095;
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
                unsigned bytes=qp_scan_buffer_bytes(s->plan,n);
                n->parallel_input=end; end+=bytes; n->parallel_vector=end;
                end+=(qs_chunk_bytes(bytes/24,n->expr.ncols)+4095)&~(size_t)4095; break;
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
    const char *device=NULL; bool big=false;
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
        for(unsigned col=0;col<n->expr.nattrs;++col)
            if(n->expr.cols[col].slot>=0 && n->expr.cols[col].type==Q_STRING && r->bytes>QS_DICT_SMALL_DATA) big=true;
    }
    if(!device) elog(ERROR,"CSD PG Plan has no prepared relation");
    s->profiling=pg_sina_profile; s->start_ns=clock_ns(); s->load_ns=s->load_calls=0;
    s->plan->nodes[s->plan->root].expr.work_mem_bytes=(uint64_t)pg_sina_max_memory*1024;
    s->lock_fd=open(device,O_RDONLY|O_CLOEXEC);
    if(s->lock_fd<0) elog(ERROR,"cannot open CSD namespace");
    while(flock(s->lock_fd,LOCK_EX|LOCK_NB)) {
        if(errno!=EWOULDBLOCK && errno!=EINTR) elog(ERROR,"cannot lock CSD namespace");
        CHECK_FOR_INTERRUPTS(); pg_usleep(1000L);
    }
    checked(csOpenCSx(device,NULL,&s->device),"open");
    unsigned entries=big?QS_DICT_BIG_ENTRIES:QS_DICT_SMALL_ENTRIES,data=big?QS_DICT_BIG_DATA:QS_DICT_SMALL_DATA;
    bool indexed[QP_NODES]={0};
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_INDEX_JOIN) indexed[s->plan->nodes[i].right]=true;
    qp_parallel_boundaries(s->plan,s->plan->root);
    for(unsigned i=0;i<s->plan->ninit;++i) qp_parallel_boundaries(s->plan,s->plan->init_nodes[i]);
    bool parallel=false;
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].parallel) parallel=true;
    s->plan->workers=1; s->workers=0;
    uint64_t capacity=0; int length=0;
    if(csQueryDeviceProperties(s->device,CS_CSE_TYPE,&length,NULL)==CS_LENGTH_RETURNED) {
        CsProperties *properties=palloc(length);
        checked(csQueryDeviceProperties(s->device,CS_CSE_TYPE,&length,properties),"query CU count");
        if(parallel) s->workers=Min(QS_WORKERS,properties->CSEDetails[0].CSE[0].NumCRs);
        pfree(properties);
    }
    length=0;
    if(csQueryDeviceProperties(s->device,CS_FDM_TYPE,&length,NULL)==CS_LENGTH_RETURNED) {
        CsProperties *properties=palloc(length);
        checked(csQueryDeviceProperties(s->device,CS_FDM_TYPE,&length,properties),"query FDM capacity");
        capacity=properties->FDMDetails[0].FDM[0].FDMSize; pfree(properties);
    }
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
    size_t sizes[]={sizeof(*s->plan),(s->workers+1)*sizeof(struct qp_control),(s->workers+1)*sizeof(*s->result),arena,(s->workers+1)*QS_SORT_IO_BYTES};
    for(unsigned i=0;i<5;++i) checked(csAllocMem(s->device,sizes[i],&flags,&s->memory[i],NULL),"allocate AFDM");
    checked(transfer(s->memory[0],0,s->plan,sizeof(*s->plan),true),"copy plan");
    checked(transfer(s->memory[1],0,&s->slots[0].control,sizeof(s->slots[0].control),true),"initialize control");
    struct qs_dict dictionary={.entries=entries,.capacity=entries,.data=data};
    checked(transfer(s->memory[3],0,&dictionary,sizeof(dictionary),true),"initialize strings");
    char zeros[4096]={0};
    size_t offset=sizeof(dictionary)+(size_t)entries*sizeof(struct qs_dict_entry);
    for(size_t i=0;i<(size_t)entries*4;i+=sizeof(zeros))
        checked(transfer(s->memory[3],offset+i,zeros,Min(sizeof(zeros),(size_t)entries*4-i),true),"clear string buckets");
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_SCAN && !indexed[i]) {
        struct qp_node *n=&s->plan->nodes[i];
        struct qs_chunk header={.capacity=n->ordered_index?256:qp_scan_buffer_bytes(s->plan,n)/24,.ncols=n->expr.ncols};
        checked(transfer(s->memory[3],n->vector_offset,&header,sizeof(header),true),"initialize scan");
    }
    for(unsigned w=0;w<s->workers;++w)
        checked(transfer(s->memory[1],(w+1)*sizeof(struct qp_control),&s->slots[w+1].control,sizeof(struct qp_control),true),"initialize worker control");
    char descriptor[]="pg-sina-plan.so\0pg_sina_plan";
    CsCSFDownloadInfo download={.Type=CS_CSF_VENDOR_SPECIFIC,.SubType=CS_SUBTYPE_VENDOR_SPECIFIC,.CSEEId=1,.Index=510};
    strcpy(download.common.UniqueName,"pg-sina-plan"); download.common.DataBuffer=descriptor; download.common.Length=sizeof(descriptor);
    u32 id; checked(csCSFDownload(s->device,&download,&id),"download"); s->loaded=true;
    CsConfigInfo config={.Type=CS_CSF_ACTIVATE,.u.CSFActivateInfo={.CSFId=510,.EEIId=1,.State=CS_ACTIVE_STATE}};
    CsConfigData result; length=sizeof(result); checked(csConfig(s->device,&length,&config,&result),"activate"); s->active=true;
    length=0; int count=0; CS_STATUS status=csGetCSFId(s->device,"pg-sina-plan",0,&length,&count,NULL);
    if(status!=CS_LENGTH_RETURNED) checked(status,"lookup function length");
    CSFIdInfo *functions=palloc(length); checked(csGetCSFId(s->device,"pg-sina-plan",0,&length,&count,functions),"lookup function");
    s->function=functions[0].CSFId; pfree(functions);
    size_t size=sizeof(CsBatchRequest)+6*sizeof(CsComputeArg); CsBatchRequest *request=palloc0(size);
    request->ReqType=CS_QUEUE_COMPUTE; request->u.Compute.CSFId=s->function; request->u.Compute.NumArgs=6;
    for(unsigned i=0;i<5;++i) csHelperSetComputeArg(&request->u.Compute.Args[i],CS_AFDM_TYPE,s->memory[i],0UL);
    for(unsigned w=0;w<=s->workers;++w) {
        csHelperSetComputeArg(&request->u.Compute.Args[5],CS_64BIT_VALUE_TYPE,(u64)(s->profiling|(w<<1)));
        checked(csAllocBatchRequest(CS_BATCH_SERIAL,1,&s->slots[w].batch),"allocate batch");
        CS_BATCH_INDEX index; checked(csConfigureBatchEntry(s->slots[w].batch,CS_BATCH_ADD,request,0,0,&index),"configure batch");
        checked(csCreateEvent(&s->slots[w].event),"create event");
    }
    pfree(request); s->started=true;
}
static void append_ranges(CsBlockIo *io,const struct qs_request *file,const struct qs_extent *extents,uint64_t at,uint64_t end)
{
    for(unsigned e=0;e<file->nextents && at<end;++e) {
        if(extents[e].logical+extents[e].bytes<=at) continue;
        if(extents[e].logical>at) elog(ERROR,"CSD unmapped heap/index page");
        uint64_t n=Min(end,extents[e].logical+extents[e].bytes)-at;
        CSBlockRange range={file->nsid,extents[e].lba+(at-extents[e].logical)/file->lba_bytes,n/file->lba_bytes};
        CSBlockRange *last=io->NumRanges?&io->Range[io->NumRanges-1]:NULL;
        if(last && last->NamespaceId==range.NamespaceId && last->StartLba+last->NumBlocks==range.StartLba) last->NumBlocks+=range.NumBlocks;
        else io->Range[io->NumRanges++]=range;
        at+=n;
    }
    if(at!=end) elog(ERROR,"CSD incomplete extent mapping");
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
                append_ranges(io,file,extents,at,end);
            }
            configure_load(s,worker,load); ++s->load_calls; pfree(load);
        }
    } else {
        if(c->offset>s->files[node].bytes || c->bytes>s->files[node].bytes-c->offset) elog(ERROR,"invalid CSD page request");
        struct qs_request *file=&s->files[node];
        CsStorageRequest *load=palloc0(sizeof(*load)+(file->nextents+1)*sizeof(CSBlockRange));
        load->Mode=CS_STORAGE_BLOCK_IO; load->DevHandle=s->device;
        CsBlockIo *io=&load->u.BlockIo; io->Type=CS_STORAGE_LOAD_TYPE; io->DevMem=(CsDevAFDM){s->memory[3],input};
        append_ranges(io,file,s->extents[node],c->offset,c->offset+c->bytes);
        configure_load(s,worker,load); ++s->load_calls; pfree(load);
    }
    c->ready=1; s->load_ns+=clock_ns()-started;
}
static void submit(PlanStateCsd *s,unsigned worker)
{
    struct plan_slot *slot=&s->slots[worker];
    if((slot->control.bytes || slot->control.index_nreads) && !slot->control.ready) {
        unsigned groups=1;
        for(unsigned j=1;j<slot->control.index_nreads;++j)
            groups+=slot->control.index_reads[j].slot!=slot->control.index_reads[j-1].slot+1;
        if(groups<64) checked(csAllocBatchRequest(CS_BATCH_SERIAL,groups+1,&slot->resume),"allocate LOAD batch");
        slot->previous=0; load_pages(s,worker);
        checked(transfer(s->memory[1],worker*sizeof(struct qp_control)+offsetof(struct qp_control,ready),&slot->control.ready,sizeof(slot->control.ready),true),"resume LOAD");
        if(slot->resume) {
            CsBatchRequest *request=palloc0(sizeof(*request)+6*sizeof(CsComputeArg));
            request->ReqType=CS_QUEUE_COMPUTE; request->u.Compute.CSFId=s->function; request->u.Compute.NumArgs=6;
            for(unsigned i=0;i<5;++i) csHelperSetComputeArg(&request->u.Compute.Args[i],CS_AFDM_TYPE,s->memory[i],0UL);
            csHelperSetComputeArg(&request->u.Compute.Args[5],CS_64BIT_VALUE_TYPE,(u64)(s->profiling|(worker<<1)));
            CS_BATCH_INDEX current;
            checked(csConfigureBatchEntry(slot->resume,CS_BATCH_ADD,request,0,slot->previous,&current),"configure resumed compute"); pfree(request);
        }
    }
    if(!worker && slot->control.available)
        checked(transfer(s->memory[1],offsetof(struct qp_control,available),&slot->control.available,sizeof(slot->control.available),true),"provide worker rows");
    CS_STATUS status=csQueueBatchRequest(slot->resume?slot->resume:slot->batch,NULL,NULL,slot->event,&slot->pending,NULL);
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
            HOLD_INTERRUPTS(); CS_STATUS status=csPollEvent(slot->event,NULL,NULL);
            if(status==CS_NOT_DONE) { RESUME_INTERRUPTS(); continue; }
            slot->pending=NULL;
            CS_STATUS copied=transfer(s->memory[1],w*sizeof(struct qp_control),&slot->control,sizeof(slot->control),false);
            if(status==CS_SUCCESS) status=copied;
            struct qs_result header;
            if(!w) {
                memset(&header,0,offsetof(struct qs_result,data));
                if(status==CS_SUCCESS && (slot->control.output_rows || slot->control.finished))
                    status=transfer(s->memory[2],0,&header,offsetof(struct qs_result,data),false);
            }
            RESUME_INTERRUPTS(); CHECK_FOR_INTERRUPTS();
            if(status!=CS_SUCCESS && slot->control.error)
                ereport(ERROR,(errmsg("CSD PG Plan node %u failed: device=%d, status=%d",slot->control.error_node,slot->control.error,status)));
            checked(status,"execute tree"); progress=true;
            if(slot->resume) { checked(csFreeBatchRequest(slot->resume),"release LOAD batch"); slot->resume=NULL; }
            if(w) { slot->ready=slot->control.output_rows || slot->control.finished; slot->done=slot->control.finished; continue; }
            memcpy(s->result,&header,offsetof(struct qs_result,data));
            for(unsigned i=1;i<=s->workers;++i) if(slot->control.drained&(1U<<(i-1))) s->slots[i].ready=false;
            s->parallel=slot->control.parallel;
            if(slot->control.epoch!=s->epoch) {
                s->epoch=slot->control.epoch;
                for(unsigned i=1;i<=s->workers;++i) s->slots[i].ready=s->slots[i].done=false;
                for(unsigned i=1;i<=s->workers;++i)
                    checked(transfer(s->memory[1],i*sizeof(struct qp_control),&s->slots[i].control,sizeof(struct qp_control),false),"read worker state");
            }
            if(s->result->rows>QS_RESULT_ROWS) elog(ERROR,"invalid CSD result page");
            if(s->result->rows) {
                checked(transfer(s->memory[2],offsetof(struct qs_result,data),s->result->data,s->result->rows*sizeof(struct qs_row),false),"read result");
                checked(transfer(s->memory[2],offsetof(struct qs_result,str_bytes),&s->result->str_bytes,
                    sizeof(s->result->str_bytes)+sizeof(s->result->str_data),false),"read result strings");
            }
            if(s->result->rows || s->result->finished) { s->row=0; return; }
        }
        if(s->parallel) for(unsigned w=1;w<=s->workers;++w) {
            struct plan_slot *slot=&s->slots[w];
            if(!slot->pending && !slot->ready && !slot->done) { HOLD_INTERRUPTS(); submit(s,w); RESUME_INTERRUPTS(); progress=true; }
        }
        struct plan_slot *main=&s->slots[0];
        if(!main->pending) {
            unsigned ready=0;
            for(unsigned w=1;w<=s->workers;++w) if(s->slots[w].ready) ready|=1U<<(w-1);
            if(!s->parallel || ready || !main->control.parallel) {
                main->control.available=ready;
                HOLD_INTERRUPTS(); submit(s,0); RESUME_INTERRUPTS(); progress=true;
            }
        }
        if(!progress) pg_usleep(50L);
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
    ExplainPropertyText("CSD Plan Source","PostgreSQL selected Plan; original join tree",es);
    ExplainPropertyText("CSD Intermediate Data","device workmem and internal spill; 128-bit values",es);
    static const char *names[]={"Scan","Projection","Merge Join","Aggregate","Sort","Limit","Index Join","CTE Scan","CTE materialization","IN hash set"};
    StringInfoData tree; initStringInfo(&tree);
    for(unsigned i=0;i<s->plan->count;++i) {
        const struct qp_node *n=&s->plan->nodes[i];
        if(i) appendStringInfoString(&tree,"; ");
        appendStringInfo(&tree,"%u:%s",i,n->kind==QP_JOIN && n->hash?"Hash Join":names[n->kind]);
        if(n->kind==QP_SCAN) appendStringInfo(&tree,"(%s%s)",get_rel_name(n->expr.relid),n->ordered_index?(n->ordered_index&2?", index ordered, heap read-ahead":", index ordered"):"");
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
        ExplainPropertyInteger("CSD Heap Rows",NULL,s->result->metrics.scanned,es);
        ExplainPropertyInteger("CSD Internal Read","bytes",s->result->metrics.input_bytes,es);
        ExplainPropertyInteger("CSD Index/Heap Page Loads",NULL,s->result->metrics.index_pages,es);
        ExplainPropertyInteger("CSD Indexed Heap Tuples",NULL,s->result->metrics.index_tuples,es);
        ExplainPropertyInteger("CSD Workmem Peak","bytes",s->result->metrics.work_peak,es);
        ExplainPropertyInteger("CSD Workers",NULL,s->result->metrics.workers,es);
        ExplainPropertyInteger("CSD Parallel Pipelines",NULL,s->slots[0].control.epoch,es);
        ExplainPropertyInteger("CSD Scan Block","bytes",s->plan->scan_bytes,es);
        ExplainPropertyInteger("CSD Spill Read","bytes",s->result->metrics.spill_read_bytes,es);
        ExplainPropertyInteger("CSD Spill Write","bytes",s->result->metrics.spill_write_bytes,es);
        ExplainPropertyInteger("CSD Runs",NULL,s->result->metrics.runs,es);
        if(s->profiling) {
            const struct qs_timing *t=&s->result->profile.operators[QS_JOIN_PROBE];
            ExplainPropertyFloat("CSD Tree Wall","ms",(clock_ns()-s->start_ns)/1e6,3,es);
            ExplainPropertyFloat("CSD CSF Wall","ms",t->wall_ns/1e6,3,es);
            ExplainPropertyFloat("CSD CSF CPU","ms",t->cpu_ns/1e6,3,es);
            ExplainPropertyInteger("CSD CSF Calls",NULL,t->calls,es);
            ExplainPropertyInteger("CSD Active CUs Peak",NULL,s->slots[0].control.peak_cus,es);
            static const unsigned ops[]={QS_SCAN,QS_FILTER,QS_SORT_SINK,QS_SORT_SOURCE};
            static const char *labels[]={"CSD Scan Decode CPU","CSD Vector Filter CPU","CSD Sort Sink CPU","CSD Sort Source CPU"};
            for(unsigned i=0;i<4;++i) ExplainPropertyFloat(labels[i],"ms",s->result->profile.operators[ops[i]].cpu_ns/1e6,3,es);
            ExplainPropertyFloat("CSD LOAD Preparation","ms",s->load_ns/1e6,3,es);
            ExplainPropertyInteger("CSD Host LOAD Requests",NULL,s->load_calls,es);
        }
    }
}
static const CustomExecMethods methods={.CustomName="SniaCsdPlan",.BeginCustomScan=begin,.ExecCustomScan=execute,
    .EndCustomScan=end,.ReScanCustomScan=rescan,.ExplainCustomScan=explain};
const CustomScanMethods pg_sina_plan_methods={.CustomName="SniaCsdPlan",.CreateCustomScanState=create};
