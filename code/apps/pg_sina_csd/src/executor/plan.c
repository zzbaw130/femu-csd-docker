/* One direct SNIA session for the selected PG tree; no per-subplan locks. */
#include "pg_internal.h"
#include "cs.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "access/parallel.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/resowner.h"
#include "utils/date.h"
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>

typedef struct PlanStateCsd PlanStateCsd;
/* Data only: native PG participants consume this queue. It contains no
 * device handles, elected submitter or PG worker-owned device state. */
struct delivery_timing {
    uint64_t lock_ns,copy_ns,empty_ns,full_ns,advance_ns,claims;
};
struct result_delivery {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    unsigned head,count,position;
    bool eof,stopped;
    CS_STATUS status;
    uint64_t result_bytes,result_reads,result_rows,fdm_bytes;
    unsigned workers,scan_bytes;
    struct pg_sina_resources resources;
    struct delivery_timing timing;
    struct qs_result batches[2];
};
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
    bool ready, done, control_copied, yielded;
    uint64_t retry_progress;
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
    List *parameter_exprs;
    struct qs_cell *host_parameters;
    struct plan_slot slots[QS_WORKERS+1];
    unsigned workers, epoch, device_cus;
    atomic_uint_fast64_t next_scan_offset;
    bool async_scan, delivery_shared, delivery_owner;
    struct result_delivery *delivery;
    dsm_segment *delivery_segment;
    /* Local callback serialization is separate from the shared data queue:
     * device requests must never hold up consumers of a published batch. */
    pthread_mutex_t advance_mutex;
    bool advance_mutex_ready;
    atomic_uint callbacks;
    CS_STATUS execution_status;
    const char *execution_operation;
    atomic_uint_fast64_t batch_progress;
    unsigned retry_worker;
    unsigned next_result_worker;
    atomic_bool stopping;
    bool parallel;
    atomic_uint_fast64_t load_ns, load_calls, completion_reads;
    uint64_t control_write_ns, control_writes;
    /* Queue time is within PG submit; LOAD preparation also includes
     * callbacks. Pending/requeue times sum concurrent requests, not latency. */
    uint64_t poll_ns, completion_copy_ns, submit_ns, queue_ns, idle_ns, pending_ns;
    uint64_t submits, polls, completions;
    uint64_t notice_ns, notice_max_ns;
    uint64_t barrier_started,barrier_ns,barriers;
    uint64_t setup_ns, cleanup_ns, result_read_ns;
    atomic_uint_fast64_t control_read_ns;
    struct delivery_timing delivery_timing;
    uint64_t fdm_bytes,result_bytes,result_reads,result_rows;
    struct qs_request files[QP_NODES];
    struct qs_extent *extents[QP_NODES];
    struct qs_request indexes[QP_NODES];
    struct qs_extent *index_extents[QP_NODES];
    struct qs_result *result;
    /* Batch-local column views: resolve the typed layout once, not for each
     * value exposed through PG's row-at-a-time TupleTableSlot interface. */
    struct {
        const unsigned char *values,*nulls;
        unsigned width,type;
    } result_columns[QS_OUT];
    CS_DEV_HANDLE device;
    CS_MEM_HANDLE memory[5];
    CS_MEM_HANDLE worker_io[QS_WORKERS];
    CS_CSF_ID function;
    unsigned program_index;
    unsigned row;
    bool started, explain_only, loaded, active, profiling;
    uint64_t start_ns;
    ResourceOwner resource_owner;
    MemoryContextCallback cleanup;
};
static const CustomExecMethods methods;
static void shutdown_scan(CustomScanState *node);
static void detach_delivery(dsm_segment *segment,Datum arg);
/* Mixed plans can keep several device streams open at once. Each program
 * retains its own device-global ID until EOF/rescan/error cleanup. */
static bool program_slots[11];
static uint64_t clock_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }
static void deliver(PlanStateCsd *s,bool callback);
static CS_STATUS queue_scan(struct plan_slot *slot);
static CS_STATUS queue_pages(struct plan_slot *slot);
static CS_STATUS transfer(CS_MEM_HANDLE,size_t,void *,size_t,bool);
/* A drained input with no output can reuse its buffers immediately. This
 * callback uses only libc, immutable mappings and SNIA, never PostgreSQL APIs.
 * Shared-table growth/spill still runs on the coordinator after workers stop. */
static void completed(void *context,CS_STATUS status,u64 value)
{
    struct plan_slot *slot=context;
    PlanStateCsd *s=slot->owner;
    atomic_fetch_add_explicit(&s->callbacks,1,memory_order_acq_rel);
    uint64_t arrived=slot->profiling?clock_ns():0;
    pthread_mutex_lock(&slot->mutex);
    if(status==CS_SUCCESS && slot->worker &&
       !atomic_load_explicit(&slot->owner->stopping,memory_order_acquire)) {
        if(value && !(value&QP_RESOURCE_WAIT)) atomic_fetch_add_explicit(&slot->owner->batch_progress,1,memory_order_release);
        uint64_t previous=slot->queued_ns;
        CS_STATUS next=CS_NOT_DONE;
        if(slot->scan_node) {
            if((value&31)==QP_INPUT_DRAINED) next=queue_scan(slot);
        } else {
            PlanStateCsd *s=slot->owner;
            if(value&QP_CONTROL_REQUIRED) {
                uint64_t reading=s->profiling?clock_ns():0;
                status=transfer(s->memory[1],slot->worker*sizeof(struct qp_control),
                    &slot->control,sizeof(slot->control),false);
                if(s->profiling) { ++s->completion_reads; s->control_read_ns+=clock_ns()-reading; }
            } else {
                memset(&slot->control,0,sizeof(slot->control));
                slot->control.output_rows=(value&QP_OUTPUT_READY)!=0;
                slot->control.finished=(value&QP_WORKER_FINISHED)!=0;
            }
            slot->control_copied=status==CS_SUCCESS;
            /* A full sink/output or EOF needs its coordinator. Pure input
             * waits and empty output batches can continue on this CU. */
            if(status==CS_SUCCESS && !(value&QP_RESOURCE_WAIT) && !slot->control.output_rows && !slot->control.finished && !slot->control.error)
                next=queue_pages(slot);
        }
        if(next==CS_QUEUED) {
            if(slot->profiling) {
                uint64_t gap=slot->queued_ns-arrived;
                ++slot->auto_submits; slot->auto_gap_ns+=gap;
                slot->auto_gap_max_ns=Max(slot->auto_gap_max_ns,gap);
                slot->auto_pending_ns+=arrived-previous;
            }
            pthread_mutex_unlock(&slot->mutex);
            atomic_fetch_sub_explicit(&s->callbacks,1,memory_order_release);
            return;
        }
        if(next!=CS_NOT_DONE) status=next;
    }
    slot->callback_ns=arrived;
    slot->completion_status=status; slot->completion_value=value;
    pthread_mutex_unlock(&slot->mutex);
    atomic_store_explicit(&slot->completed,true,memory_order_release);
    if(s->async_scan) deliver(s,true);
    atomic_fetch_sub_explicit(&s->callbacks,1,memory_order_release);
}
static CS_STATUS transfer(CS_MEM_HANDLE memory,size_t offset,void *data,size_t bytes,bool write)
{
    /* Keep large plan uploads bounded; one projected result batch fits in
     * a single request instead of three independent 64KiB requests. */
    for(size_t done=0;done<bytes;) {
        size_t n=Min((size_t)256*1024,bytes-done);
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
    if(s->delivery_shared && !s->delivery_owner) { RESUME_INTERRUPTS(); return; }
    if(s->delivery) pthread_mutex_lock(&s->delivery->mutex);
    atomic_store_explicit(&s->stopping,true,memory_order_release);
    if(s->delivery) {
        s->delivery->stopped=true;
        pthread_cond_broadcast(&s->delivery->ready);
        pthread_mutex_unlock(&s->delivery->mutex);
    }
    if(s->advance_mutex_ready) {
        pthread_mutex_lock(&s->advance_mutex);
        pthread_mutex_unlock(&s->advance_mutex);
    }
    for(unsigned w=0;w<=s->workers;++w) if(s->slots[w].pending) {
        pthread_mutex_lock(&s->slots[w].mutex);
        if(!atomic_load_explicit(&s->slots[w].completed,memory_order_acquire)) csAbortRequest(s->slots[w].inflight);
        pthread_mutex_unlock(&s->slots[w].mutex);
        while(!atomic_load_explicit(&s->slots[w].completed,memory_order_acquire)) pg_usleep(50L);
        s->slots[w].pending=NULL;
    }
    /* completed is published before a callback advances peer slots. Wait for
     * that whole continuation too, before freeing plans, handles or mutexes. */
    while(atomic_load_explicit(&s->callbacks,memory_order_acquire)) pg_usleep(50L);
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
    if(s->program_index) {
        program_slots[s->program_index-pg_sina_program_index(500)]=false;
        s->program_index=0;
    }
    if(s->device) { csCloseCSx(s->device); s->device=NULL; }
    if(s->profiling) s->cleanup_ns+=clock_ns()-started;
    RESUME_INTERRUPTS();
}
/* Parallel workers can exit after ERROR without deleting their query memory
 * context. Release device state during PG resource-owner abort as well. */
static void release_resources(ResourceReleasePhase phase,bool isCommit,bool isTopLevel,void *arg)
{
    PlanStateCsd *s=arg;
    if(phase==RESOURCE_RELEASE_BEFORE_LOCKS && CurrentResourceOwner==s->resource_owner)
        close_state(s);
}
static void cleanup_state(void *arg)
{
    UnregisterResourceReleaseCallback(release_resources,arg);
    PlanStateCsd *s=arg;
    close_state(s);
    if(s->delivery_segment) {
        cancel_on_dsm_detach(s->delivery_segment,detach_delivery,PointerGetDatum(s));
        s->delivery_segment=NULL;
    }
    if(s->delivery && !s->delivery_shared) {
        pthread_cond_destroy(&s->delivery->ready); pthread_mutex_destroy(&s->delivery->mutex);
        free(s->delivery);
    }
    if(s->advance_mutex_ready) { pthread_mutex_destroy(&s->advance_mutex); s->advance_mutex_ready=false; }
    s->delivery=NULL;
}
static void begin(CustomScanState *node,EState *estate,int flags)
{
    PlanStateCsd *s=(void *)node; CustomScan *scan=(void *)node->ss.ps.plan;
    Const *constant=linitial_node(Const,scan->custom_private); bytea *bytes=DatumGetByteaP(constant->constvalue);
    size_t length=VARSIZE_ANY_EXHDR(bytes);
    if(length<offsetof(struct qp_plan,nodes) || length>sizeof(*s->plan)) elog(ERROR,"invalid CSD PG Plan");
    s->plan=palloc0(sizeof(*s->plan)); memcpy(s->plan,VARDATA_ANY(bytes),length);
    if(!s->plan->count || s->plan->count>QP_NODES || s->plan->root>=s->plan->count ||
       (length!=sizeof(*s->plan) && length!=qp_plan_constant_bytes(s->plan))) elog(ERROR,"invalid CSD PG Plan");
    s->plan->packed_result=QP_TYPED_RESULT;
    s->parameter_exprs=ExecInitExprList(scan->custom_exprs,(PlanState *)node);
    size_t parameter_bytes=qp_host_parameter_bytes(s->plan);
    if(parameter_bytes) s->host_parameters=(void *)TYPEALIGN(16,(uintptr_t)palloc0(parameter_bytes+15));
    const struct qs_plan *output=&s->plan->nodes[s->plan->root].expr;
    for(unsigned col=0;col<output->nout;++col) {
        s->result_columns[col].type=output->outputs[col].type_oid;
        s->result_columns[col].width=qp_host_width(output->outputs[col].type_oid);
    }
    s->result=(void *)TYPEALIGN(16,(uintptr_t)palloc0(sizeof(*s->result)+15));
    s->explain_only=flags&EXEC_FLAG_EXPLAIN_ONLY; s->profiling=pg_sina_profile;
    atomic_init(&s->stopping,false); atomic_init(&s->next_scan_offset,0);
    atomic_init(&s->callbacks,0); atomic_init(&s->control_read_ns,0);
    s->async_scan=pg_sina_exe_path==PG_SINA_FILTER && s->plan->nodes[s->plan->root].kind==QP_SCAN;
    atomic_init(&s->load_ns,0); atomic_init(&s->load_calls,0);
    atomic_init(&s->completion_reads,0); atomic_init(&s->batch_progress,0);
    s->resource_owner=CurrentResourceOwner;
    RegisterResourceReleaseCallback(release_resources,s);
    s->cleanup.func=cleanup_state; s->cleanup.arg=s;
    MemoryContextRegisterResetCallback(estate->es_query_cxt,&s->cleanup);

}
/* Only the query leader initializes a shared stream, before launching PG
 * consumers. Private streams use exactly the same SNIA continuation. */
static void initialize_delivery(PlanStateCsd *s,struct result_delivery *shared)
{
    struct result_delivery *q=shared?shared:calloc(1,sizeof(*q));
    if(!q) elog(ERROR,"cannot allocate CSD result delivery buffers");
    if(shared) memset(q,0,sizeof(*q));
    pthread_mutexattr_t mutex_attr; pthread_condattr_t cond_attr;
    pthread_mutexattr_init(&mutex_attr); pthread_condattr_init(&cond_attr);
    if(shared) {
        pthread_mutexattr_setpshared(&mutex_attr,PTHREAD_PROCESS_SHARED);
        pthread_condattr_setpshared(&cond_attr,PTHREAD_PROCESS_SHARED);
    }
    int status=pthread_mutex_init(&q->mutex,&mutex_attr);
    if(!status) {
        status=pthread_cond_init(&q->ready,&cond_attr);
        if(status) pthread_mutex_destroy(&q->mutex);
    }
    pthread_mutexattr_destroy(&mutex_attr); pthread_condattr_destroy(&cond_attr);
    if(status) { if(!shared) free(q); elog(ERROR,"cannot initialize CSD result delivery"); }
    s->delivery=q; s->delivery_shared=shared!=NULL; s->delivery_owner=true;
    if(!s->advance_mutex_ready) {
        if(pthread_mutex_init(&s->advance_mutex,NULL)) elog(ERROR,"cannot initialize CSD callback serialization");
        s->advance_mutex_ready=true;
    }
}
/* Compute offsets without allocating, so a larger I/O block can be tried
 * before reducing CU concurrency. Small relations need only their own bytes. */
static size_t layout_buffers(PlanStateCsd *s,const bool *indexed)
{
    size_t arena=(sizeof(struct qs_dict)+4095)&~(size_t)4095;
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_SCAN && !indexed[i]) {
        struct qp_node *n=&s->plan->nodes[i]; unsigned bytes=n->ordered_index?QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE:qp_scan_buffer_bytes(s->plan,n);
        n->input_offset=arena; arena+=bytes; n->vector_offset=arena;
        arena+=(qs_chunk_bytes(n->ordered_index?256:qp_scan_capacity(s->plan,n),n->expr.ncols)+4095)&~(size_t)4095;
    }
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_INDEX_JOIN) {
        struct qp_node *n=&s->plan->nodes[i]; n->input_offset=arena;
        arena+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE; n->vector_offset=arena;
        arena+=qs_index_input_bytes()+((qs_chunk_bytes(256,s->plan->nodes[n->right].expr.ncols+2)+4095)&~(size_t)4095);
    }
    size_t stride=0;
    if(s->workers>1) for(unsigned root=0;root<s->plan->count;++root) if(s->plan->nodes[root].parallel) {
        size_t end=arena;
        int source=qp_pipeline_source(s->plan,root);
        for(int id=root;id>=0;id=s->plan->nodes[id].left) {
            struct qp_node *n=&s->plan->nodes[id];
            if(id==source && n->kind!=QP_SCAN) break;
            if(n->kind==QP_SCAN) {
                unsigned bytes=n->ordered_index?QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE:qp_scan_buffer_bytes(s->plan,n);
                n->parallel_input=end; end+=bytes; n->parallel_vector=end;
                end+=(qs_chunk_bytes(n->ordered_index?256:qp_scan_capacity(s->plan,n),n->expr.ncols)+4095)&~(size_t)4095; break;
            }
            if(n->kind==QP_INDEX_JOIN) {
                n->parallel_input=end; end+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE; n->parallel_vector=end;
                end+=qs_index_input_bytes()+((qs_chunk_bytes(256,s->plan->nodes[n->right].expr.ncols+2)+4095)&~(size_t)4095);
            }
        }
        stride=Max(stride,end-arena);
    }
    s->plan->parallel_stride=stride; s->plan->workers=s->workers?s->workers:1;
    return arena+(s->workers?s->workers-1:0)*stride;
}
/* Preserve finite PG Numeric values as scaled int128, never through double
 * or the int64 literal representation. NULL has no payload to dereference. */
struct qs_cell pg_sina_parameter_cell(Datum value,Oid type,bool isnull)
{
    struct qs_cell result={.isnull=isnull};
    if(isnull) return result;
    switch(type) {
    case INT2OID:result.value=DatumGetInt16(value);break;
    case INT4OID:result.value=DatumGetInt32(value);break;
    case INT8OID:result.value=DatumGetInt64(value);break;
    case BOOLOID:result.value=DatumGetBool(value);break;
    case DATEOID:
        result.value=DatumGetInt32(value);
        if(DATE_NOT_FINITE((int32)result.value)) elog(ERROR,"non-finite HOST date parameter is unsupported");
        break;
    case NUMERICOID: {
        char *text=DatumGetCString(DirectFunctionCall1(numeric_out,value));
        bool negative=*text=='-',point=false; unsigned scale=0;
        for(char *at=text+negative;*at;++at) {
            if(*at=='.' && !point) { point=true; continue; }
            if(*at<'0' || *at>'9' ||
               __builtin_mul_overflow(result.value,(__int128)10,&result.value) ||
               (negative?__builtin_sub_overflow(result.value,(__int128)(*at-'0'),&result.value):
                         __builtin_add_overflow(result.value,(__int128)(*at-'0'),&result.value)))
                elog(ERROR,"HOST numeric parameter exceeds the finite CSD int128 representation");
            if(point && ++scale>UINT8_MAX) elog(ERROR,"HOST numeric parameter scale exceeds the CSD representation");
        }
        result.scale=result.dscale=scale; pfree(text); break;
    }
    default:elog(ERROR,"unsupported HOST InitPlan parameter type %u",type);
    }
    return result;
}
static void start(PlanStateCsd *s)
{
    /* This runs only on the PG owner, before callbacks/consumers. Param
     * evaluation uses PG's own lazy InitPlan executor and dependency rules. */
    CustomScan *scan=(CustomScan *)s->css.ss.ps.plan;
    ListCell *parameter,*expression;
    forboth(parameter,scan->custom_exprs,expression,s->parameter_exprs) {
        Param *p=lfirst(parameter); bool isnull;
        Datum value=ExecEvalExprSwitchContext(lfirst(expression),s->css.ss.ps.ps_ExprContext,&isnull);
        s->host_parameters[p->paramid]=pg_sina_parameter_cell(value,p->paramtype,isnull);
    }
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
    size_t parameter_bytes=qp_host_parameter_bytes(s->plan);
    size_t plan_bytes=sizeof(*s->plan)+parameter_bytes;
    for(unsigned i=0;i<s->plan->count;++i) {
        const struct qs_request *files[]={&s->files[i],&s->indexes[i]};
        struct qp_file_mapping *maps[]={&s->plan->files[i],&s->plan->indexes[i]};
        for(unsigned k=0;k<2;++k) if(files[k]->nextents) {
            *maps[k]=(struct qp_file_mapping){.offset=plan_bytes,.nsid=files[k]->nsid,
                .lba_bytes=files[k]->lba_bytes,.count=files[k]->nextents};
            plan_bytes+=(size_t)files[k]->nextents*sizeof(struct qs_extent);
        }
    }
    s->profiling=pg_sina_profile; s->start_ns=clock_ns(); s->load_ns=s->load_calls=0;
    s->result_bytes=s->result_reads=s->result_rows=0;
    atomic_store(&s->stopping,false);
    atomic_store(&s->batch_progress,0); s->retry_worker=s->next_result_worker=0;
    s->poll_ns=s->completion_copy_ns=s->submit_ns=s->queue_ns=s->control_write_ns=0;
    s->idle_ns=s->pending_ns=s->submits=s->polls=s->completions=0;
    s->completion_reads=s->control_writes=0;
    s->notice_ns=s->notice_max_ns=0;
    s->barrier_started=s->barrier_ns=s->barriers=0;
    s->setup_ns=s->cleanup_ns=s->result_read_ns=0; s->control_read_ns=0;
    memset(s->plan->pipelines,0,sizeof(s->plan->pipelines));
    s->plan->nodes[s->plan->root].expr.work_mem_bytes=pg_sina_query_memory();
    for(unsigned i=0;i<lengthof(program_slots);++i) if(!program_slots[i]) {
        program_slots[i]=true; s->program_index=pg_sina_program_index(500+i); break;
    }
    if(!s->program_index) elog(ERROR,"too many simultaneously active CSD subtrees");
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
    if(parallel) s->workers=qp_task_slots(s->plan,s->device_cus);
    uint64_t capacity=pg_sina_resources.fdm_bytes;
    if(s->workers<=1) s->workers=0;
    s->plan->scan_bytes=QS_CHUNK;
    size_t arena,sizes[5];
    CsMemFlags flags={.FDMId=1};
    for(;;) {
        arena=layout_buffers(s,indexed);
        size_t needed=arena+plan_bytes+(s->workers+1)*(sizeof(struct qp_control)+sizeof(*s->result)+QS_SORT_IO_BYTES)+65536;
        if(capacity && needed>capacity) {
            if(s->plan->scan_bytes>QP_SCAN_BYTES) { s->plan->scan_bytes/=2; continue; }
            if(s->workers) { if(--s->workers==1) s->workers=0; continue; }
        }
        sizes[0]=plan_bytes; sizes[1]=(s->workers+1)*sizeof(struct qp_control);
        sizes[2]=(s->workers+1)*sizeof(*s->result); sizes[3]=arena; sizes[4]=QS_SORT_IO_BYTES;
        CS_STATUS status=CS_SUCCESS;
        for(unsigned i=0;i<5 && status==CS_SUCCESS;++i)
            status=csAllocMem(s->device,sizes[i],&flags,&s->memory[i],NULL);
        for(unsigned w=0;w<s->workers && status==CS_SUCCESS;++w)
            status=csAllocMem(s->device,QS_SORT_IO_BYTES,&flags,&s->worker_io[w],NULL);
        if(status==CS_SUCCESS) break;
        /* Concurrent HOST join inputs share physical AFDM. Retry a smaller
         * layout using actual allocation failure, not a fixed node quota. */
        for(unsigned i=0;i<5;++i) if(s->memory[i]) {
            csFreeMem(s->memory[i],&flags); s->memory[i]=NULL;
        }
        for(unsigned w=0;w<s->workers;++w) if(s->worker_io[w]) {
            csFreeMem(s->worker_io[w],&flags); s->worker_io[w]=NULL;
        }
        if(status!=CS_NOT_ENOUGH_MEMORY && status!=CS_OUT_OF_RESOURCES) checked(status,"allocate AFDM");
        if(s->plan->scan_bytes>QP_SCAN_BYTES) { s->plan->scan_bytes/=2; continue; }
        if(s->workers) { if(--s->workers==1) s->workers=0; continue; }
        checked(status,"allocate AFDM");
    }
    s->fdm_bytes=s->workers*QS_SORT_IO_BYTES;
    for(unsigned i=0;i<5;++i) s->fdm_bytes+=sizes[i];
    /* Independent unordered scans return CU batches directly. The same
     * AFDM allocation/layout is retained; no input is shared across scans. */
    const struct qp_node *root=&s->plan->nodes[s->plan->root];
    if(s->async_scan && s->workers>1 && !s->plan->ninit && !root->ordered_index)
        s->plan->packed_result=QP_DIRECT_RESULT;
    else s->plan->packed_result=QP_TYPED_RESULT;
    checked(transfer(s->memory[0],0,s->plan,sizeof(*s->plan),true),"copy plan");
    if(parameter_bytes) checked(transfer(s->memory[0],sizeof(*s->plan),s->host_parameters,parameter_bytes,true),"copy HOST parameters");
    for(unsigned i=0;i<s->plan->count;++i) {
        if(s->plan->files[i].count) checked(transfer(s->memory[0],s->plan->files[i].offset,s->extents[i],
            (size_t)s->plan->files[i].count*sizeof(struct qs_extent),true),"copy heap mapping");
        if(s->plan->indexes[i].count) checked(transfer(s->memory[0],s->plan->indexes[i].offset,s->index_extents[i],
            (size_t)s->plan->indexes[i].count*sizeof(struct qs_extent),true),"copy index mapping");
    }
    checked(transfer(s->memory[1],0,&s->slots[0].control,sizeof(s->slots[0].control),true),"initialize control");
    struct qs_dict dictionary={.entries=entries,.capacity=entries,.data=data};
    checked(transfer(s->memory[3],0,&dictionary,sizeof(dictionary),true),"initialize strings");
    for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_SCAN && !indexed[i]) {
        struct qp_node *n=&s->plan->nodes[i];
        struct qs_chunk header={.capacity=n->ordered_index?256:qp_scan_capacity(s->plan,n),.ncols=n->expr.ncols};
        checked(transfer(s->memory[3],n->vector_offset,&header,sizeof(header),true),"initialize scan");
    }
    for(unsigned w=0;w<s->workers;++w)
        checked(transfer(s->memory[1],(w+1)*sizeof(struct qp_control),&s->slots[w+1].control,sizeof(struct qp_control),true),"initialize worker control");
    char descriptor[]="pg-sina-plan.so\0pg_sina_plan";
    CsCSFDownloadInfo download={.Type=CS_CSF_VENDOR_SPECIFIC,.SubType=CS_SUBTYPE_VENDOR_SPECIFIC,.CSEEId=1,.Index=s->program_index};
    snprintf(download.common.UniqueName,sizeof(download.common.UniqueName),"pg-sina-plan-%u",s->program_index);
    download.common.DataBuffer=descriptor; download.common.Length=sizeof(descriptor);
    u32 id; checked(csCSFDownload(s->device,&download,&id),"download"); s->loaded=true;
    CsConfigInfo config={.Type=CS_CSF_ACTIVATE,.u.CSFActivateInfo={.CSFId=s->program_index,.EEIId=1,.State=CS_ACTIVE_STATE}};
    CsConfigData result; int length=sizeof(result); checked(csConfig(s->device,&length,&config,&result),"activate"); s->active=true;
    length=0; int count=0; CS_STATUS status=csGetCSFId(s->device,download.common.UniqueName,0,&length,&count,NULL);
    if(status!=CS_LENGTH_RETURNED) checked(status,"lookup function length");
    CSFIdInfo *functions=palloc(length); checked(csGetCSFId(s->device,download.common.UniqueName,0,&length,&count,functions),"lookup function");
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
    if(s->async_scan) {
        if(!s->delivery) initialize_delivery(s,NULL);
        struct result_delivery *q=s->delivery;
        q->fdm_bytes=s->fdm_bytes; q->workers=s->workers;
        q->scan_bytes=s->plan->scan_bytes; q->resources=pg_sina_resources;
        deliver(s,false); /* One initial submission; callbacks own all continuations. */
    }
    if(s->profiling) s->setup_ns=clock_ns()-s->start_ns;
}
static CS_STATUS append_ranges(CsBlockIo *io,const struct qs_request *file,const struct qs_extent *extents,uint64_t at,uint64_t end)
{
    unsigned lo=0,hi=file->nextents;
    while(lo<hi) {
        unsigned mid=lo+(hi-lo)/2;
        if(extents[mid].logical+extents[mid].bytes<=at) lo=mid+1; else hi=mid;
    }
    for(unsigned e=lo;e<file->nextents && at<end;++e) {
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
static CS_STATUS configure_load(PlanStateCsd *s,unsigned worker,CsBatchRequest *load)
{
    struct plan_slot *slot=&s->slots[worker];
    if(!slot->resume) return csQueueStorageRequest(&load->u.StorageIo,NULL,NULL,NULL,NULL,NULL);
    CS_BATCH_INDEX current;
    CS_STATUS status=csConfigureBatchEntry(slot->resume,CS_BATCH_ADD,load,0,slot->previous,&current);
    if(status==CS_SUCCESS) slot->previous=current;
    return status;
}
/* Uses immutable mappings and SNIA only: it is also safe in a callback. */
static CS_STATUS load_pages(PlanStateCsd *s,unsigned worker)
{
    struct qp_control *c=&s->slots[worker].control;
    unsigned node=c->node; uint64_t started=s->profiling?clock_ns():0;
    if(node>=s->plan->count) return CS_INVALID_ARG;
    const struct qp_node *n=&s->plan->nodes[node];
    size_t input=worker>1?n->parallel_input+(worker-2)*s->plan->parallel_stride:n->input_offset;
    if(c->index_nreads) {
        if(c->index_nreads>QS_INDEX_READS || (n->kind!=QP_INDEX_JOIN && !n->ordered_index)) return CS_INVALID_ARG;
        unsigned leaf=n->ordered_index?node:(unsigned)n->right,max_extents=Max(s->files[leaf].nextents,s->indexes[leaf].nextents);
        for(unsigned j=0;j<c->index_nreads;) {
            unsigned first=j++; const struct qs_index_read *read=&c->index_reads[first];
            while(j<c->index_nreads && c->index_reads[j].slot==read->slot+j-first) ++j;
            CsBatchRequest *load=calloc(1,sizeof(*load)+((j-first)*(max_extents+1))*sizeof(CSBlockRange));
            if(!load) return CS_OUT_OF_RESOURCES;
            load->ReqType=CS_STORAGE_IO;
            load->u.StorageIo.Mode=CS_STORAGE_BLOCK_IO; load->u.StorageIo.DevHandle=s->device;
            CsBlockIo *io=&load->u.StorageIo.u.BlockIo; io->Type=CS_STORAGE_LOAD_TYPE;
            io->DevMem=(CsDevAFDM){s->memory[3],input+QS_SORT_IO_BYTES+(size_t)read->slot*QS_PAGE};
            CS_STATUS status=CS_SUCCESS;
            for(unsigned k=first;k<j;++k) {
                read=&c->index_reads[k]; const struct qs_request *file=read->heap?&s->files[leaf]:&s->indexes[leaf];
                const struct qs_extent *extents=read->heap?s->extents[leaf]:s->index_extents[leaf];
                uint64_t at=(uint64_t)read->block*QS_PAGE,end=at+QS_PAGE;
                if(read->heap>1 || read->slot>=QS_INDEX_CACHE || end>file->bytes) { status=CS_INVALID_ARG; break; }
                status=append_ranges(io,file,extents,at,end); if(status!=CS_SUCCESS) break;
            }
            if(status==CS_SUCCESS) status=configure_load(s,worker,load);
            free(load); if(status!=CS_SUCCESS) return status;
            if(s->profiling) ++s->load_calls;
        }
    } else {
        if(c->offset>s->files[node].bytes || c->bytes>s->files[node].bytes-c->offset) return CS_INVALID_ARG;
        const struct qs_request *file=&s->files[node];
        CsBatchRequest *load=calloc(1,sizeof(*load)+(file->nextents+1)*sizeof(CSBlockRange));
        if(!load) return CS_OUT_OF_RESOURCES;
        load->ReqType=CS_STORAGE_IO;
        load->u.StorageIo.Mode=CS_STORAGE_BLOCK_IO; load->u.StorageIo.DevHandle=s->device;
        CsBlockIo *io=&load->u.StorageIo.u.BlockIo; io->Type=CS_STORAGE_LOAD_TYPE; io->DevMem=(CsDevAFDM){s->memory[3],input};
        CS_STATUS status=append_ranges(io,file,s->extents[node],c->offset,c->offset+c->bytes);
        if(status==CS_SUCCESS) status=configure_load(s,worker,load);
        free(load); if(status!=CS_SUCCESS) return status;
        if(s->profiling) ++s->load_calls;
    }
    c->ready=1; if(s->profiling) s->load_ns+=clock_ns()-started;
    return CS_SUCCESS;
}
/* The completed batch no longer owns its snapshot. Keep one fresh LOAD ->
 * compute batch per continuation; the slot mutex protects cancellation. */
static CS_STATUS queue_pages(struct plan_slot *slot)
{
    PlanStateCsd *s=slot->owner; unsigned worker=slot->worker;
    CS_STATUS status; bool loaded=false;
    if(slot->resume) {
        status=csFreeBatchRequest(slot->resume); if(status!=CS_SUCCESS) return status;
        slot->resume=NULL;
    }
    if((slot->control.bytes || slot->control.index_nreads) && !slot->control.ready) {
        unsigned groups=1;
        for(unsigned j=1;j<slot->control.index_nreads;++j)
            groups+=slot->control.index_reads[j].slot!=slot->control.index_reads[j-1].slot+1;
        if(groups<64) {
            status=csAllocBatchRequest(CS_BATCH_SERIAL,groups+1,&slot->resume);
            if(status!=CS_SUCCESS) return status;
        }
        slot->previous=0;
        status=load_pages(s,worker); if(status!=CS_SUCCESS) return status;
        loaded=true;
        if(!slot->resume) {
            status=csAllocBatchRequest(CS_BATCH_SERIAL,1,&slot->resume);
            if(status!=CS_SUCCESS) return status;
        }
    }
    if(slot->resume) {
        CsBatchRequest *request=calloc(1,sizeof(*request)+6*sizeof(CsComputeArg));
        if(!request) return CS_OUT_OF_RESOURCES;
        request->ReqType=CS_QUEUE_COMPUTE; request->u.Compute.CSFId=s->function; request->u.Compute.NumArgs=6;
        for(unsigned i=0;i<5;++i) csHelperSetComputeArg(&request->u.Compute.Args[i],CS_AFDM_TYPE,s->memory[i],0UL);
        csHelperSetComputeArg(&request->u.Compute.Args[4],CS_AFDM_TYPE,worker?s->worker_io[worker-1]:s->memory[4],0UL);
        csHelperSetComputeArg(&request->u.Compute.Args[5],CS_64BIT_VALUE_TYPE,(u64)(s->profiling|(worker<<1)|(loaded?QP_PAGES_INPUT:0)));
        CS_BATCH_INDEX index;
        status=csConfigureBatchEntry(slot->resume,CS_BATCH_ADD,request,0,slot->previous,&index);
        free(request); if(status!=CS_SUCCESS) return status;
    }
    slot->control_copied=false;
    if(s->profiling) slot->queued_ns=clock_ns();
    return csQueueBatchRequest(slot->resume?slot->resume:slot->batch,slot,completed,NULL,&slot->inflight,NULL);
}
static CS_STATUS submit(PlanStateCsd *s,unsigned worker)
{
    uint64_t started=s->profiling?clock_ns():0;
    struct plan_slot *slot=&s->slots[worker];
    slot->yielded=false;
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
        return status==CS_QUEUED?CS_SUCCESS:status;
    }
    if(!worker && slot->control.available) {
        uint64_t written=s->profiling?clock_ns():0;
        CS_STATUS status=transfer(s->memory[1],offsetof(struct qp_control,available),&slot->control.available,sizeof(slot->control.available),true);
        if(status!=CS_SUCCESS) return status;
        if(s->profiling) { s->control_write_ns+=clock_ns()-written; ++s->control_writes; }
    }
    atomic_store_explicit(&slot->completed,false,memory_order_relaxed);
    pthread_mutex_lock(&slot->mutex);
    CS_STATUS status=queue_pages(slot);
    slot->pending=status==CS_QUEUED?slot->inflight:NULL;
    if(s->profiling) {
        uint64_t now=clock_ns();
        s->submit_ns+=now-started;
        if(status==CS_QUEUED) { s->queue_ns+=now-slot->queued_ns; ++s->submits; }
    }
    pthread_mutex_unlock(&slot->mutex);
    return status==CS_QUEUED?CS_SUCCESS:status;
}
/* Called either by PG for a synchronous tree, or by the existing SNIA
 * completion workers for a scan. No PostgreSQL API is used on this path. */
static int execution_failed(PlanStateCsd *s,CS_STATUS status,const char *operation)
{ s->execution_status=status; s->execution_operation=operation; return -1; }
static CS_STATUS read_result(PlanStateCsd *s,unsigned worker,const struct qp_control *control,struct qs_result *result)
{
    const struct qs_plan *output=&s->plan->nodes[s->plan->root].expr;
    if(control->output_rows>qp_host_capacity(output) || control->output_strings>sizeof(result->str_data))
        return CS_INVALID_LENGTH;
    size_t bytes=offsetof(struct qs_result,data)+qp_host_bytes(output,control->output_rows);
    bool packed=worker && s->plan->packed_result==QP_DIRECT_RESULT;
    if(packed) bytes+=control->output_strings;
    uint64_t reading=s->profiling?clock_ns():0;
    CS_STATUS status=transfer(s->memory[2],(size_t)worker*sizeof(*result),result,bytes,false);
    s->result_bytes+=bytes; ++s->result_reads; s->result_rows+=control->output_rows;
    if(status==CS_SUCCESS && result->rows!=control->output_rows) status=CS_INVALID_LENGTH;
    if(status==CS_SUCCESS && packed) qp_host_unpack(result,output,control->output_strings);
    else result->str_bytes=control->output_strings;
    if(status==CS_SUCCESS && !packed && result->rows && result->str_bytes) {
        status=transfer(s->memory[2],(size_t)worker*sizeof(*result)+offsetof(struct qs_result,str_data),
            result->str_data,result->str_bytes,false);
        s->result_bytes+=result->str_bytes; ++s->result_reads;
    }
    if(s->profiling) s->result_read_ns+=clock_ns()-reading;
    return status;
}
static int advance(PlanStateCsd *s,struct qs_result *result)
{
    bool direct=s->plan->packed_result==QP_DIRECT_RESULT;
    unsigned first=s->next_result_worker;
    for(unsigned step=0;step<=s->workers;++step) {
        unsigned w=step && direct?1+(first+step-1)%s->workers:step;
        struct plan_slot *slot=&s->slots[w];
        if(!slot->pending) continue;
        uint64_t polled=s->profiling?clock_ns():0;
        bool done=atomic_load_explicit(&slot->completed,memory_order_acquire);
        CS_STATUS status=done?slot->completion_status:CS_NOT_DONE;
        u64 value=done?slot->completion_value:0;
        uint64_t observed=s->profiling?clock_ns():0;
        if(s->profiling) { s->poll_ns+=observed-polled; ++s->polls; }
        if(status==CS_NOT_DONE) continue;
        if(s->profiling) {
            uint64_t notice=observed-slot->callback_ns;
            s->notice_ns+=notice; s->notice_max_ns=Max(s->notice_max_ns,notice);
            s->pending_ns+=observed-slot->queued_ns; ++s->completions;
        }
        slot->pending=NULL;
        if(w && slot->scan_node && status==CS_SUCCESS && !(value&QP_CONTROL_REQUIRED)) {
            slot->control.output_rows=value&QP_TYPED_OUTPUT?qp_output_rows(value):(value&QP_OUTPUT_READY)!=0;
            slot->control.output_strings=value&QP_TYPED_OUTPUT?qp_output_strings(value):0;
            slot->control.finished=(value&QP_WORKER_FINISHED)!=0;
            slot->need_input=(value&QP_INPUT_DRAINED)!=0;
        } else {
            if(!slot->control_copied) {
                uint64_t reading=s->profiling?clock_ns():0;
                CS_STATUS copied=transfer(s->memory[1],w*sizeof(struct qp_control),&slot->control,sizeof(slot->control),false);
                if(status==CS_SUCCESS) status=copied;
                if(s->profiling) { ++s->completion_reads; s->control_read_ns+=clock_ns()-reading; }
            }
            slot->control_copied=false;
        }
        if(!w) { memset(result,0,offsetof(struct qs_result,data)); result->str_bytes=0; }
        if(status==CS_SUCCESS && (!w || direct) && (slot->control.output_rows || slot->control.finished))
            status=read_result(s,w,&slot->control,result);
        if(s->profiling) s->completion_copy_ns+=clock_ns()-observed;
        if(status!=CS_SUCCESS) return execution_failed(s,status,"execute tree");
        if(slot->resume) {
            status=csFreeBatchRequest(slot->resume);
            if(status!=CS_SUCCESS) return execution_failed(s,status,"release LOAD batch");
            slot->resume=NULL;
        }
        if(w) {
            slot->ready=slot->control.output_rows || slot->control.finished; slot->done=slot->control.finished;
            slot->yielded=(value&QP_RESOURCE_WAIT)!=0;
            slot->retry_progress=atomic_load_explicit(&s->batch_progress,memory_order_acquire);
            if(direct) {
                /* Its packet is now HOST-owned, so this CU can reuse its own
                 * result/input without a coordinator acknowledgement. */
                slot->ready=false;
                result->finished=0; /* Only the coordinator's final EOF ends the stream. */
                bool publish=slot->control.output_rows!=0;
                if(!slot->done && !slot->yielded) {
                    status=submit(s,w);
                    if(status!=CS_SUCCESS) return execution_failed(s,status,"continue direct scan");
                }
                if(publish) {
                    s->next_result_worker=w%s->workers;
                    return 1;
                }
            }
            continue;
        }
        if(result->rows || result->finished || slot->control.drained)
            atomic_fetch_add_explicit(&s->batch_progress,1,memory_order_release);
        for(unsigned i=1;i<=s->workers;++i) if(slot->control.drained&(UINT64_C(1)<<(i-1))) s->slots[i].ready=false;
        s->parallel=slot->control.parallel;
        if(slot->control.epoch!=s->epoch) {
            s->epoch=slot->control.epoch;
            atomic_store(&s->next_scan_offset,0);
            for(unsigned i=1;i<=s->workers;++i) {
                struct plan_slot *worker=&s->slots[i];
                worker->ready=worker->done=worker->yielded=false;
                worker->scan_node=slot->control.scan_node;
                worker->need_input=worker->scan_node!=0;
                /* The device initializes each phase's worker control to
                 * zero except its private state pointer. PG never uses it. */
                memset(&worker->control,0,sizeof(worker->control));
            }
        }
        if(result->finished && s->profiling) {
            status=transfer(s->memory[0],offsetof(struct qp_plan,pipelines),s->plan->pipelines,sizeof(s->plan->pipelines),false);
            if(status!=CS_SUCCESS) return execution_failed(s,status,"read pipeline profile");
        }
        if(result->rows || result->finished) return 1;
    }
    if(s->parallel && !(s->slots[0].control.parallel==2 && s->slots[0].pending)) {
        uint64_t advanced=atomic_load_explicit(&s->batch_progress,memory_order_acquire);
        bool pending=s->slots[0].pending!=NULL;
        unsigned retry=0;
        for(unsigned w=1;w<=s->workers;++w) {
            struct plan_slot *slot=&s->slots[w];
            if(slot->pending) { pending=true; continue; }
            if(slot->ready || slot->done) continue;
            if(!slot->yielded || slot->retry_progress!=advanced) {
                CS_STATUS status=submit(s,w);
                if(status!=CS_SUCCESS) return execution_failed(s,status,"queue tree");
                pending=true;
            } else if(!retry || (retry<=s->retry_worker && w>s->retry_worker)) retry=w;
        }
        /* RESOURCE_WAIT retains its input but cannot currently
         * advance (for example, another CU owns dictionary I/O).
         * Wake it on peer progress, not every host poll. If all calls
         * yielded, retry one in turn so an empty batch cannot deadlock. */
        if(!pending && retry) {
            s->retry_worker=retry;
            CS_STATUS status=submit(s,retry);
            if(status!=CS_SUCCESS) return execution_failed(s,status,"queue tree");
        }
    }
    struct plan_slot *main=&s->slots[0];
    if(!main->pending) {
        uint64_t ready=0;
        for(unsigned w=1;w<=s->workers;++w)
            if(direct?s->slots[w].done:s->slots[w].ready) ready|=UINT64_C(1)<<(w-1);
        /* Direct build/aggregate sinks share a bounded table. Its owner
         * may resize/spill only after the wave is idle. A yielded CU
         * still retains input/update state, so it is not at that barrier. */
        bool idle=!direct || !s->parallel || ready==qp_worker_mask(s->workers);
        if(main->control.parallel==2)
            for(unsigned w=1;w<=s->workers;++w) {
                struct plan_slot *slot=&s->slots[w];
                if(slot->pending || (slot->yielded && !slot->ready && !slot->done)) idle=false;
            }
        if(s->profiling && main->control.parallel==2) {
            bool output=false;
            for(unsigned w=1;w<=s->workers;++w)
                if(s->slots[w].ready && s->slots[w].control.output_rows) output=true;
            if(!idle && output && !s->barrier_started) { s->barrier_started=clock_ns(); ++s->barriers; }
            if(idle && s->barrier_started) { s->barrier_ns+=clock_ns()-s->barrier_started; s->barrier_started=0; }
        }
        if(idle && (!s->parallel || ready || !main->control.parallel)) {
            main->control.available=ready;
            CS_STATUS status=submit(s,0);
            if(status!=CS_SUCCESS) return execution_failed(s,status,"queue tree");
        }
    }
    return 0;
}
/* A full queue parks an existing SNIA completion worker on a condition
 * variable. PG consumption only signals space; it never resumes a CSF or
 * submits LOAD. A sleeping callback occupies no CU; AFDM remains query-owned. */
static void deliver(PlanStateCsd *s,bool callback)
{
    struct result_delivery *q=s->delivery;
    pthread_mutex_lock(&s->advance_mutex);
    pthread_mutex_lock(&q->mutex);
    while(!atomic_load_explicit(&s->stopping,memory_order_acquire) &&
          s->execution_status==CS_SUCCESS && !q->eof) {
        if(q->count==lengthof(q->batches)) {
            if(!callback) break;
            uint64_t waiting=s->profiling?clock_ns():0;
            pthread_cond_wait(&q->ready,&q->mutex);
            if(s->profiling) q->timing.full_ns+=clock_ns()-waiting;
            continue;
        }
        struct qs_result *next=&q->batches[(q->head+q->count)%lengthof(q->batches)];
        /* The producer owns the unpublished tail. Consuming the head keeps
         * (head + count) unchanged, so this slot remains private while I/O
         * and request configuration run outside the shared queue lock. */
        pthread_mutex_unlock(&q->mutex);
        uint64_t advancing=s->profiling?clock_ns():0;
        int produced=advance(s,next);
        uint64_t advanced=s->profiling?clock_ns():0;
        pthread_mutex_lock(&q->mutex);
        if(s->profiling) q->timing.advance_ns+=advanced-advancing;
        q->status=s->execution_status;
        if(produced<=0) break;
        ++q->count; q->eof=next->finished;
        q->result_bytes=s->result_bytes; q->result_reads=s->result_reads; q->result_rows=s->result_rows;
        pthread_cond_broadcast(&q->ready);
    }
    if(q->status!=CS_SUCCESS) pthread_cond_broadcast(&q->ready);
    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_unlock(&s->advance_mutex);
}
static void fetch(PlanStateCsd *s)
{
    uint64_t empty=0;
    for(;;) {
        CHECK_FOR_INTERRUPTS();
        if(s->async_scan) {
            struct result_delivery *q=s->delivery;
            uint64_t waiting=s->profiling?clock_ns():0;
            pthread_mutex_lock(&q->mutex);
            uint64_t copying=s->profiling?clock_ns():0;
            if(s->profiling) { q->timing.lock_ns+=copying-waiting; q->timing.empty_ns+=empty; empty=0; }
            CS_STATUS status=q->status;
            bool have=q->count!=0 || q->eof || q->stopped;
            int copied=0;
            if(q->count) {
                struct qs_result *batch=&q->batches[q->head];
                unsigned rows=s->delivery_shared?Min(256U,batch->rows-q->position):batch->rows;
                memcpy(s->result,batch,offsetof(struct qs_result,data));
                if(s->delivery_shared)
                    copied=qp_host_slice(s->result,batch,&s->plan->nodes[s->plan->root].expr,q->position,rows);
                else {
                    memcpy(s->result->data,batch->data,qp_host_bytes(&s->plan->nodes[s->plan->root].expr,rows));
                    memcpy(s->result->str_data,batch->str_data,batch->str_bytes);
                    s->result->str_bytes=batch->str_bytes;
                }
                q->position+=rows;
                s->result->finished=batch->finished && q->position==batch->rows;
                if(q->position==batch->rows) {
                    q->head=(q->head+1)%lengthof(q->batches); --q->count; q->position=0;
                    pthread_cond_broadcast(&q->ready);
                }
            } else if(q->eof || q->stopped) { s->result->rows=0; s->result->finished=1; }
            if(have && s->profiling) { q->timing.copy_ns+=clock_ns()-copying; ++q->timing.claims; }
            pthread_mutex_unlock(&q->mutex);
            if(status!=CS_SUCCESS) checked(status,"scan completion");
            if(copied) elog(ERROR,"invalid CSD variable result slice");
            if(have) { s->row=0; return; }
        } else {
            HOLD_INTERRUPTS();
            int produced=advance(s,s->result);
            RESUME_INTERRUPTS();
            if(produced<0) checked(s->execution_status,s->execution_operation);
            if(produced>0) { s->row=0; return; }
        }
        uint64_t idle=s->profiling?clock_ns():0;
        pg_usleep(50L);
        if(s->profiling) { uint64_t slept=clock_ns()-idle; s->idle_ns+=slept; empty+=slept; }
    }
}
static TupleTableSlot *execute(CustomScanState *node)
{
    PlanStateCsd *s=(void *)node; TupleTableSlot *slot=node->ss.ss_ScanTupleSlot;
    if(s->explain_only) return ExecClearTuple(slot);
    const struct qs_plan *root=&s->plan->nodes[s->plan->root].expr;
    if(!s->started && !s->delivery_shared) start(s);
    if(s->row==s->result->rows && !s->result->finished) fetch(s);
    if(s->row==s->result->rows) { close_state(s); return ExecClearTuple(slot); }
    if(!s->row) for(unsigned i=0;i<root->nout;++i) {
        s->result_columns[i].values=qp_host_values(s->result,root,s->result->rows,i);
        s->result_columns[i].nulls=s->result_columns[i].values+(size_t)s->result->rows*s->result_columns[i].width;
    }
    ResetExprContext(node->ss.ps.ps_ExprContext); ExecClearTuple(slot);
    unsigned row=s->row++;
    for(unsigned i=0;i<root->nout;++i) {
        bool isnull=(s->result_columns[i].nulls[row/8]>>(row%8))&1;
        slot->tts_isnull[i]=isnull; if(isnull) continue;
        Oid type=s->result_columns[i].type;
        const void *field=s->result_columns[i].values+(size_t)row*s->result_columns[i].width;
        if(qp_host_variable(type)) {
            unsigned bytes; const void *value=qp_host_var(s->result,field,&bytes);
            if(!value) elog(ERROR,"invalid CSD variable result");
            slot->tts_values[i]=PointerGetDatum(value);
        } else if(type==FLOAT8OID) { double v; memcpy(&v,field,8); slot->tts_values[i]=Float8GetDatum(v); }
        else if(type==BOOLOID) slot->tts_values[i]=BoolGetDatum(*(const unsigned char *)field!=0);
        else if(type==INT2OID) { int16_t v; memcpy(&v,field,2); slot->tts_values[i]=Int16GetDatum(v); }
        else if(type==INT4OID || type==DATEOID) { int32_t v; memcpy(&v,field,4); slot->tts_values[i]=Int32GetDatum(v); }
        else if(type==INT8OID) { int64_t v; memcpy(&v,field,8); slot->tts_values[i]=Int64GetDatum(v); }
        else elog(ERROR,"unsupported CSD output type");
    }
    ExecStoreVirtualTuple(slot); node->ss.ps.ps_ExprContext->ecxt_scantuple=slot;
    return node->ss.ps.ps_ProjInfo?ExecProject(node->ss.ps.ps_ProjInfo):slot;
}
static void end(CustomScanState *node) { close_state(node); }
static Size estimate_dsm(CustomScanState *node,ParallelContext *pcxt)
{ return sizeof(struct result_delivery); }
static void initialize_dsm(CustomScanState *node,ParallelContext *pcxt,void *coordinate)
{
    PlanStateCsd *s=(void *)node;
    initialize_delivery(s,coordinate);
    /* PG releases DSM before extension ResourceOwner callbacks on ERROR.
     * Stop continuations while the queue is still mapped, including errors
     * during start() before InitializeDSMCustomScan returns. */
    s->delivery_segment=pcxt->seg;
    on_dsm_detach(pcxt->seg,detach_delivery,PointerGetDatum(s));
    start(s); /* Query leader, before any PG worker can consume the stream. */
}
static void detach_delivery(dsm_segment *segment,Datum arg)
{
    PlanStateCsd *s=(void *)DatumGetPointer(arg);
    s->delivery_segment=NULL;
    shutdown_scan((CustomScanState *)s);
}
static void initialize_worker(CustomScanState *node,shm_toc *toc,void *coordinate)
{
    PlanStateCsd *s=(void *)node;
    s->delivery=coordinate; s->delivery_shared=true; s->delivery_owner=false;
}
static void shutdown_scan(CustomScanState *node)
{
    PlanStateCsd *s=(void *)node;
    close_state(s); /* Drain leader callbacks before PG detaches shared memory. */
    if(s->delivery_segment) {
        cancel_on_dsm_detach(s->delivery_segment,detach_delivery,PointerGetDatum(s));
        s->delivery_segment=NULL;
    }
    if(s->delivery_shared && s->delivery) {
        struct result_delivery *q=s->delivery;
        pthread_mutex_lock(&q->mutex);
        pg_sina_resources=q->resources;
        s->fdm_bytes=q->fdm_bytes; s->workers=q->workers; s->plan->scan_bytes=q->scan_bytes;
        s->result_bytes=q->result_bytes; s->result_reads=q->result_reads; s->result_rows=q->result_rows;
        s->delivery_timing=q->timing;
        memcpy(s->result,&q->batches[(q->head+q->count+1)%2],offsetof(struct qs_result,data));
        pthread_mutex_unlock(&q->mutex);
        s->delivery=NULL;
    }
}
static void rescan(CustomScanState *node)
{
    PlanStateCsd *s=(void *)node; close_state(s);
    if(s->delivery && !s->delivery_shared) {
        s->delivery->head=s->delivery->count=s->delivery->position=0;
        s->delivery->eof=s->delivery->stopped=false; s->delivery->status=CS_SUCCESS;
        s->delivery->result_bytes=s->delivery->result_reads=s->delivery->result_rows=0;
        memset(&s->delivery->timing,0,sizeof(s->delivery->timing));
        memset(s->delivery->batches,0,sizeof(s->delivery->batches));
    }
    s->execution_status=CS_SUCCESS; s->execution_operation=NULL;
    memset(s->slots,0,sizeof(s->slots)); s->parallel=false; s->epoch=0; memset(s->result,0,sizeof(*s->result)); s->started=false; s->row=0;
}
static void explain(CustomScanState *node,List *ancestors,ExplainState *es)
{
    PlanStateCsd *s=(void *)node;
    /* EXPLAIN can inspect a LIMIT-stopped private stream before EndCustomScan.
     * Freeze callback-written counters before exposing them to PG. */
    if(s->async_scan && !s->delivery_shared && s->started) close_state(s);
    pg_sina_explain_placement((CustomScan *)node->ss.ps.plan,es);
    ExplainPropertyText("Execution Path","csd",es);
    ExplainPropertyInteger("CSD Device CUs",NULL,pg_sina_resources.cus,es);
    ExplainPropertyInteger("CSD HOST Parameter Count",NULL,list_length(s->parameter_exprs),es);
    ExplainPropertyInteger("CSD Device Initializers",NULL,s->plan->ninit,es);
    ExplainPropertyInteger("CSD Device FDM","bytes",pg_sina_resources.fdm_bytes,es);
    ExplainPropertyInteger("CSD Device Workmem","bytes",pg_sina_resources.workmem_bytes,es);
    ExplainPropertyInteger("CSD Query Memory Limit","bytes",s->plan->nodes[s->plan->root].expr.work_mem_bytes,es);
    ExplainPropertyText("CSD Plan Source","PostgreSQL selected Plan; CSD build/probe orientation",es);
    ExplainPropertyInteger("CSD Host Result Bytes","bytes",s->result_bytes,es);
    ExplainPropertyText("CSD Counter Scope",s->delivery_shared?"shared device stream; do not sum PG participants":"current device stream",es);
    ExplainPropertyBool("CSD Direct CU Output",s->plan->packed_result==QP_DIRECT_RESULT,es);
    ExplainPropertyText("CSD Result Delivery",s->async_scan?"SNIA callbacks; two bounded host batches":"synchronous",es);

    if(s->profiling && s->async_scan) {
        struct delivery_timing t=s->delivery_timing;
        if(s->delivery) {
            pthread_mutex_lock(&s->delivery->mutex); t=s->delivery->timing;
            pthread_mutex_unlock(&s->delivery->mutex);
        }
        ExplainPropertyFloat("CSD Consumer Queue Lock Sum","ms",t.lock_ns/1e6,3,es);
        ExplainPropertyFloat("CSD Consumer Batch Copy Sum","ms",t.copy_ns/1e6,3,es);
        ExplainPropertyFloat("CSD Consumer Empty Sleep Sum","ms",t.empty_ns/1e6,3,es);
        ExplainPropertyFloat("CSD Delivery Full Wait Sum","ms",t.full_ns/1e6,3,es);
        ExplainPropertyFloat("CSD Delivery Advance Wall","ms",t.advance_ns/1e6,3,es);
        ExplainPropertyInteger("CSD Consumer Claims",NULL,t.claims,es);
    }
    ExplainPropertyInteger("CSD Host Result Requests",NULL,s->result_reads,es);
    ExplainPropertyInteger("CSD Returned Rows",NULL,s->result_rows,es);
    ExplainPropertyInteger("CSD Output Columns",NULL,s->plan->nodes[s->plan->root].expr.nout,es);
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
        ExplainPropertyInteger("CSD Dictionary AFDM","bytes",4096,es);
        ExplainPropertyInteger("CSD Dictionary Entries",NULL,s->result->metrics.dictionary_entries,es);
        ExplainPropertyInteger("CSD Dictionary Bytes","bytes",s->result->metrics.dictionary_bytes,es);
        ExplainPropertyInteger("CSD Heap Rows",NULL,s->result->metrics.scanned,es);
        ExplainPropertyInteger("CSD Internal Read","bytes",s->result->metrics.input_bytes,es);
        ExplainPropertyInteger("CSD Index/Heap Page Loads",NULL,s->result->metrics.index_pages,es);
        ExplainPropertyInteger("CSD Indexed Heap Tuples",NULL,s->result->metrics.index_tuples,es);
        ExplainPropertyInteger("CSD Workmem Peak","bytes",s->result->metrics.work_peak,es);
        ExplainPropertyInteger("CSD Workers",NULL,s->result->metrics.workers,es);
        ExplainPropertyInteger("CSD Task Slots",NULL,s->workers?s->workers:1,es);
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
                "CSD Hash Build Coordinator CPU","CSD Aggregate Sink Coordinator CPU","CSD Aggregate Merge Coordinator CPU"};
            for(unsigned i=0;i<lengthof(ops);++i) ExplainPropertyFloat(labels[i],"ms",s->result->profile.operators[ops[i]].cpu_ns/1e6,3,es);
            for(unsigned i=0;i<s->plan->count;++i) {
                const struct qp_pipeline_profile *p=&s->plan->pipelines[i];
                if(p->hash_reindexes || p->hash_memory_ns) {
                    char label[64],value[256];
                    snprintf(label,sizeof(label),"CSD Hash Table %u",i);
                    snprintf(value,sizeof(value),"memory_cpu=%.3f ms reindex_cpu=%.3f ms reindexes=%llu rows=%llu moved_bytes=%llu (within Build)",
                        p->hash_memory_ns/1e6,p->hash_reindex_ns/1e6,
                        (unsigned long long)p->hash_reindexes,(unsigned long long)p->hash_reindexed_rows,
                        (unsigned long long)p->hash_moved_bytes);
                    ExplainPropertyText(label,value,es);
                }
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
            ExplainPropertyFloat("CSD Result Read Wall","ms",s->result_read_ns/1e6,3,es);
            ExplainPropertyFloat("CSD Control Read Sum","ms",s->control_read_ns/1e6,3,es);
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
    .EndCustomScan=end,.ReScanCustomScan=rescan,.ExplainCustomScan=explain,
    .EstimateDSMCustomScan=estimate_dsm,.InitializeDSMCustomScan=initialize_dsm,
    .ReInitializeDSMCustomScan=initialize_dsm,.InitializeWorkerCustomScan=initialize_worker,
    .ShutdownCustomScan=shutdown_scan};
const CustomScanMethods pg_sina_plan_methods={.CustomName="SniaCsdPlan",.CreateCustomScanState=create};
