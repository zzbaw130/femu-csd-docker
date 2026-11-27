#include "graph_state.h"
#include "graph_batch.h"

static int64_t sort_read_row(struct execution *,unsigned,struct qs_cell *);

int64_t projection_sort_fill(struct execution *s,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[b->scan];
    struct sorted_rows *sort=&s->nodes[b->scan].left_sort;
    int64_t status=sorted_input(s,sort,n->left,n->expr.norder,NULL,n->expr.order,NULL);
    if(status!=2) return status;
    b->count=sort->output->rows-sort->position; b->position=0;
    if(b->count>QS_VECTOR_ROWS) b->count=QS_VECTOR_ROWS;
    struct qs_vector_input input={.cells=sort->output->data[sort->position].cells,
        .column_stride=1,.row_stride=QS_OUT,.dictionary=s->dictionary,
        .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    b->input_cursor=&sort->position;
    if(projection_execute(b,NULL,NULL,0,input)) return -5;
    return 2;
}

void parallel_sort_begin(struct execution *s,unsigned root)
{
    struct sorted_rows *sort=NULL;
    for(unsigned i=0;i<s->plan->count;++i)
        if(s->plan->nodes[i].kind==QP_SORT && s->plan->nodes[i].left==(int)root &&
           s->nodes[i].left_sort.initialized) { sort=&s->nodes[i].left_sort; break; }
    if(!sort || s->partition_join>=0) return;
    sort->group.count=1; sort->group.states[0]=sort->task.state;
    for(unsigned w=0;w<s->plan->workers;++w) {
        struct execution *e=s->workers[w];
        struct sorted_rows *local=cs_work_calloc(e->context,1,sizeof(*local));
        if(!local) goto fallback;
        e->sort_sink=local; local->plan=sort->plan; local->task.workers=1;
        if(!w) local->task.state=sort->task.state;
        else {
            local->input=&s->outputs[w+1]; local->input->rows=local->input->begin=0;
            if(sort_call(e,local,0)) goto fallback;
            sort->group.states[sort->group.count++]=local->task.state;
            if(s->paged) { if(qs_sort_paged_dictionary(local->task.state,s->paged)) goto fallback; }
            else qs_sort_dictionary(local->task.state,s->dictionary);
        }
        if(qs_sort_prepare(local->task.state)) goto fallback;
    }
    return;
fallback:
    /* Nothing has consumed input or spilled. Keep the original owner and
     * the ordinary parallel-producer/serial-Sink path on allocation failure. */
    for(unsigned w=1;w<sort->group.count;++w) {
        struct qs_group group={.count=1,.states={sort->group.states[w]}};
        void *mr[]={&group}; long long len[]={sizeof(group)};
        struct femu_csd_args a={.numr=1,.mr_addr=mr,.mr_len=len}; pg_sina_sort_source_impl(&a);
        sort->group.states[w]=0;
    }
    sort->group.count=1;
    for(unsigned w=0;w<s->plan->workers;++w) {
        struct execution *e=s->workers[w]; cs_work_free(e->context,e->sort_sink); e->sort_sink=NULL;
    }
}

static bool sort_result_ready(const struct execution *s,unsigned id)
{
    return s->plan->nodes[id].kind==QP_SORT && s->nodes[id].left_sort.complete;
}

static int64_t sort_result_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->nodes[id];
    if(o->finished && !projection_pending(o->projection) && !projection_pending(o->source_projection)) {
        projection_clear(s,&o->projection);
        projection_clear(s,&o->source_projection);
        return 0;
    }
    if(!s->hash_input && !s->worker && s->plan->workers>1 && n->parallel)
        return parallel_next(s,id,row);
    int64_t status=projection_init(s,id,0); if(status) return status;
    if(o->projection) return projection_next(s,id,row,0);
    for(;;) {
        status=sorted_input(s,&o->left_sort,n->left,n->expr.norder,NULL,n->expr.order,o->left);
        if(status!=2) return status;
        int count=project(s,n,o->left,NULL,row); if(count<0) return count;
        if(count) { ++o->emitted; return 2; }
    }
}

static int64_t sort_read_row(struct execution *s,unsigned id,struct qs_cell *row)
{ return sort_result_ready(s,id)?sort_result_next(s,id,row):-1; }

int64_t pg_sina_graph_sort_result(struct femu_csd_args *a)
{
    static const struct graph_result_source source={.ready=sort_result_ready,.next=sort_result_next};
    qs_dictionary_graph_context(a);
    int64_t status=graph_emit(a,GRAPH_EMIT_SORT,&source);
    qs_dictionary_graph_context(NULL);
    return status;
}

static int64_t sort_batch(struct execution *s,unsigned id,struct projection_batch *batch,struct qs_result *out)
{
    (void)id;
    (void)out;
    return sort_sink_vectors(s,batch);
}

static int64_t finish_sort(struct execution *s,unsigned sink)
{
    (void)sink;
    s->sort_sink->finishing_sink=1;
    return sort_call(s,s->sort_sink,0);
}

static int64_t stream_sort(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    static const struct graph_sink_ops ops={
        .consume=sort_batch,.finish=finish_sort,.borrow_projection=true
    };
    return graph_stream_sink(s,scan,sink,out,&ops);
}

int64_t pg_sina_graph_block_sort(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_SORT,stream_sort); }

static int start_sort_workers(struct execution *s,unsigned id,unsigned root)
{
    (void)id;
    if(!s->graph_source_pending) parallel_sort_begin(s,root);
    return 0;
}

int64_t pg_sina_graph_sort_wave(struct femu_csd_args *a)
{
    if(!graph_operator_args(a)) return -1;
    struct qp_plan *p=a->mr_addr[0];
    struct qp_control *control=a->mr_addr[1];
    struct execution *s=(void *)(uintptr_t)control->state;
    int id=graph_wave_node(a);
    if(id<0 || p->nodes[id].kind!=QP_SORT) return -1;
    if(!s) {
        int status=open_execution(a,p,control,&graph_project_ops,graph_index_init,&s);
        if(status) return status;
    }
    if(s->plan!=p) return -1;
    const struct qp_node *n=&p->nodes[id];
    int root=n->left;
    if(root<0 || (unsigned)root>=p->count) return -1;
    bool aggregate=p->nodes[root].kind==QP_AGGREGATE;
    if(aggregate && s->nodes[root].group_phase<2) return -1;
    s->args=a; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
    if(!csIsReentry(a)) control->drained=0;
    struct sorted_rows *sort=&s->nodes[id].left_sort;
    int64_t status=initialize_dictionary(s,a);
    if(status) return status;
    s->nodes[id].read_projection=projection_sort_fill;
    s->nodes[id].read_row=sort_read_row;
    status=sort_init(s,sort,&p->nodes[root].expr,n->expr.norder,NULL,n->expr.order);
    if(status) return status;
    const struct operator_state *input=&s->nodes[root];
    if(aggregate && !input->group_radix && (input->group_phase!=3 || !input->groups ||
       atomic_load(input->group_count)<QP_VECTOR_ROWS))
        return QP_GRAPH_PHASE_DONE|QP_GRAPH_SERIAL_SORT;
    if(s->profiling) s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID);
    const struct graph_wave_sink sink={.node=id,.start=start_sort_workers};
    status=parallel_wave_next(s,root,NULL,&sink);
    if(status==QP_WAIT) return s->workers[0] && s->workers[0]->sort_sink?0:-4;
    if(status==2 || status==3) return -4;
    if(status) return status;
    sort->complete=1;
    cs_work_free(s->context,sort->input); sort->input=NULL;
    return QP_GRAPH_PHASE_DONE;
}

static int64_t sort_sink_step(struct execution *,struct femu_csd_args *,unsigned,unsigned);

int64_t pg_sina_graph_sort_sink(struct femu_csd_args *a)
{ return graph_tail_call(a,sort_sink_step,true); }

static int64_t sort_source_step(struct execution *,struct femu_csd_args *,unsigned,unsigned);

int64_t pg_sina_graph_sort_source(struct femu_csd_args *a)
{ return graph_tail_call(a,sort_source_step,false); }

static int64_t sort_source_step(struct execution *s,struct femu_csd_args *a,unsigned worker,unsigned id)
{
    const struct qp_plan *p=s->plan;
    const struct qp_node *n=&p->nodes[id];
    int64_t status;
    (void)a;
    if(n->kind!=QP_SORT || n->left<0 || (unsigned)n->left>=p->count) return -1;
    struct sorted_rows *sort=&s->nodes[id].left_sort;
    if(worker || !sort->complete) return -1;
    while(!sort->drained) {
        status=sort_call(s,sort,1); if(status) return status;
        struct qs_result *out=sort->output;
        sort->drained=out->finished;
        unsigned kept=0;
        for(unsigned i=0;i<out->rows;++i) {
            struct qs_cell row[QS_OUT];
            int projected=project(s,n,out->data[i].cells,NULL,row);
            if(projected<0) return projected;
            if(projected) memcpy(out->data[kept++].cells,row,n->expr.nout*sizeof(*row));
        }
        out->rows=kept;
        if(kept) return graph_publish_output();
    }
    return graph_finish_stream();
}

static int64_t sort_sink_step(struct execution *s,struct femu_csd_args *a,unsigned worker,unsigned id)
{
    const struct qp_plan *p=s->plan;
    const struct qp_node *n=&p->nodes[id];
    int64_t status;
    struct qp_control *control=s->control;
    if(n->kind!=QP_SORT || n->left<0 || (unsigned)n->left>=p->count) return -1;
    struct sorted_rows *sort=&s->nodes[id].left_sort;

    if(worker) sort=s->sort_sink;
    if(!sort || (!worker && !sort->initialized)) return -1;
    if(a->graph_input==FEMU_GRAPH_INPUT_EOF) {
        sort->finishing_sink=1;
        status=sort_call(s,sort,0);
        if(status) return status;
        if(!worker) {
            sort->complete=1;
            sort->group.count=1; sort->group.states[0]=sort->task.state;
            cs_work_free(s->context,sort->input); sort->input=NULL;
            return csGraphStep(FEMU_GRAPH_STEP_DONE,id==p->root?QP_GRAPH_PHASE_DONE:0);
        }
        struct qs_result *out=(struct qs_result *)a->mr_addr[2]+worker;
        out->finished=control->finished=1;
        return csGraphStep(FEMU_GRAPH_STEP_DONE,QP_WORKER_FINISHED);
    }
    struct projection_batch *input=s->nodes[n->left].source_projection;
    if(!input) return -1;
    if(materialize_outputs(s,input))
        return s->text_wait==QS_DICTIONARY_BUSY?csGraphWait():s->text_wait?s->text_wait:-3;
    sort->columns=input;
    status=sort_call(s,sort,0); if(status) return status;
    input->position=input->count; sort->columns=NULL;
    return graph_consume_input();
}

int64_t sort_sink_vectors(struct execution *s,struct projection_batch *b)
{
    struct sorted_rows *sort=s->sort_sink;
    sort->columns=b;
    int64_t status=sort_call(s,sort,0);
    if(status) return status;
    b->position=b->count;
    sort->columns=NULL;
    return 0;
}
