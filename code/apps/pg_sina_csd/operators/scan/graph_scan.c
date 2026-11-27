#include "graph_state.h"
#include "graph_batch.h"

int64_t pg_sina_scan_impl(struct femu_csd_args *);

int64_t scan_decode(struct execution *s,unsigned id,unsigned bytes,bool eager)
{
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->nodes[id];
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n));
    const struct qs_plan *p=&n->expr;
    struct qs_task task={.input_bytes=bytes};
    int64_t status=scan_filter_init(s,p,o); if(status) return status;
    void *mr[]={s->arena+input_offset(s,n)+o->scan_input,!eager && o->scan_filter?o->scan_filter:(void *)p,chunk,&task,s->dictionary};
    long long len[]={bytes,sizeof(*p),qs_chunk_bytes(chunk->capacity,chunk->ncols),sizeof(task),0};
    struct femu_csd_args a={.numr=5,.mr_addr=mr,.mr_len=len};
    o->scan_raw=o->scan_input; o->scan_raw_bytes=bytes;
    const struct qs_plan *decoder=mr[1]; int eager_text=0;
    for(unsigned col=0;col<decoder->nattrs;++col)
        eager_text|=decoder->cols[col].slot>=0 && decoder->cols[col].type==Q_STRING && !decoder->cols[col].deferred;
    /* Numeric and borrowed-text scans cannot fault on a dictionary.
     * Keep the baseline decoder instead of saving a paging cursor. */
    if(s->paged && (eager || eager_text)) {
        if(!o->scan_cursor) o->scan_cursor=qs_scan_cursor_create(s->context,s->paged);
        if(!o->scan_cursor) return -4;
        mr[4]=s->io; len[4]=s->io_bytes;
        struct blocking_timer timer={0}; if(s->profiling) timer=blocking_begin(s);
        status=qs_scan_columns(o->scan_cursor,&a,s->dictionary_wait?csReentryValue(s->args):0);
        if(s->profiling) blocking_end(s,QS_SCAN,timer);
        s->dictionary_wait=status>0?status:0;
    } else status=timed_call(s,pg_sina_scan_impl,&a,QS_SCAN);
    if(status) return status;
    if(!eager) {
        s->metrics.scanned+=chunk->rows; s->metrics.input_bytes+=task.input_bytes; ++s->metrics.chunks;
    }
    return 0;
}

static unsigned scan_vector_bytes(const unsigned char *input,unsigned remaining,unsigned capacity)
{
    unsigned bytes=0,rows=0;
    while(bytes<remaining) {
        uint16_t lower; memcpy(&lower,input+bytes+12,sizeof(lower));
        unsigned items=lower>=24?(lower-24)/4:0;
        if(bytes && rows+items>capacity) break;
        bytes+=QS_PAGE; rows+=items;
    }
    return bytes;
}

int64_t scan_chunk_next(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n));
    if(n->ordered_index) {
        const struct execution *owner=s->owner?s->owner:s;
        if(owner->read_ordered_index) return owner->read_ordered_index(s,id);
#ifdef QP_SPLIT_RUNTIME
        return -1;
#else
        return ordered_index_next(s,id);
#endif
    }
    for(;;) {
        if(!o->index_task.index_pending && !s->dictionary_wait && o->scan_position<chunk->selected) return 2;
        if(s->control->ready && s->control->node==id && !s->control->index_nreads) {
            /* The retained heap input must not own the shared page-request
             * record: an Index Join may use that record between vectors. */
            o->scan_bytes=s->control->bytes;
            s->control->ready=s->control->bytes=0;
        }
        if(s->stream_block && id==(unsigned)qp_probe_scan(s->plan,s->root)) {
            s->scan_drained=true;
            return QP_WAIT;
        }
        if(o->scan_bytes) {
            /* Keep the I/O block and fill vectors with complete heap pages.
             * Line-pointer counts conservatively include unused entries;
             * one page always fits the minimum capacity (QS_PAGE/24). */
            if(chunk->capacity<QS_PAGE/24 || o->scan_input>=o->scan_bytes) return -1;
            unsigned remaining=o->scan_bytes-o->scan_input;
            if(remaining%QS_PAGE) return -1;
            const unsigned char *input=s->arena+input_offset(s,n)+o->scan_input;
            unsigned bytes=scan_vector_bytes(input,remaining,chunk->capacity);
            int64_t status=scan_decode(s,id,bytes,false); if(status) return status;
            status=scan_filter_chunk(s,id);
            if(status==-4) {
                /* Reuse this input with the scalar predicate if scratch is full. */
                s->raw_projection=0;
                cs_work_free(s->context,o->scan_filter); o->scan_filter=NULL;
                status=scan_decode(s,id,bytes,true);
            }
            if(status) return status;
            o->scan_position=0; o->position+=bytes; o->scan_input+=bytes;
            if(o->scan_input==o->scan_bytes) o->scan_input=o->scan_bytes=0;
            continue;
        }
        if(s->host_scan && id==(unsigned)qp_probe_scan(s->plan,s->root)) {
            s->scan_drained=true;
            return o->position>=n->bytes?0:QP_WAIT;
        }
        if(s->worker && id==(unsigned)qp_probe_scan(s->plan,s->root))
            o->position=atomic_fetch_add_explicit(&s->owner->next_offset,s->plan->scan_bytes,memory_order_relaxed);
        if(o->position>=n->bytes) return 0;
        s->control->node=id; s->control->offset=o->position;
        s->control->bytes=n->bytes-o->position<s->plan->scan_bytes?n->bytes-o->position:s->plan->scan_bytes;
        return QP_WAIT;
    }
}
int64_t scan_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; const struct qs_plan *p=&n->expr;
    struct operator_state *o=&s->nodes[id];
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n));
    for(;;) {
        int64_t status=scan_chunk_next(s,id); if(status!=2) return status;
        unsigned at=qs_selection(chunk)[o->scan_position++];
        if(id==s->plan->root && s->raw_projection) {
            /* A pure projection reads the selected physical columns directly.
             * Do not construct nattrs cells, walk nattrs again for borrowed
             * values, then copy those same cells through a projection. */
            const unsigned char *raw=s->arena+input_offset(s,n)+o->scan_raw;
            for(unsigned col=0;col<p->nout;++col) {
                const struct qs_column *spec=&p->cols[p->exprs[p->outputs[col].index].column];
                unsigned slot=spec->slot;
                row[col]=(struct qs_cell){.value=qs_values(chunk)[(size_t)slot*chunk->capacity+at],
                    .scale=spec->scale,.dscale=qs_dscales(chunk)[(size_t)slot*chunk->capacity+at],
                    .isnull=(qs_nulls(chunk)[(size_t)slot*((chunk->capacity+7)/8)+at/8]>>(at%8))&1};
                if((spec->type==Q_DECIMAL || spec->type==Q_STRING) && !row[col].isnull) {
                    struct qs_heap_ref ref; uint64_t bits=(uint64_t)row[col].value; memcpy(&ref,&bits,sizeof(ref));
                    if(ref.offset>o->scan_raw_bytes || ref.bytes>o->scan_raw_bytes-ref.offset) return -3;
                    if(spec->type==Q_STRING && spec->trim_spaces)
                        while(ref.bytes && raw[ref.offset+ref.bytes-1]==' ') --ref.bytes;
                    row[col].value=(unsigned __int128)(uintptr_t)(raw+ref.offset) | ((unsigned __int128)ref.bytes<<64);
                }
            }
            ++s->metrics.matched; return 2;
        }
        struct qs_cell values[QS_COLS];
        for(unsigned i=0;i<p->nattrs;++i) if(p->cols[i].slot>=0) {
            unsigned slot=p->cols[i].slot;
            values[i]=(struct qs_cell){.value=qs_values(chunk)[(size_t)slot*chunk->capacity+at],
                .scale=p->cols[i].scale,.dscale=qs_dscales(chunk)[(size_t)slot*chunk->capacity+at],
                .isnull=(qs_nulls(chunk)[(size_t)slot*((chunk->capacity+7)/8)+at/8]>>(at%8))&1};
        }
        int count=project_filtered(s,n,values,NULL,row,o->scan_filter && o->scan_filter->predicate>=0); if(count<0) return count;
        if(count) { ++s->metrics.matched; return 2; }
    }
}
static int prepare_scan_consumer(struct execution *s,unsigned scan,unsigned sink)
{
    unsigned first=s->root;
    while(first!=scan && s->plan->nodes[first].left!=(int)scan)
        first=s->plan->nodes[first].left;
    /* The first consumer may select deferred column decoding.
     * Set up its projection before decoding any page in the block. */
    int ready;
    if(s->plan->nodes[first].kind==QP_JOIN)
        ready=hash_probe_projection(s,first,0);
    else
        ready=projection_init(s,first==scan?sink:
            s->plan->nodes[first].kind==QP_INDEX_JOIN?scan:first,0);
    return ready;
}

static int64_t read_index_chunk(struct execution *s,struct projection_batch *batch)
{ return scan_chunk_next(s,batch->scan); }

static int64_t stream_scan(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    const struct qp_node *leaf=&s->plan->nodes[scan];
    struct operator_state *o=&s->nodes[scan];
#ifdef QP_SPLIT_RUNTIME
    if(!o->read_chunk) {
        struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,leaf));
        *chunk=(struct qs_chunk){.capacity=leaf->ordered_index?256:qp_scan_capacity(s->plan,leaf),
            .ncols=leaf->expr.ncols};
    }
#endif
    o->read_row=scan_next;
    o->read_chunk=scan_chunk_next;
    if(leaf->ordered_index) {
        bool host_output=s->plan->packed_result==QP_DIRECT_RESULT;
        if(host_output) {
            /* The index cursor spans the relation. End this graph at an
             * output window boundary and retain the cursor for the next one. */
            unsigned capacity=qp_host_capacity(&leaf->expr);
            if(s->scan_packets || (s->host_building && out->rows &&
               out->rows+QS_VECTOR_ROWS>capacity)) return graph_finish_stream();
            s->stream_scanning=true;
        }
        int64_t status=graph_device_scan(s,scan,read_index_chunk);
        if(host_output && status==graph_finish_stream()) s->scan_output_finished=true;
        return status;
    }
    o->decode_chunk=scan_decode;
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,leaf));
    s->published_chunk=chunk;
    if(!s->stream_scanning) {
        uint64_t page=(uint64_t)s->args->cparam1>>QP_SCAN_PAGE_SHIFT;
        if(page>UINT64_MAX/QS_PAGE || page*QS_PAGE>leaf->bytes) return -1;
        uint64_t offset=page*QS_PAGE;
        s->stream_bytes=leaf->bytes-offset<s->plan->scan_bytes?leaf->bytes-offset:s->plan->scan_bytes;
        s->stream_offset=0;
        s->stream_scanning=s->host_scan=true;
        s->stream_filter_retry=s->scan_drained=false;
        o->position=offset;
        o->scan_bytes=o->scan_input=o->scan_raw_bytes=o->scan_position=0;
        out->rows=out->begin=out->finished=out->str_bytes=0;
        int ready=prepare_scan_consumer(s,scan,sink);
        if(ready) return ready;
    }
    while(s->stream_offset<s->stream_bytes) {
        unsigned remaining=s->stream_bytes-s->stream_offset;
        if(remaining%QS_PAGE || chunk->capacity<QS_PAGE/24) return -1;
        o->scan_input=s->stream_offset;
        const unsigned char *input=s->arena+input_offset(s,leaf)+o->scan_input;
        unsigned bytes=scan_vector_bytes(input,remaining,chunk->capacity);
        int64_t status=scan_decode(s,scan,bytes,false); if(status) return status;
        s->stream_offset+=bytes; o->position+=bytes; o->scan_position=0;
        if(leaf->expr.predicate<0) {
            /* No Filter EXEC follows this Scan. Publish only a fresh
             * nonempty window and let the next operator borrow it. */
            if(!chunk->selected) continue;
            s->scan_drained=false;
            if(s->plan->nodes[s->root].kind==QP_INDEX_JOIN)
                s->nodes[s->root].input_done=s->nodes[s->root].finished=0;
        }
        return graph_publish_output();
    }
    return graph_finish_stream();
}

int64_t pg_sina_graph_block_scan(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_SCAN,stream_scan); }

/* Most blocks fit the direct result AFDM. Retain extra packed packets only
 * when the block outgrows it, then drain them without reissuing the scan. */
struct scan_packet {
    struct scan_packet *next;
    size_t bytes;
    unsigned strings;
    unsigned char data[];
};

static int save_scan_packet(struct execution *s,struct qs_result *out,unsigned strings)
{
    const struct qs_plan *p=&s->plan->nodes[s->root].expr;
    size_t bytes=offsetof(struct qs_result,data)+qp_host_bytes(p,out->rows)+strings;
    struct scan_packet *packet=cs_work_alloc(s->context,sizeof(*packet)+bytes);
    if(!packet) return -4;
    packet->next=NULL; packet->bytes=bytes; packet->strings=strings;
    memcpy(packet->data,out,bytes);
    if(s->scan_packets_tail) s->scan_packets_tail->next=packet;
    else s->scan_packets=packet;
    s->scan_packets_tail=packet;
    return 0;
}

static unsigned pack_scan_packet(struct qs_result *out,const struct qs_plan *p,unsigned capacity)
{
    unsigned strings=out->str_bytes;
    qp_host_compact(out,p,capacity);
    qp_host_pack(out,p,strings);
    return strings;
}

static int64_t scan_output_ready(struct execution *s,struct qs_result *out,unsigned strings)
{
    bool drained=!s->scan_packets;
    bool finished=drained && s->scan_output_finished;
    out->finished=s->control->finished=finished;
    s->control->output_rows=out->rows;
    s->control->output_strings=strings;
    return QP_TYPED_OUTPUT|((uint64_t)out->rows<<5)|((uint64_t)strings<<22)|
        (out->rows?QP_OUTPUT_READY:0)|(drained?QP_INPUT_DRAINED:0)|(finished?QP_WORKER_FINISHED:0);
}

static int64_t read_scan_packet(struct execution *s,struct qs_result *out)
{
    struct scan_packet *packet=s->scan_packets;
    if(!packet) return -1;
    memcpy(out,packet->data,packet->bytes);
    s->scan_packets=packet->next;
    if(!s->scan_packets) s->scan_packets_tail=NULL;
    unsigned strings=packet->strings;
    cs_work_free(s->context,packet);
    if(!s->scan_packets && s->scan_output_finished) plan_output_metrics(s,out,s->worker);
    return scan_output_ready(s,out,strings);
}

static int64_t stream_scan_output(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    (void)sink;
    const struct qp_node *node=&s->plan->nodes[scan];
    const struct qs_plan *p=&node->expr;
    unsigned capacity=qp_host_capacity(p);
    if(!capacity) return -1;
    if(!s->host_building) {
        s->host_building=1;
        out->status=out->rows=out->begin=out->str_bytes=0;
        host_begin(s,p,out,capacity);
    }
    if(s->args->graph_input==FEMU_GRAPH_INPUT_EOF) {
        unsigned strings=pack_scan_packet(out,p,capacity);
        if(!node->ordered_index)
            s->scan_output_finished=s->nodes[scan].position==node->bytes;
        s->host_building=s->stream_scanning=false;
        if(s->scan_packets) {
            if(out->rows && save_scan_packet(s,out,strings)) return -4;
            int64_t status=read_scan_packet(s,out);
            if(status<0) return status;
            return csGraphStep(FEMU_GRAPH_STEP_DONE,status);
        }
        return csGraphStep(FEMU_GRAPH_STEP_DONE,scan_output_ready(s,out,strings));
    }
    for(;;) {
        if(!s->host_pending) {
            if(node->ordered_index) {
                struct projection_batch *batch=s->nodes[scan].projection;
                if(!batch || batch->position==batch->count) return graph_consume_input();
            }
            int64_t status=next_row(s,scan,s->host_row);
            if(status==QP_WAIT && s->scan_drained) return graph_consume_input();
            if(status!=2) return status?status:-1;
            s->host_pending=1;
        }
        int status=out->rows==capacity?1:host_append(s,p,out);
        if(status<0) return status;
        if(status) {
            if(!out->rows) return -4;
            unsigned strings=pack_scan_packet(out,p,capacity);
            if(save_scan_packet(s,out,strings)) return -4;
            out->rows=out->str_bytes=0;
            host_begin(s,p,out,capacity);
        }
    }
}

int64_t pg_sina_graph_scan_output(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_HOST_OUTPUT,stream_scan_output); }

int64_t pg_sina_graph_scan_drain(struct femu_csd_args *a)
{
    if(!graph_operator_args(a)) return -1;
    const struct qp_plan *p=a->mr_addr[0];
    unsigned worker=((uint64_t)a->cparam1>>1)&127;
    if(!worker || worker>p->workers || p->packed_result!=QP_DIRECT_RESULT) return -1;
    struct qp_control *control=(struct qp_control *)a->mr_addr[1]+worker;
    struct execution *s=(void *)(uintptr_t)control->state;
    if(!s || s->plan!=p || s->stream_scanning) return -1;
    return read_scan_packet(s,(struct qs_result *)a->mr_addr[2]+worker);
}

int64_t pg_sina_graph_scan_wave(struct femu_csd_args *a)
{
    int id=graph_wave_node(a); if(id<0) return -1;
    const struct qp_plan *p=a->mr_addr[0];
    if(p->count!=1 || p->root!=(unsigned)id || p->nodes[id].kind!=QP_SCAN ||
       p->packed_result!=QP_DIRECT_RESULT || !p->workers) return -1;
    struct qp_control *control=a->mr_addr[1];
    struct execution *s=(void *)(uintptr_t)control->state;
    struct qs_result *out=a->mr_addr[2];
    if(s && s->closing) {
        s->args=a; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
        return graph_finish(s,a,out);
    }
    int64_t status=graph_wave_begin(a,&graph_project_ops,graph_index_init,&s);
    if(status) return status;
    const struct graph_wave_sink sink={.node=id};
    status=parallel_wave_next(s,id,NULL,&sink);
    if(status==QP_WAIT) return 0;
    if(status) return status==3?-4:status;
    out->rows=out->begin=out->str_bytes=0;
    plan_output_metrics(s,out,0);
    s->closing=1;
    return graph_finish(s,a,out);
}
