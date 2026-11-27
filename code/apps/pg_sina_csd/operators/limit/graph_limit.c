#include "graph_state.h"
#include "graph_batch.h"

static int64_t limit_step(struct execution *,struct femu_csd_args *,unsigned,unsigned);

int64_t pg_sina_graph_limit(struct femu_csd_args *a)
{ return graph_tail_call(a,limit_step,false); }

static int64_t limit_step(struct execution *s,struct femu_csd_args *a,unsigned worker,unsigned id)
{
    struct qp_plan *p=s->plan;
    const struct qp_node *n=&p->nodes[id];
    struct qp_control *control=s->control;
    int64_t status;
    if(worker || n->kind!=QP_LIMIT || n->left<0 || (unsigned)n->left>=p->count || id!=p->root) return -1;
    struct qs_result *out=a->mr_addr[2];
    unsigned capacity=qp_host_capacity(&n->expr);
    if(!s->host_building && !s->closing) {
        out->rows=out->begin=out->finished=out->str_bytes=0;
        host_begin(s,&n->expr,out,capacity);
        s->host_building=1;
    }
    if(a->graph_input==FEMU_GRAPH_INPUT_EOF) {
        if(!s->closing) {
            qp_host_compact(out,&n->expr,capacity);
            control->output_strings=out->str_bytes;
            plan_output_metrics(s,out,0);
            control->peak_cus=atomic_load(&s->peak_cus);
            s->closing=1;
        }
        status=graph_finish(s,a,out);
        if(status>0 && status<32 && !control->state)
            return csGraphStep(FEMU_GRAPH_STEP_DONE,status);
        return status;
    }
    const struct qs_result *input=s->nodes[n->left].left_sort.output;
    struct operator_state *o=&s->nodes[id];
    for(;o->position<input->rows;++o->position) {
        if(n->expr.limit>=0 && o->emitted>=(uint64_t)n->expr.limit) break;
        if(o->skipped<(uint64_t)n->expr.offset) { ++o->skipped; continue; }
        if(!s->host_pending) {
            int count=project(s,n,input->data[o->position].cells,NULL,s->host_row);
            if(s->text_wait) return s->text_wait==QS_DICTIONARY_BUSY?csGraphWait():s->text_wait;
            if(count<0) return count;
            if(!count) continue;
            s->host_pending=1;
        }
        int appended=host_append(s,&n->expr,out);
        if(s->late_wait) return s->late_wait;
        if(s->text_wait) return s->text_wait==QS_DICTIONARY_BUSY?csGraphWait():s->text_wait;
        if(appended) return appended<0?appended:-4;
        ++o->emitted;
    }
    o->position=0;
    return graph_consume_input();
}

static int valid_batch(const struct femu_csd_args *a)
{
    return a && a->numr==2 && a->mr_len[0]>=(long long)sizeof(struct qs_result) &&
        a->mr_len[1]>=(long long)sizeof(struct qs_graph_tail);
}

int64_t pg_sina_graph_offset(struct femu_csd_args *a)
{
    if(!valid_batch(a)) return -1;
    struct qs_result *out=a->mr_addr[0];
    struct qs_graph_tail *state=a->mr_addr[1];
    if(out->status || out->begin>QS_RESULT_ROWS || out->rows>QS_RESULT_ROWS-out->begin) return -1;
    unsigned skip=state->remaining<out->rows?state->remaining:out->rows;
    state->remaining-=skip;
    out->begin+=skip;
    out->rows-=skip;
    return 0;
}
