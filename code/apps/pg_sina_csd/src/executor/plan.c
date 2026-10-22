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

typedef struct {
    CustomScanState css;
    struct qp_plan *plan;
    struct qp_control control;
    struct qs_request files[QP_NODES];
    struct qs_extent *extents[QP_NODES];
    struct qs_request indexes[QP_NODES];
    struct qs_extent *index_extents[QP_NODES];
    struct qs_result *result;
    CS_DEV_HANDLE device;
    CS_MEM_HANDLE memory[5];
    CS_BATCH_HANDLE batch;
    CS_EVT_HANDLE event;
    CS_REQ_HANDLE pending;
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
    if(s->pending) {
        csAbortRequest(s->pending);
        while(csPollEvent(s->event,NULL,NULL)==CS_NOT_DONE) pg_usleep(50L);
        s->pending=NULL;
    }
    if(s->device && s->batch && s->memory[1] && s->active) {
        unsigned close=1;
        /* A failed CSF may already have allocated state. Preserve its device
         * handle even when the failed call never copied control back to PG. */
        if(transfer(s->memory[1],offsetof(struct qp_control,close),&close,sizeof(close),true)==CS_SUCCESS)
            csQueueBatchRequest(s->batch,NULL,NULL,NULL,NULL,NULL);
    }
    if(s->batch) { csFreeBatchRequest(s->batch); s->batch=NULL; }
    if(s->event) { csDeleteEvent(s->event); s->event=NULL; }
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
        }
        for(unsigned col=0;col<n->expr.nattrs;++col)
            if(n->expr.cols[col].slot>=0 && n->expr.cols[col].type==Q_STRING && r->bytes>QS_DICT_SMALL_DATA) big=true;
    }
    if(!device) elog(ERROR,"CSD PG Plan has no prepared relation");
    s->profiling=pg_sina_profile; s->start_ns=clock_ns();
    s->plan->nodes[s->plan->root].expr.work_mem_bytes=(uint64_t)pg_sina_max_memory*1024;
    s->lock_fd=open(device,O_RDONLY|O_CLOEXEC);
    if(s->lock_fd<0) elog(ERROR,"cannot open CSD namespace");
    while(flock(s->lock_fd,LOCK_EX|LOCK_NB)) {
        if(errno!=EWOULDBLOCK && errno!=EINTR) elog(ERROR,"cannot lock CSD namespace");
        CHECK_FOR_INTERRUPTS(); pg_usleep(1000L);
    }
    checked(csOpenCSx(device,NULL,&s->device),"open");
    unsigned entries=big?QS_DICT_BIG_ENTRIES:QS_DICT_SMALL_ENTRIES,data=big?QS_DICT_BIG_DATA:QS_DICT_SMALL_DATA;
    size_t arena=(qs_dict_bytes(entries,data)+4095)&~(size_t)4095;
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_SCAN) {
        struct qp_node *n=&s->plan->nodes[i]; n->input_offset=arena; arena+=QP_SCAN_BYTES;
        n->vector_offset=arena; arena+=(qs_chunk_bytes(QP_SCAN_BYTES/24,n->expr.ncols)+4095)&~(size_t)4095;
    }
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_INDEX_JOIN) {
        struct qp_node *n=&s->plan->nodes[i]; n->input_offset=arena;
        arena+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE;
        n->vector_offset=arena;
        unsigned cols=s->plan->nodes[n->right].expr.ncols+2;
        arena+=4096+((qs_chunk_bytes(256,cols)+4095)&~(size_t)4095);
    }
    CsMemFlags flags={.FDMId=1};
    size_t sizes[]={sizeof(*s->plan),sizeof(s->control),sizeof(*s->result),arena,QS_SORT_IO_BYTES};
    for(unsigned i=0;i<5;++i) checked(csAllocMem(s->device,sizes[i],&flags,&s->memory[i],NULL),"allocate AFDM");
    checked(transfer(s->memory[0],0,s->plan,sizeof(*s->plan),true),"copy plan");
    checked(transfer(s->memory[1],0,&s->control,sizeof(s->control),true),"initialize control");
    struct qs_dict dictionary={.entries=entries,.capacity=entries,.data=data};
    checked(transfer(s->memory[3],0,&dictionary,sizeof(dictionary),true),"initialize strings");
    char zeros[4096]={0};
    size_t offset=sizeof(dictionary)+(size_t)entries*sizeof(struct qs_dict_entry);
    for(size_t i=0;i<(size_t)entries*4;i+=sizeof(zeros))
        checked(transfer(s->memory[3],offset+i,zeros,Min(sizeof(zeros),(size_t)entries*4-i),true),"clear string buckets");
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_SCAN) {
        struct qp_node *n=&s->plan->nodes[i];
        struct qs_chunk header={.capacity=QP_SCAN_BYTES/24,.ncols=n->expr.ncols};
        checked(transfer(s->memory[3],n->vector_offset,&header,sizeof(header),true),"initialize scan");
    }
    char descriptor[]="pg-sina-plan.so\0pg_sina_plan";
    CsCSFDownloadInfo download={.Type=CS_CSF_VENDOR_SPECIFIC,.SubType=CS_SUBTYPE_VENDOR_SPECIFIC,.CSEEId=1,.Index=510};
    strcpy(download.common.UniqueName,"pg-sina-plan"); download.common.DataBuffer=descriptor; download.common.Length=sizeof(descriptor);
    u32 id; checked(csCSFDownload(s->device,&download,&id),"download"); s->loaded=true;
    CsConfigInfo config={.Type=CS_CSF_ACTIVATE,.u.CSFActivateInfo={.CSFId=510,.EEIId=1,.State=CS_ACTIVE_STATE}};
    CsConfigData result; int length=sizeof(result); checked(csConfig(s->device,&length,&config,&result),"activate"); s->active=true;
    length=0; int count=0; CS_STATUS status=csGetCSFId(s->device,"pg-sina-plan",0,&length,&count,NULL);
    if(status!=CS_LENGTH_RETURNED) checked(status,"lookup function length");
    CSFIdInfo *functions=palloc(length); checked(csGetCSFId(s->device,"pg-sina-plan",0,&length,&count,functions),"lookup function");
    s->function=functions[0].CSFId; pfree(functions);
    size_t size=sizeof(CsBatchRequest)+6*sizeof(CsComputeArg); CsBatchRequest *request=palloc0(size);
    request->ReqType=CS_QUEUE_COMPUTE; request->u.Compute.CSFId=s->function; request->u.Compute.NumArgs=6;
    for(unsigned i=0;i<5;++i) csHelperSetComputeArg(&request->u.Compute.Args[i],CS_AFDM_TYPE,s->memory[i],0UL);
    csHelperSetComputeArg(&request->u.Compute.Args[5],CS_64BIT_VALUE_TYPE,(u64)s->profiling);
    checked(csAllocBatchRequest(CS_BATCH_SERIAL,1,&s->batch),"allocate batch");
    CS_BATCH_INDEX index; checked(csConfigureBatchEntry(s->batch,CS_BATCH_ADD,request,0,0,&index),"configure batch"); pfree(request);
    checked(csCreateEvent(&s->event),"create event"); s->started=true;
}
static void load_pages(PlanStateCsd *s)
{
    unsigned node=s->control.node;
    if(node>=s->plan->count) elog(ERROR,"invalid CSD page request");
    if(s->control.index_nreads) {
        if(s->control.index_nreads>QS_INDEX_READS || s->plan->nodes[node].kind!=QP_INDEX_JOIN) elog(ERROR,"invalid CSD index request");
        unsigned leaf=s->plan->nodes[node].right;
        for(unsigned j=0;j<s->control.index_nreads;++j) {
            const struct qs_index_read *read=&s->control.index_reads[j];
            struct qs_request *file=read->heap?&s->files[leaf]:&s->indexes[leaf];
            struct qs_extent *extents=read->heap?s->extents[leaf]:s->index_extents[leaf];
            uint64_t at=(uint64_t)read->block*QS_PAGE,end=at+QS_PAGE;
            if(read->heap>1 || read->slot>=QS_INDEX_CACHE || end>file->bytes) elog(ERROR,"invalid CSD index page");
            CsStorageRequest *load=palloc0(sizeof(*load)+(file->nextents+1)*sizeof(CSBlockRange));
            load->Mode=CS_STORAGE_BLOCK_IO; load->DevHandle=s->device;
            CsBlockIo *io=&load->u.BlockIo; io->Type=CS_STORAGE_LOAD_TYPE;
            io->DevMem=(CsDevAFDM){s->memory[3],s->plan->nodes[node].input_offset+QS_SORT_IO_BYTES+(size_t)read->slot*QS_PAGE};
            for(unsigned e=0;e<file->nextents && at<end;++e) {
                if(extents[e].logical+extents[e].bytes<=at) continue;
                uint64_t n=Min(end,extents[e].logical+extents[e].bytes)-at;
                io->Range[io->NumRanges++]=(CSBlockRange){file->nsid,extents[e].lba+(at-extents[e].logical)/file->lba_bytes,n/file->lba_bytes}; at+=n;
            }
            checked(csQueueStorageRequest(load,NULL,NULL,NULL,NULL,NULL),"index/heap LOAD"); pfree(load);
        }
        s->control.ready=1;
        checked(transfer(s->memory[1],0,&s->control,sizeof(s->control),true),"resume index");
        return;
    }
    if(node>=s->plan->count || s->control.offset>s->files[node].bytes ||
       s->control.bytes>s->files[node].bytes-s->control.offset) elog(ERROR,"invalid CSD page request");
    struct qs_request *r=&s->files[node]; uint64_t at=s->control.offset,end=at+s->control.bytes;
    size_t bytes=sizeof(CsStorageRequest)+(r->nextents+1)*sizeof(CSBlockRange);
    CsStorageRequest *load=palloc0(bytes);
    load->Mode=CS_STORAGE_BLOCK_IO; load->DevHandle=s->device;
    CsBlockIo *io=&load->u.BlockIo; io->Type=CS_STORAGE_LOAD_TYPE;
    io->DevMem=(CsDevAFDM){s->memory[3],s->plan->nodes[node].input_offset};
    for(unsigned i=0;i<r->nextents && at<end;++i) {
        const struct qs_extent *e=&s->extents[node][i];
        if(e->logical+e->bytes<=at) continue;
        uint64_t n=Min(e->logical+e->bytes,end)-at;
        io->Range[io->NumRanges++]=(CSBlockRange){r->nsid,e->lba+(at-e->logical)/r->lba_bytes,n/r->lba_bytes}; at+=n;
    }
    checked(csQueueStorageRequest(load,NULL,NULL,NULL,NULL,NULL),"internal LOAD"); pfree(load);
    s->control.ready=1;
    checked(transfer(s->memory[1],0,&s->control,sizeof(s->control),true),"resume scan");
}
static void fetch(PlanStateCsd *s)
{
    for(;;) {
        CHECK_FOR_INTERRUPTS();
        if((s->control.bytes || s->control.index_nreads) && !s->control.ready) load_pages(s);
        HOLD_INTERRUPTS();
        CS_STATUS status=csQueueBatchRequest(s->batch,NULL,NULL,s->event,&s->pending,NULL);
        if(status==CS_QUEUED) {
            do {
                status=csPollEvent(s->event,NULL,NULL);
                if(status==CS_NOT_DONE && !(QueryCancelPending || ProcDiePending)) pg_usleep(50L);
                else if(status==CS_NOT_DONE) csAbortRequest(s->pending);
            } while(status==CS_NOT_DONE);
        }
        s->pending=NULL;
        CS_STATUS control_status=transfer(s->memory[1],0,&s->control,sizeof(s->control),false);
        if(status==CS_SUCCESS) status=control_status;
        if(status==CS_SUCCESS) status=transfer(s->memory[2],0,s->result,offsetof(struct qs_result,data),false);
        RESUME_INTERRUPTS(); CHECK_FOR_INTERRUPTS();
        if(status!=CS_SUCCESS && s->control.error)
            ereport(ERROR,(errmsg("CSD PG Plan node %u failed: device=%d, status=%d",s->control.error_node,s->control.error,status)));
        checked(status,"execute tree");
        if(s->result->rows>QS_RESULT_ROWS) elog(ERROR,"invalid CSD result page");
        if(s->result->rows) {
            checked(transfer(s->memory[2],offsetof(struct qs_result,data),s->result->data,s->result->rows*sizeof(struct qs_row),false),"read result");
            checked(transfer(s->memory[2],offsetof(struct qs_result,str_bytes),&s->result->str_bytes,
                sizeof(s->result->str_bytes)+sizeof(s->result->str_data),false),"read result strings");
        }
        if(s->result->rows || s->result->finished) { s->row=0; return; }
        if(!s->control.bytes && !s->control.index_nreads) elog(ERROR,"CSD tree made no progress");
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
    memset(&s->control,0,sizeof(s->control)); memset(s->result,0,sizeof(*s->result)); s->started=false; s->row=0;
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
        if(n->kind==QP_SCAN) appendStringInfo(&tree,"(%s)",get_rel_name(n->expr.relid));
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
        ExplainPropertyInteger("CSD Workers",NULL,1,es);
        ExplainPropertyInteger("CSD Spill Read","bytes",s->result->metrics.spill_read_bytes,es);
        ExplainPropertyInteger("CSD Spill Write","bytes",s->result->metrics.spill_write_bytes,es);
        ExplainPropertyInteger("CSD Runs",NULL,s->result->metrics.runs,es);
        if(s->profiling) {
            const struct qs_timing *t=&s->result->profile.operators[QS_JOIN_PROBE];
            ExplainPropertyFloat("CSD Tree Wall","ms",(clock_ns()-s->start_ns)/1e6,3,es);
            ExplainPropertyFloat("CSD CSF Wall","ms",t->wall_ns/1e6,3,es);
            ExplainPropertyFloat("CSD CSF CPU","ms",t->cpu_ns/1e6,3,es);
            ExplainPropertyInteger("CSD CSF Calls",NULL,t->calls,es);
        }
    }
}
static const CustomExecMethods methods={.CustomName="SniaCsdPlan",.BeginCustomScan=begin,.ExecCustomScan=execute,
    .EndCustomScan=end,.ReScanCustomScan=rescan,.ExplainCustomScan=explain};
const CustomScanMethods pg_sina_plan_methods={.CustomName="SniaCsdPlan",.CreateCustomScanState=create};
