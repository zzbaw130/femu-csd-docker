#include "graph_state.h"
#include "graph_batch.h"

/* Prove a Sort/Merge key prefix through key-preserving projections/filters.
 * A NULL order accepts either direction, as required by streaming grouping. */
int input_ordered(const struct qp_plan *p,unsigned child,unsigned count,
    const int *columns,const struct qs_order *order)
{
    int keys[QS_KEYS]; memcpy(keys,columns,count*sizeof(*keys));
    for(;;) {
        const struct qp_node *n=&p->nodes[child];
        int merge=n->kind==QP_JOIN && !n->hash;
        if((n->kind!=QP_PROJECT && n->kind!=QP_SORT && !merge) || n->left<0) return 0;
        for(unsigned k=0;k<count;++k) {
            if(keys[k]<0 || (unsigned)keys[k]>=n->expr.nout) return 0;
            int index=n->expr.outputs[keys[k]].index;
            if(index<0 || (unsigned)index>=n->expr.nexprs || n->expr.exprs[index].op!=Q_COLUMN) return 0;
            keys[k]=n->expr.exprs[index].column;
        }
        if(n->kind==QP_SORT || merge) {
            if((merge?n->nkeys:n->expr.norder)<count) return 0;
            for(unsigned k=0;k<count;++k) {
                const struct qs_order *found=n->expr.norder?&n->expr.order[k]:NULL;
                if(keys[k]!=(merge?n->left_keys[k]:found->output)) return 0;
                if(order && (order[k].descending!=(found?found->descending:0) ||
                    order[k].nulls_first!=(found?found->nulls_first:0))) return 0;
            }
            return 1;
        }
        child=n->left;
    }
}
size_t sort_spill_reserve(const struct execution *s)
{
    size_t workspace=qs_sort_min_workspace();
    return 2*(workspace<s->sort_limit?workspace:s->sort_limit);
}

static int64_t sort_columns_call(struct femu_csd_args *a)
{
    const struct projection_batch *b=a->mr_addr[0];
    return qs_sort_sink_columns(a,&b->plan,b->vectors,b->position,b->count);
}

static int64_t sort_finish_call(struct femu_csd_args *a)
{ return qs_sort_finish_sink(((struct qs_task *)a->mr_addr[2])->state,a); }

static void close_sort(struct sorted_rows *sort)
{
    if(!sort->task.state || sort->drained) return;
    struct qs_group group=sort->group;
    if(!group.count) { group.count=1; group.states[0]=sort->task.state; }
    void *mr[]={&group}; long long len[]={sizeof(group)};
    struct femu_csd_args a={.numr=1,.mr_addr=mr,.mr_len=len};
    pg_sina_sort_source_impl(&a); sort->task.state=0;
}

int64_t sort_call(struct execution *s,struct sorted_rows *sort,int source)
{
    void *mr[4]; long long len[4];
    struct femu_csd_args a={.numr=source?3:4,.mr_addr=mr,.mr_len=len};
    if(source) {
        mr[0]=&sort->group; len[0]=sizeof(sort->group);
        mr[1]=sort->output; len[1]=sizeof(*sort->output); mr[2]=s->io; len[2]=s->io_bytes;
    } else {
        sort->plan.ordered_rows=sort->batch?3:2; /* Packed CU rows or padded scalar rows. */
        mr[0]=sort->columns?(void *)sort->columns:sort->batch?(void *)sort->batch:sort->input;
        len[0]=sizeof(*sort->input); mr[1]=&sort->plan; len[1]=sizeof(sort->plan);
        mr[2]=&sort->task; len[2]=sizeof(sort->task); mr[3]=s->io; len[3]=s->io_bytes;
    }
    if(s->pending_sort==sort) { a.cparam2=s->args->cparam2; s->pending_sort=NULL; }
    int64_t (*call)(struct femu_csd_args *)=source?pg_sina_sort_source_impl:sort->finishing_sink?sort_finish_call:
        sort->columns?sort_columns_call:pg_sina_sort_sink_impl;
    int64_t status=timed_call(s,call,&a,source?QS_SORT_SOURCE:QS_SORT_SINK);
    if(status>0) s->pending_sort=sort;
    if(!status && sort->finishing_sink) { sort->finishing_sink=0; sort->sink_finished=1; }
    if(source && !status) {
        const struct qs_metrics *m=&sort->output->metrics,*old=&sort->metrics;
        s->metrics.spill_read_bytes+=m->spill_read_bytes-old->spill_read_bytes;
        s->metrics.spill_write_bytes+=m->spill_write_bytes-old->spill_write_bytes;
        s->metrics.runs+=m->runs-old->runs;
        s->metrics.spilled_runs+=m->spilled_runs-old->spilled_runs;
        sort->metrics=*m;
    }
    return status;
}

int64_t sort_init(struct execution *s,struct sorted_rows *sort,const struct qs_plan *input,
    unsigned keys,const int *columns,const struct qs_order *order)
{
    if(sort->initialized) return 0;
    sort->input=cs_work_calloc(s->context,1,sizeof(*sort->input));
    sort->output=cs_work_calloc(s->context,1,sizeof(*sort->output));
    if(!sort->input || !sort->output) return -4;
    sort->plan.nout=input->nout; sort->plan.ordered_rows=2; sort->plan.limit=-1;
    for(unsigned id=0;id<s->plan->count;++id)
        if(&s->nodes[id].left_sort==sort && s->plan->nodes[id].kind==QP_SORT) {
            sort->plan.limit=qp_sort_bound(s->plan,id);
            break;
        }
    /* The plan depth supplies a conservative fallback reserve, not a fixed
     * partition of all memory. Borrow from actual free query memory when
     * fewer sorts are live, retaining workspace for consumers and merge. */
    if(!sort->plan.work_mem_bytes) {
        struct execution *owner=s->owner?s->owner:s;
        unsigned active=0;
        for(unsigned i=0;i<owner->plan->count;++i) {
            const struct operator_state *o=&owner->nodes[i];
            active+=o->left_sort.initialized && !o->left_sort.drained;
            active+=o->right_sort.initialized && !o->right_sort.drained;
        }
        size_t free_bytes=cs_work_available(owner->context),reserve=2*s->sort_limit;
        size_t available=free_bytes/(active+2);
        if(free_bytes>reserve && free_bytes-reserve>available) available=free_bytes-reserve;
        sort->plan.work_mem_bytes=available>s->sort_limit?available:s->sort_limit;
    }
    sort->plan.work_mem_parent=(uintptr_t)s->context;
    memcpy(sort->plan.outputs,input->outputs,input->nout*sizeof(input->outputs[0]));
    for(unsigned i=0;i<keys;++i) sort->plan.order[sort->plan.norder++]=order?order[i]:(struct qs_order){.output=columns[i]};
    sort->task.workers=1;
    int64_t status=sort_call(s,sort,0); if(status) return status;
    sort->initialized=1; sort->close=close_sort; sort->lookup_text=qs_sort_output_text;
    if(s->paged) { int bound=qs_sort_paged_dictionary(sort->task.state,s->paged); if(bound) return bound; }
    else qs_sort_dictionary(sort->task.state,s->dictionary);
    return 0;
}

int64_t sorted_input(struct execution *s,struct sorted_rows *sort,unsigned child,
    unsigned keys,const int *columns,const struct qs_order *order,struct qs_cell *row)
{
    const struct qp_node *input=&s->plan->nodes[child];
    bool edge=graph_input_edge(s,child);
    int64_t status=sort_init(s,sort,&input->expr,keys,columns,order); if(status) return status;
    if(sort->drained && !sort->output) return 0;
    while(!sort->complete) {
        if(sort->input_pending) {
            status=sort_call(s,sort,0); if(status) return status;
            sort->input_pending=0; sort->input->rows=0;
        }
        if(sort->batch) {
            status=sort_call(s,sort,0); if(status) return status;
            if(edge) s->hash_input_position=sort->batch->rows;
            else s->parallel_position[sort->batch_worker]=sort->batch->rows;
            sort->batch=NULL; continue;
        }
        if(sort->prefix && sort->prefix_position==sort->prefix_rows) {
            cs_work_free(s->context,sort->prefix); sort->prefix=NULL;
        }
        if(edge && !s->hash_input->begin && !sort->input->rows && s->hash_input_position<s->hash_input->rows) {
            /* The same packed input as the worker path. Keep the packet
             * borrowed until the sort sink commits it across spill reentry. */
            sort->batch=s->hash_input;
            continue;
        }
        if(!edge && !s->worker && s->plan->workers>1 && !sort->input->rows && sort->prefix_position==sort->prefix_rows &&
           input->parallel && !s->nodes[child].finished) {
            /* Do not drain/requeue this slot until Sort has consumed it,
             * including any STORE/LOAD reentry. Prefix fallback rows keep
             * their existing scalar adapter. */
            status=parallel_next(s,child,NULL);
            if(status<0 || status>3 || status==QP_WAIT) return status;
            if(status==2) {
                for(unsigned w=0;w<s->plan->workers;++w)
                    if((s->control->available&(UINT64_C(1)<<w)) && s->parallel_position[w]<s->outputs[w+1].rows) {
                        sort->batch=&s->outputs[w+1]; sort->batch_worker=w;
                        s->outputs[w+1].begin=s->parallel_position[w]; break;
                    }
                if(!sort->batch) return -1;
                continue;
            }
            /* 3 means this producer selected its ordinary scalar fallback. */
        }
        if(!edge && !sort->input->rows && sort->prefix_position==sort->prefix_rows &&
           !(!s->worker && s->plan->workers>1 && input->parallel)) {
            status=sort_sink_columns(s,sort,child);
            if(status!=3) {
                if(status) return status;
                sort->complete=1;
                if(!sort->group.count) { sort->group.count=1; sort->group.states[0]=sort->task.state; }
                cs_work_free(s->context,sort->input); sort->input=NULL; break;
            }
        }
        if(sort->prefix_position<sort->prefix_rows) {
            hash_load(sort->input->data[sort->input->rows].cells,
                sort->prefix+(size_t)sort->prefix_position++*hash_row_bytes(&input->expr),&input->expr);
            status=2;
        } else {
            status=next_row(s,child,sort->input->data[sort->input->rows].cells);
        }
        if(status<0 || status>2) return status;
        if(status==QP_WAIT) return QP_WAIT;
        /* next_row returns 2 for a row and 0 for EOF. */
        if(!status) {
            if(sort->input->rows) { sort->input_pending=1; continue; }
            sort->complete=1;
            if(!sort->group.count) { sort->group.count=1; sort->group.states[0]=sort->task.state; }
            cs_work_free(s->context,sort->input); sort->input=NULL; break;
        }
        if(++sort->input->rows==QS_RESULT_ROWS) sort->input_pending=1;
    }
    if((s->pending_sort==sort || sort->position==sort->output->rows) && !sort->drained) {
        status=sort_call(s,sort,1); if(status) return status;
        sort->position=0; sort->drained=sort->output->finished;
    }
    if(sort->position<sort->output->rows) {
        if(!row) return 2; /* Borrow the remaining Source batch. */
        memcpy(row,sort->output->data[sort->position++].cells,input->expr.nout*sizeof(*row)); return 2;
    }
    if(sort->drained) { cs_work_free(s->context,sort->output); sort->output=NULL; }
    return 0;
}

int64_t sort_sink_columns(struct execution *s,struct sorted_rows *sort,unsigned id)
{
    int source=s->plan->nodes[id].kind==QP_AGGREGATE;
    int status=projection_init(s,id,source); if(status) return status;
    struct projection_batch **slot=source?&s->nodes[id].source_projection:&s->nodes[id].projection;
    while(*slot) {
        if(sort->columns) {
            struct projection_batch *b=sort->columns; unsigned count=b->count-b->position;
            /* Runs outlive this producer batch; retain text as dictionary IDs. */
            if(materialize_outputs(s,b)) return -3;
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            int64_t result=sort_call(s,sort,0);
            if(s->profiling && s->worker) s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick;
            if(result) return result;
            b->position=b->count; sort->columns=NULL; s->metrics.matched+=count;
            if(s->profiling && s->worker) s->sink_rows+=count;
        }
        int64_t ready=projection_fill(s,id,source);
        if(ready!=2) return ready;
        sort->columns=*slot;
    }
    return 3; /* Unsupported producer keeps its original row cursor. */
}

