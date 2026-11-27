#include "graph_state.h"

#include <stdlib.h>

static int64_t external_set_probe(struct execution *s,unsigned id,const struct qs_expr *expression,
    const struct qs_cell *keys,const unsigned *rows,unsigned count,struct qs_cell **values)
{
    struct operator_state *o=&s->nodes[id];
    const struct operator_state *set=graph_state_owner(s,id);
    struct set_probe *p=o->set_probe;
    while(p && p->expression!=expression) p=p->next;
    if(!p) {
        p=cs_work_calloc(s->context,1,sizeof(*p)); if(!p) return -4;
        p->pages=set->set_pages; p->pages.cache=NULL; p->pages.loaded=UINT_MAX;
        p->expression=expression; p->next=o->set_probe; o->set_probe=p;
    }
    int same=p->count==count;
    for(unsigned i=0;same && i<count;++i)
        same=p->rows[i]==rows[i] && p->keys[i].value==keys[i].value && p->keys[i].isnull==keys[i].isnull;
    if(!same) {
        p->count=count; p->block=0; p->ready=0;
        for(unsigned i=0;i<count;++i) {
            p->keys[i]=keys[i]; p->rows[i]=rows[i];
            p->values[i]=(struct qs_cell){.isnull=keys[i].isnull?(set->set_count || set->set_null):set->set_null};
        }
    }
    while(!p->ready && p->block<=p->pages.blocks) {
        unsigned block=p->block,used=block<p->pages.blocks?QS_SORT_IO_BYTES/sizeof(int64_t):p->pages.used;
        if(used) {
            int64_t first;
            int64_t status=spool_row_bytes(s,&p->pages,(uint64_t)block*(QS_SORT_IO_BYTES/sizeof(int64_t)),sizeof(first),&first);
            if(status) return status;
            const int64_t *data=(const void *)(block<p->pages.blocks?p->pages.cache:p->pages.tail);
            for(unsigned i=0;i<count;++i) {
                struct qs_cell key=keys[i];
                if(key.isnull || p->values[i].value || key.value<INT64_MIN || key.value>INT64_MAX ||
                   key.value<data[0] || key.value>data[used-1]) continue;
                unsigned lo=0,hi=used;
                while(lo<hi) { unsigned mid=lo+(hi-lo)/2; if(data[mid]<(int64_t)key.value) lo=mid+1; else hi=mid; }
                if(lo<used && data[lo]==key.value) p->values[i]=(struct qs_cell){.value=1};
            }
        }
        ++p->block;
    }
    p->ready=1; *values=p->values; return 0;
}

static int set_order(const void *a,const void *b)
{ int64_t x=*(const int64_t *)a,y=*(const int64_t *)b; return (x>y)-(x<y); }

static int64_t set_append(struct execution *s,struct operator_state *o,int64_t value)
{
    if(o->set_pages.used==QS_SORT_IO_BYTES/sizeof(value))
        qsort(o->set_pages.tail,o->set_pages.used,sizeof(value),set_order);
    return spool_append_bytes(s,&o->set_pages,&value,sizeof(value));
}

static int64_t set_insert(struct execution *s,struct operator_state *o,struct qs_cell value)
{
    if(value.isnull) { o->set_null=1; return 0; }
    if(value.value>INT64_MAX || value.value<INT64_MIN) return -5;
    if(!o->set_external && (!o->set_capacity || o->set_count*2>=o->set_capacity)) {
        unsigned capacity=o->set_capacity?o->set_capacity*2:64;
        struct set_key *next=cs_work_calloc(s->context,capacity,sizeof(*next));
        if(!next) o->set_external=1;
        else {
            for(unsigned i=0;i<o->set_capacity;++i) if(o->set[i].occupied) {
                unsigned slot=mix_hash((uint64_t)o->set[i].value)&(capacity-1);
                while(next[slot].occupied) slot=(slot+1)&(capacity-1);
                next[slot]=o->set[i];
            }
            cs_work_free(s->context,o->set);
            o->set=next; o->set_capacity=capacity;
        }
    }
    if(o->set_external) {
        if(o->set) {
            for(;o->set_convert<o->set_capacity;++o->set_convert) {
                const struct set_key *key=&o->set[o->set_convert];
                if(key->occupied) {
                    int64_t status=set_append(s,o,key->value); if(status) return status;
                }
            }
            cs_work_free(s->context,o->set); o->set=NULL; o->set_capacity=0;
        }
        int64_t status=set_append(s,o,(int64_t)value.value); if(status) return status;
        ++o->set_count;
        return 0;
    }
    unsigned slot=mix_hash((uint64_t)value.value)&(o->set_capacity-1);
    while(o->set[slot].occupied && o->set[slot].value!=value.value) slot=(slot+1)&(o->set_capacity-1);
    if(!o->set[slot].occupied) { o->set[slot]=(struct set_key){.value=value.value,.occupied=1}; ++o->set_count; }
    return 0;
}

static void set_finish(struct operator_state *o)
{
    if(o->set_external && o->set_pages.used)
        qsort(o->set_pages.tail,o->set_pages.used,sizeof(int64_t),set_order);
    if(o->set_external) o->lookup_spilled_set=external_set_probe;
    o->set_ready=1;
}

int64_t build_set(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id]; const struct qp_node *n=&s->plan->nodes[id];
    while(!o->set_ready) {
        if(!o->have_left) {
            int64_t status=next_row(s,n->left,o->left);
            if(status==QP_WAIT || status<0 || status>2) return status;
            if(!status) { set_finish(o); break; }
            o->have_left=1;
        }
        int64_t status=set_insert(s,o,o->left[0]); if(status) return status;
        o->have_left=0;
    }
    return 0;
}

static int64_t set_combine(struct execution *s,unsigned id,struct execution *worker)
{
    struct operator_state *from=&worker->nodes[id],*to=&s->nodes[id];
    to->set_null|=from->set_null;
    for(;from->set_convert<from->set_capacity;++from->set_convert) {
        const struct set_key *key=&from->set[from->set_convert];
        if(key->occupied) {
            int64_t status=set_insert(s,to,(struct qs_cell){.value=key->value}); if(status) return status;
        }
    }
    for(;worker->source_at<from->set_pages.rows;++worker->source_at) {
        int64_t key;
        int64_t status=spool_row_bytes(s,&from->set_pages,worker->source_at,sizeof(key),&key);
        if(status) return status;
        status=set_insert(s,to,(struct qs_cell){.value=key}); if(status) return status;
    }
    while(from->set_pages.blocks) {
        uint64_t token=from->set_pages.pages[--from->set_pages.blocks].token;
        if(token) return csNeedFree(token);
    }
    cs_work_free(s->context,from->set_pages.cache); from->set_pages.cache=NULL;
    return 0;
}

static int64_t set_sink_vectors(struct execution *s,struct projection_batch *b);

static int64_t set_batch(struct execution *s,unsigned id,struct projection_batch *batch,struct qs_result *out)
{
    (void)id;
    (void)out;
    return set_sink_vectors(s,batch);
}

static int64_t finish_set(struct execution *s,unsigned sink)
{
    (void)sink;
    set_finish(&s->nodes[s->set_sink-1]);
    return 0;
}

static int64_t stream_set(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    if(graph_device_source(&s->plan->nodes[scan]) && s->args->graph_input==FEMU_GRAPH_INPUT_EOF)
        return 3;
    static const struct graph_sink_ops ops={.consume=set_batch,.finish=finish_set};
    return graph_stream_sink(s,scan,sink,out,&ops);
}

int64_t pg_sina_graph_block_set(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_SET,stream_set); }

int64_t pg_sina_graph_set_wave(struct femu_csd_args *a)
{
    int id=graph_wave_node(a); if(id<0) return -1;
    const struct qp_plan *p=a->mr_addr[0];
    int root=p->nodes[id].left;
    if(p->nodes[id].kind!=QP_SET || root<0 || (unsigned)root>=p->count) return -1;
    struct execution *s;
    int64_t status=graph_wave_begin(a,&graph_project_ops,graph_index_init,&s); if(status) return status;
    if(s->initializer>=p->ninit || p->init_nodes[s->initializer]!=id ||
       p->init_params[s->initializer]>=0 || p->nodes[root].expr.nout!=1) return -1;
    if(s->profiling) s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID);
    const struct graph_wave_sink sink={.node=id,.set=id+1,.merge=set_combine};
    status=parallel_wave_next(s,root,NULL,&sink);
    if(status==QP_WAIT) return 0;
    if(status) return status==3?-4:status;
    set_finish(&s->nodes[id]);
    ++s->initializer;
    return QP_GRAPH_PHASE_DONE;
}

static int64_t set_sink_vectors(struct execution *s,struct projection_batch *b)
{
    unsigned index=b->plan.outputs[0].index;
    for(;b->position<b->count;++b->position) {
        if(b->vectors[index].errors[b->position]) return -5;
        struct qs_cell key=qs_vector_cell(&b->plan,b->vectors,index,b->position);
        int64_t status=set_insert(s,&s->nodes[s->set_sink-1],key);
        if(status) return status;
    }
    return 0;
}
