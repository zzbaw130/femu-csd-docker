/* Translate a finite SNIA batch to one device graph. Spill stays on device. */
#include "femu_internal.h"
#include "csd-graph.h"
#include <endian.h>
struct cs_graph_request { void *execution; uint64_t cookie; struct cs_graph_request *next; };
/* Only mixed batches containing host copies need per-node rollback bookkeeping. */
struct cs_graph_scope { void *scope; uint64_t cookie; struct cs_graph_scope *next; };
static CS_STATUS session_command(struct cs_device *d, unsigned action, uint64_t cookie, u64 *value)
{
    struct nvme_passthru_cmd64 cmd = {.opcode=FEMU_CSD_GRAPH_SESSION, .nsid=d->nsid,
        .cdw10=action, .cdw11=d->graph_session, .cdw12=cookie, .cdw13=cookie>>32};
    CS_STATUS s=cs_io64(d,false,&cmd);
    if(value) *value=cmd.result;
    return s;
}
void femu_graph_close(struct cs_device *d)
{
    if(d->graph_session) session_command(d,FEMU_GRAPH_CLOSE,0,NULL);
    while(d->graph_scopes) {
        struct cs_graph_scope *next=d->graph_scopes->next;
        free(d->graph_scopes); d->graph_scopes=next;
    }
}
void femu_finishBatchExecution(void *device,void *scope,bool success)
{
    struct cs_device *d=device;
    pthread_mutex_lock(&d->graph_lock);
    struct cs_graph_scope **slot=&d->graph_scopes;
    while(*slot) {
        struct cs_graph_scope *r=*slot;
        if(r->scope!=scope) { slot=&r->next; continue; }
        if(!success) session_command(d,FEMU_GRAPH_ROLLBACK,r->cookie,NULL);
        *slot=r->next; free(r);
    }
    pthread_mutex_unlock(&d->graph_lock);
}
CS_STATUS femu_abortBatchRequest(void *device,void *execution)
{
    struct cs_device *d=device;
    CS_STATUS s=CS_SUCCESS;
    pthread_mutex_lock(&d->graph_lock);
    for(struct cs_graph_request *r=d->graphs;r;r=r->next)
        if(r->execution==execution) {
            CS_STATUS cancelled=session_command(d,FEMU_GRAPH_CANCEL,r->cookie,NULL);
            if(s==CS_SUCCESS) s=cancelled;
        }
    pthread_mutex_unlock(&d->graph_lock);
    return s;
}
static CS_STATUS queue_graph(void *device,CS_BATCH_MODE mode,unsigned capacity,
    const struct cs_backend_batch_entry *entries,bool (*cancelled)(void *),void *execution,u64 *value,void *scope)
{
    struct cs_device *d=device;
    unsigned count=0,map[CS_MAX_BATCH]={0};
    size_t bytes=sizeof(struct femu_graph_header);
    if(!capacity || capacity>CS_MAX_BATCH) return CS_INVALID_LENGTH;
    for(unsigned i=0;i<capacity;++i) if(entries[i].request) {
        const CsBatchRequest *r=entries[i].request;
        map[i]=count++;
        if(r->ReqType==CS_QUEUE_COMPUTE) {
            for(int j=0;j<r->u.Compute.NumArgs;++j)
                if(r->u.Compute.Args[j].Type==CS_AFDM_TYPE) bytes+=32;
        } else if(r->ReqType==CS_STORAGE_IO) bytes+=(size_t)r->u.StorageIo.u.BlockIo.NumRanges*sizeof(struct femu_graph_range);
        else return CS_UNSUPPORTED;
    }
    bytes+=(size_t)count*sizeof(struct femu_graph_node);
    if(!count) return CS_INVALID_ARG;
    if(bytes>FEMU_CSD_GRAPH_MAX_BYTES) return CS_INVALID_LENGTH;
    uint8_t *data=calloc(1,bytes);
    if(!data) return CS_OUT_OF_RESOURCES;
    struct cs_graph_scope *record=scope?calloc(1,sizeof(*record)):NULL;
    if(scope && !record) { free(data); return CS_OUT_OF_RESOURCES; }
    struct femu_graph_header *h=(void *)data;
    h->count=htole32(count); h->bytes=htole32(bytes);
    h->flags=htole32(mode==CS_BATCH_SERIAL?FEMU_CSD_GRAPH_SERIAL:0);
    struct femu_graph_node *nodes=(void *)(h+1);
    size_t offset=sizeof(*h)+count*sizeof(*nodes);
    for(unsigned i=0;i<capacity;++i) if(entries[i].request) {
        const CsBatchRequest *r=entries[i].request;
        struct femu_graph_node *n=&nodes[map[i]];
        uint64_t dependencies=0;
        for(unsigned j=0;j<capacity;++j)
            if(entries[i].dependencies&(UINT64_C(1)<<j)) dependencies|=UINT64_C(1)<<map[j];
        n->dependencies=htole64(dependencies); n->payload=htole32(offset);
        if(r->ReqType==CS_QUEUE_COMPUTE) {
            const CsComputeRequest *c=&r->u.Compute;
            unsigned nr=0,scalars=0; uint64_t params[2]={0};
            n->operation=htole32(FEMU_GRAPH_EXEC); n->program=htole32((uintptr_t)c->CSFId);
            for(int j=0;j<c->NumArgs;++j) {
                const CsComputeArg *a=&c->Args[j];
                if(a->Type==CS_AFDM_TYPE) {
                    struct cs_memory *m=a->u.DevMem.MemHandle;
                    struct {uint32_t nsid,len; uint64_t id,reserved[2];} mr={.id=htole64(m->id)};
                    memcpy(data+offset,&mr,sizeof(mr)); offset+=sizeof(mr); ++nr;
                } else {
                    params[scalars++]=a->Type==CS_8BIT_VALUE_TYPE?a->u.Value8:
                        a->Type==CS_16BIT_VALUE_TYPE?a->u.Value16:
                        a->Type==CS_32BIT_VALUE_TYPE?a->u.Value32:a->u.Value64;
                }
            }
            n->count=htole32(nr); n->cparam1=htole64(params[0]); n->cparam2=htole64(params[1]);
        } else {
            const CsBlockIo *b=&r->u.StorageIo.u.BlockIo;
            struct cs_memory *m=b->DevMem.MemHandle;
            n->operation=htole32(b->Type==CS_STORAGE_STORE_TYPE?FEMU_GRAPH_STORE:FEMU_GRAPH_LOAD);
            n->afdm=htole32(m->id); n->afdm_offset=htole64(b->DevMem.ByteOffset);
            n->count=htole32(b->NumRanges);
            for(int j=0;j<b->NumRanges;++j) {
                struct femu_graph_range range={htole32(b->Range[j].NamespaceId),
                    htole32(b->Range[j].NumBlocks),htole64(b->Range[j].StartLba)};
                memcpy(data+offset,&range,sizeof(range)); offset+=sizeof(range);
            }
        }
    }
    struct cs_graph_request active={.execution=execution};
    pthread_mutex_lock(&d->graph_lock);
    CS_STATUS status=CS_SUCCESS;
    if(!d->graph_session) {
        u64 id=0; status=session_command(d,FEMU_GRAPH_OPEN,0,&id);
        if(status==CS_SUCCESS) d->graph_session=id;
    }
    if(status!=CS_SUCCESS || cancelled(execution)) {
        pthread_mutex_unlock(&d->graph_lock); free(data); free(record);
        return status==CS_SUCCESS?CS_ERROR_IN_EXECUTION:status;
    }
    active.cookie=++d->next_graph;
    active.next=d->graphs; d->graphs=&active;
    h->session=htole64(d->graph_session); h->cookie=htole64(active.cookie);
    pthread_mutex_unlock(&d->graph_lock);
    struct nvme_passthru_cmd64 cmd={.opcode=FEMU_CSD_EXEC_GRAPH,.nsid=d->nsid,
        .addr=(uintptr_t)data,.data_len=bytes,.cdw10=bytes};
    status=cs_io64(d,false,&cmd);
    pthread_mutex_lock(&d->graph_lock);
    struct cs_graph_request **slot=&d->graphs;
    while(*slot!=&active) slot=&(*slot)->next;
    *slot=active.next;
    if(cancelled(execution)) {
        session_command(d,FEMU_GRAPH_FORGET_CANCEL,active.cookie,NULL);
        /* Cancellation can race a successful completion. Reclaim its tokens too. */
        session_command(d,FEMU_GRAPH_ROLLBACK,active.cookie,NULL);
        status=CS_ERROR_IN_EXECUTION;
    }
    if(record && status==CS_SUCCESS) {
        *record=(struct cs_graph_scope){scope,active.cookie,d->graph_scopes};
        d->graph_scopes=record; record=NULL;
    }
    pthread_mutex_unlock(&d->graph_lock);
    if(value) *value=cmd.result;
    free(record); free(data); return status;
}
CS_STATUS femu_queueBatchRequest(void *device,CS_BATCH_MODE mode,unsigned capacity,
    const struct cs_backend_batch_entry *entries,bool (*cancelled)(void *),void *execution,u64 *value)
{ return queue_graph(device,mode,capacity,entries,cancelled,execution,value,NULL); }
CS_STATUS femu_executeBatchEntry(void *device,const CsBatchRequest *req,bool (*cancelled)(void *),
    void *context,void *scope,u64 *value)
{
    if(req->ReqType!=CS_QUEUE_COMPUTE) {
        struct cs_task task={.dev=device,.req=req};
        return cs_task_run(&task,value);
    }
    struct cs_backend_batch_entry entry={req,0};
    return queue_graph(device,CS_BATCH_SERIAL,1,&entry,cancelled,context,value,scope);
}
