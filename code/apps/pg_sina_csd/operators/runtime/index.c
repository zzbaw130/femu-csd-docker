#include "graph_state.h"
#include "graph_batch.h"

static void close_index(struct operator_state *state)
{ qs_index_close(&state->index_control); }

void ordered_index_metrics(struct operator_state *state,struct qs_metrics *metrics,struct qs_profile *profile)
{
    (void)profile;
    if(metrics) {
        metrics->index_pages+=state->index_task.index_pages;
        metrics->index_tuples+=state->index_task.index_tuples;
    } else {
        state->index_task.index_pages=state->index_task.index_tuples=0;
        qs_index_cache_stats(&state->index_control,NULL,NULL);
    }
}

int graph_open_index(const struct qs_plan *plan,struct operator_state *state)
{
    int status=qs_index_init(plan,&state->index_control); if(status) return status;
    state->close=close_index;
    state->metrics=ordered_index_metrics;
    return 0;
}

void graph_index_init(struct execution *s,size_t budget)
{
    s->read_ordered_index=ordered_index_next;
    uint64_t bytes=s->plan->index_cache_bytes;
    size_t limit=QP_INDEX_CACHE_MAX_MIB*1024*1024ULL;
    if(bytes>=1024*1024 && bytes<=limit && bytes<budget)
        s->index_cache=qs_index_cache_create(s->context,bytes,limit,s->profiling);
}

int64_t ordered_index_next(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id];
    const struct qs_plan *p=&n->expr;
    struct operator_state *o=&s->nodes[id];
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n));
    if(!o->index_control.state) {
        o->index_plan=cs_work_alloc(s->context,sizeof(*p)); if(!o->index_plan) return -4;
        *o->index_plan=*p; o->index_plan->work_mem_parent=(uintptr_t)s->context;
        o->index_plan->join.nattrs=p->nattrs; o->index_plan->join.work_mem_bytes=QS_INDEX_WORK_BYTES;
        int status=graph_open_index(o->index_plan,o); if(status) return status;
        if(s->paged) { int bound=qs_index_paged_dictionary(&o->index_control,s->paged); if(bound) return bound; }
        else qs_index_dictionary(&o->index_control,s->dictionary);
    }
    for(;;) {
        if(!o->index_task.index_pending && !qs_index_dictionary_pending(&o->index_control,0) &&
           !s->dictionary_wait && o->scan_position<chunk->selected) return 2;
        if(s->stream_source_window) { s->scan_drained=true; return QP_WAIT; }
        if(s->worker && (n->ordered_index&8) && !o->input_done) {
            uint64_t first=atomic_fetch_add_explicit(&s->owner->next_offset,16*QS_PAGE,memory_order_relaxed)/QS_PAGE;
            unsigned pages=n->index_bytes/QS_PAGE;
            if(first>=pages) return 0;
            qs_index_scan_range(&o->index_control,first?first:1,first+16<pages?first+16:pages);
            o->input_done=1;
        }
        if(o->index_task.index_pending) {
            if(!s->control->ready) return QP_WAIT;
            s->control->ready=s->control->index_nreads=0;
        } else if(!s->paged || !qs_index_dictionary_pending(&o->index_control,0)) chunk->rows=chunk->selected=0;
        int64_t status=s->paged?qs_index_scan_paged(chunk,&o->index_task,&o->index_control,s->arena+input_offset(s,n),
            n->bytes/QS_PAGE,n->index_bytes/QS_PAGE,n->ordered_index,s->args):
            qs_index_scan(chunk,&o->index_task,&o->index_control,s->arena+input_offset(s,n),
            n->bytes/QS_PAGE,n->index_bytes/QS_PAGE,n->ordered_index);
        s->dictionary_wait=s->paged && qs_index_dictionary_pending(&o->index_control,0)?status:0;
        if(status==QS_INDEX_WAIT && o->index_task.index_pending) {
            s->control->node=id; s->control->index_nreads=o->index_task.index_nreads;
            memcpy(s->control->index_reads,o->index_task.index_reads,sizeof(s->control->index_reads));
            return QP_WAIT;
        }
        if(status) return status;
        o->scan_position=0; s->metrics.scanned+=chunk->rows;
        if(!chunk->rows) {
            if(s->worker && (n->ordered_index&8)) { o->input_done=0; continue; }
            return 0;
        }
    }
}

