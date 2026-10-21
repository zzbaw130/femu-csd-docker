/* Per-CU workmem hash aggregation, then device merge and exact finalization. */
#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "query_vector.h"
#include "cs_csf.h"
static int64_t external_sink(struct femu_csd_args *);
static int64_t external_source(struct femu_csd_args *);
static void external_drop(struct qs_group *);
struct accumulator { __int128 sum; uint64_t count; uint8_t dscale; };
struct group { struct qs_cell keys[QS_KEYS]; struct accumulator aggs[QS_AGGS]; };
struct state { cs_work_context *context; struct qs_plan plan; struct qs_metrics metrics;
    struct qs_vector *vectors; struct qs_dict *dict;
    uint8_t needed[QS_EXPRS], accumulator[QS_AGGS], sum_required[QS_AGGS];
    unsigned count; uint16_t buckets[1024]; struct group groups[QS_GROUPS]; };
static struct group *get_group(struct state *s,struct qs_cell *keys)
{
    uint64_t hash=1469598103934665603ULL;
    for(unsigned i=0;i<s->plan.nkeys;++i) { hash^=(uint64_t)keys[i].value+(keys[i].isnull?0x9e3779b9U:0); hash*=1099511628211ULL; }
    unsigned at=hash%1024;
    for(;;at=(at+1)%1024) {
        if(!s->buckets[at]) {
            if(s->count==QS_GROUPS) return NULL;
            struct group *g=&s->groups[s->count]; memcpy(g->keys,keys,s->plan.nkeys*sizeof(*keys));
            s->buckets[at]=++s->count; return g;
        }
        struct group *g=&s->groups[s->buckets[at]-1]; unsigned i;
        for(i=0;i<s->plan.nkeys;++i) if(g->keys[i].isnull!=keys[i].isnull || (!keys[i].isnull && g->keys[i].value!=keys[i].value)) break;
        if(i==s->plan.nkeys) return g;
    }
}
static int accumulate(struct state *s,struct qs_chunk *c,unsigned first,unsigned count)
{
    const struct qs_plan *p=&s->plan;
    if(qs_vector_execute(p,c,qs_selection(c),first,count,s->needed,s->vectors)) return -5;
    struct group *groups[QS_VECTOR_ROWS];
    for(unsigned i=0;i<count;++i) {
        if(!p->nkeys) { groups[i]=&s->groups[0]; continue; }
        struct qs_cell keys[QS_KEYS];
        for(unsigned k=0;k<p->nkeys;++k) {
            if(s->vectors[p->keys[k]].errors[i]) return -5;
            keys[k]=qs_vector_cell(p,s->vectors,p->keys[k],i);
        }
        groups[i]=get_group(s,keys); if(!groups[i]) return 1;
    }
    /* One update per distinct input expression: SUM/AVG/COUNT(expr) share
     * its accumulator, and each output is finalized independently. */
    for(unsigned j=0;j<p->naggs;++j) {
        if(s->accumulator[j]!=j) continue;
        const struct qs_agg *agg=&p->aggs[j];
        if(agg->kind==Q_COUNT_STAR) {
            if(!p->nkeys) s->groups[0].aggs[j].count+=count;
            else for(unsigned i=0;i<count;++i) ++groups[i]->aggs[j].count;
            continue;
        }
        const struct qs_vector *values=&s->vectors[agg->expr];
        if(s->sum_required[j]) {
            for(unsigned i=0;i<count;++i) {
                if(values->errors[i]) return -5;
                if(qs_vector_null(values,i)) continue;
                struct accumulator *v=&groups[i]->aggs[j]; ++v->count;
                if(__builtin_add_overflow(v->sum,qs_vector_value(values,i),&v->sum)) return -5;
                unsigned dscale=qs_vector_dscale(values,i);
                if(dscale>v->dscale) v->dscale=dscale;
            }
        } else {
            for(unsigned i=0;i<count;++i) {
                if(values->errors[i]) return -5;
                if(!qs_vector_null(values,i)) ++groups[i]->aggs[j].count;
            }
        }
    }
    return 0;
}
int64_t pg_sina_aggregate_sink(struct femu_csd_args *a)
{
    if(!a || (a->numr!=3 && a->numr!=4) || a->mr_len[1]<(long long)sizeof(struct qs_plan) || a->mr_len[2]<(long long)sizeof(struct qs_task)) return -1;
    struct qs_chunk *c=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1]; struct qs_task *task=a->mr_addr[2];
    if(p->external_aggregate) return external_sink(a);
    if(a->numr!=3) return -1;
    if(p->nkeys>QS_KEYS || p->naggs>QS_AGGS || p->nexprs>QS_EXPRS ||
        a->mr_len[0]<(long long)sizeof(*c) || qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0] ||
        c->selected>c->rows || c->rows>c->capacity) return -1;
    struct state *s=(void *)(uintptr_t)task->state;
    if(!s) {
        size_t limit=qs_sink_memory(p,task->workers); if(!limit) return -4;
        cs_work_context *context=cs_work_begin_limit(limit); if(!context) return -4;
        s=cs_work_calloc(context,1,sizeof(*s)); if(!s) { cs_work_end(context); return -4; }
        s->context=context; s->plan=*p;
        if(p->nexprs) s->vectors=cs_work_alloc(context,p->nexprs*sizeof(*s->vectors));
        if(p->nexprs && !s->vectors) { cs_work_end(context); return -4; }
        for(unsigned k=0;k<p->nkeys;++k)
            if(qs_vector_require(p,p->keys[k],s->needed)) { cs_work_end(context); return -5; }
        for(unsigned j=0;j<p->naggs;++j) {
            const struct qs_agg *agg=&p->aggs[j]; unsigned shared=0;
            if(agg->kind!=Q_COUNT_STAR && qs_vector_require(p,agg->expr,s->needed)) { cs_work_end(context); return -5; }
            for(;shared<j;++shared)
                if(p->aggs[shared].expr==agg->expr &&
                   (p->aggs[shared].kind==Q_COUNT_STAR)==(agg->kind==Q_COUNT_STAR)) break;
            s->accumulator[j]=shared<j?s->accumulator[shared]:j;
            if(agg->kind==Q_SUM || agg->kind==Q_AVG) s->sum_required[s->accumulator[j]]=1;
        }
        task->state=(uintptr_t)s;
        if(!p->nkeys) { struct qs_cell keys[QS_KEYS]={{0}}; get_group(s,keys); }
    }
    s->metrics.scanned+=c->rows; s->metrics.matched+=c->selected;
    s->metrics.input_bytes+=task->input_bytes; ++s->metrics.chunks;
    for(unsigned first=0;first<c->selected;first+=QS_VECTOR_ROWS) {
        unsigned count=c->selected-first; if(count>QS_VECTOR_ROWS) count=QS_VECTOR_ROWS;
        int status=accumulate(s,c,first,count); if(status) return status>0?-6:status;
    }
    return 0;
}
static int finalize_group(const struct state *s,const struct group *g,struct qs_row *row)
{
    const struct qs_plan *p=&s->plan; struct qs_cell values[QS_AGGS];
    for(unsigned j=0;j<p->naggs;++j) {
        const struct qs_agg *agg=&p->aggs[j]; const struct accumulator *v=&g->aggs[s->accumulator[j]];
        struct qs_cell value={0};
        if(agg->kind==Q_COUNT || agg->kind==Q_COUNT_STAR) value.value=v->count;
        else {
            value=(struct qs_cell){.value=v->sum,.scale=p->exprs[agg->expr].scale,.dscale=v->dscale,.isnull=!v->count};
            if(agg->kind==Q_AVG && v->count && qs_average(value,v->count,&value)) return -5;
        }
        values[j]=value;
    }
    struct qs_eval eval={.plan=p,.aggregates=values};
    for(unsigned j=0;j<p->nout;++j) {
        const struct qs_output *o=&p->outputs[j];
        if(o->aggregate==2) { if(qs_evaluate(&eval,o->index,&row->cells[j])) return -5; }
        else if(o->aggregate) row->cells[j]=values[o->index];
        else {
            row->cells[j]=g->keys[o->index];
            if(p->exprs[p->keys[o->index]].type==Q_STRING) row->cells[j].dscale=2;
        }
    }
    if(p->having>=0) {
        struct qs_eval hv={.plan=p,.aggregates=values}; struct qs_cell keep;
        if(qs_evaluate(&hv,p->having,&keep)) return -5;
        if(keep.isnull || !keep.value) return 1;
    }
    return 0;
}
static int output_expr(const struct qs_plan *p,unsigned j)
{
    const struct qs_output *o=&p->outputs[j];
    if(o->aggregate==0) return p->keys[o->index];
    if(o->aggregate==2) return o->index;
    return -1;
}
/* Resolve dictionary IDs only when a final result block leaves the source;
 * group keys and partial states stay fixed-size IDs through sorting/spill. */
static int resolve_result_strings(const struct state *s,struct qs_result *out)
{
    const struct qs_plan *p=&s->plan;
    out->str_bytes=0;
    for(unsigned r=0;r<out->rows;++r) for(unsigned j=0;j<p->nout;++j) {
        int expr=output_expr(p,j); if(expr<0 || p->exprs[expr].type!=Q_STRING) continue;
        struct qs_cell *cell=&out->data[r].cells[j];
        if(cell->isnull || cell->dscale!=2) continue;
        if(!s->dict) return -1;
        const char *bytes=NULL; unsigned length=0;
        if(qs_dict_get(s->dict,(unsigned)cell->value,&bytes,&length)) return -1;
        if(length<=QS_STRING_MAX) qs_cell_set_string(cell,bytes,length);
        else {
            if(out->str_bytes+length>QS_RESULT_STRING_BYTES || length>255) return -1;
            memcpy(out->str_data+out->str_bytes,bytes,length);
            qs_cell_set_string_ref(cell,out->str_bytes,length);
            out->str_bytes+=length;
        }
    }
    return 0;
}
int64_t pg_sina_aggregate_source(struct femu_csd_args *a)
{
    if(!a || (a->numr!=1 && a->numr!=2 && a->numr!=3 && a->numr!=4) || a->mr_len[0]<(long long)sizeof(struct qs_group)) return -1;
    struct qs_group *group=a->mr_addr[0];
    if(!group->count || group->count>QS_WORKERS) return -1;
    if(a->numr==3 || a->numr==4) return external_source(a);
    if(a->numr==1) {
        for(unsigned i=0;i<group->count;++i) if(group->states[i]) {
            struct state *s=(void *)(uintptr_t)group->states[i];
            if(s->plan.external_aggregate) { external_drop(group); return 0; }
            break;
        }
        for(unsigned i=0;i<group->count;++i) if(group->states[i]) {
            struct state *s=(void *)(uintptr_t)group->states[i]; cs_work_end(s->context); group->states[i]=0;
        }
        return 0;
    }
    if(a->mr_len[1]<(long long)sizeof(struct qs_result) || !group->states[0]) return -1;
    struct state *s=(void *)(uintptr_t)group->states[0]; const struct qs_plan *p=&s->plan;
    struct qs_result *out=a->mr_addr[1]; memset(out,0,sizeof(*out));
    for(unsigned i=1;i<group->count;++i) if(group->states[i]) {
        struct state *other=(void *)(uintptr_t)group->states[i];
        for(unsigned j=0;j<other->count;++j) {
            struct group *g=get_group(s,other->groups[j].keys); if(!g) return -6;
            for(unsigned k=0;k<p->naggs;++k) {
                if(s->accumulator[k]!=k) continue;
                struct accumulator *dest=&g->aggs[k],*src=&other->groups[j].aggs[k];
                if(__builtin_add_overflow(dest->sum,src->sum,&dest->sum)) return -5;
                dest->count+=src->count; if(src->dscale>dest->dscale) dest->dscale=src->dscale;
            }
        }
        s->metrics.scanned+=other->metrics.scanned; s->metrics.matched+=other->metrics.matched;
        s->metrics.input_bytes+=other->metrics.input_bytes; s->metrics.chunks+=other->metrics.chunks;
        cs_work_end(other->context); group->states[i]=0;
    }
    out->rows=s->count; out->finished=1; out->metrics=s->metrics; out->metrics.workers=group->count;
    struct cs_work_stats stats; if(!cs_work_get_stats(&stats)) out->metrics.work_peak=stats.peak_used_bytes;
    for(unsigned i=0;i<s->count;++i)
        if(finalize_group(s,&s->groups[i],&out->data[i])) return -5;
    cs_work_end(s->context); group->states[0]=0; return 0;
}

/* Local hash preaggregation keeps duplicate-heavy input small. Full tables
 * become sorted partial-state rows, reusing sort.c's bounded run storage.
 * Only key cells are compared; other cells carry opaque accumulator records. */
extern int64_t pg_sina_sort_impl(struct femu_csd_args *);
extern int64_t pg_sina_sort_sink_impl(struct femu_csd_args *);
extern int64_t pg_sina_sort_source_impl(struct femu_csd_args *);
_Static_assert(sizeof(struct accumulator)==sizeof(struct qs_cell), "partial aggregate cell size");
struct external_state {
    struct state local; /* Common prefix also used by abort cleanup. */
    struct qs_plan sort_plan;
    struct qs_task sort_task, output_task;
    struct qs_group inputs, output;
    struct qs_result *partial;
    struct qs_metrics input_metrics, output_metrics;
    uint64_t output_budget, skipped, emitted;
    uint32_t nested_counts[1024];
    unsigned position;
    unsigned finalize, partial_position;
    int flushing, input_started, input_finished, have_group, output_pending, pulling;
    int reducing_finished, finishing, nested_ready;
};
static void sort_drop(struct qs_group *group)
{
    if(!group->count || !group->states[0]) return;
    void *mr[]={group}; long long len[]={sizeof(*group)};
    struct femu_csd_args call={.numr=1,.mr_addr=mr,.mr_len=len};
    pg_sina_sort_source_impl(&call);
}
static void external_drop(struct qs_group *group)
{
    struct external_state *root=NULL;
    for(unsigned i=0;i<group->count;++i) if(group->states[i]) { root=(void *)(uintptr_t)group->states[i]; break; }
    if(!root) return;
    if(root->input_started) sort_drop(&root->inputs);
    else for(unsigned i=0;i<group->count;++i) if(group->states[i]) {
        struct external_state *s=(void *)(uintptr_t)group->states[i];
        struct qs_group one={.count=1,.states={s->sort_task.state}}; sort_drop(&one);
    }
    sort_drop(&root->output);
    for(unsigned i=0;i<group->count;++i) if(group->states[i]) {
        struct state *s=(void *)(uintptr_t)group->states[i];
        cs_work_end(s->context); group->states[i]=0;
    }
}
static int external_init(const struct qs_plan *p,struct qs_task *task,struct external_state **out)
{
    size_t limit=qs_sink_memory(p,task->workers);
    size_t fixed=(sizeof(struct external_state)+(size_t)p->nexprs*sizeof(struct qs_vector)+
        sizeof(struct qs_result)+4095)/4096*4096+4096;
    if(!p->nkeys || p->nkeys>QS_KEYS || p->naggs>QS_AGGS || p->nexprs>QS_EXPRS ||
        p->nkeys+p->naggs>QS_OUT || limit<fixed+2*QS_SORT_IO_BYTES) return -4;
    cs_work_context *context=cs_work_begin_limit(fixed); if(!context) return -4;
    struct external_state *e=cs_work_calloc(context,1,sizeof(*e));
    if(!e) { cs_work_end(context); return -4; }
    struct state *s=&e->local; s->context=context; s->plan=*p;
    s->vectors=cs_work_alloc(context,p->nexprs*sizeof(*s->vectors));
    e->partial=cs_work_calloc(context,1,sizeof(*e->partial));
    if(!s->vectors || !e->partial) { cs_work_end(context); return -4; }
    for(unsigned k=0;k<p->nkeys;++k)
        if(qs_vector_require(p,p->keys[k],s->needed)) { cs_work_end(context); return -5; }
    for(unsigned j=0;j<p->naggs;++j) {
        const struct qs_agg *agg=&p->aggs[j]; unsigned shared=0;
        if(agg->kind!=Q_COUNT_STAR && qs_vector_require(p,agg->expr,s->needed)) { cs_work_end(context); return -5; }
        for(;shared<j;++shared) if(p->aggs[shared].expr==agg->expr &&
            (p->aggs[shared].kind==Q_COUNT_STAR)==(agg->kind==Q_COUNT_STAR)) break;
        s->accumulator[j]=shared<j?s->accumulator[shared]:j;
        if(agg->kind==Q_SUM || agg->kind==Q_AVG) s->sum_required[s->accumulator[j]]=1;
    }
    unsigned workers=task->workers?task->workers:1;
    size_t run_budget=(limit-fixed)/(p->norder?2:1);
    e->output_budget=(limit-fixed-run_budget)*workers;
    if(p->join.relid && p->norder) {
        /* PG releases the build hash before source starts. Its old reservation
         * can hold the final result sort, leaving this slot's full remainder
         * for partial-state runs and their resident metadata during probe. */
        struct cs_work_stats stats;
        if(cs_work_get_stats(&stats)) { cs_work_end(context); return -4; }
        run_budget=limit-fixed; e->output_budget=p->join.work_mem_bytes;
        if(!e->output_budget || e->output_budget>stats.capacity_bytes/2) e->output_budget=stats.capacity_bytes/2;
    }
    e->sort_plan=(struct qs_plan){.nout=p->nkeys+p->naggs,.norder=p->nkeys,
        .ordered_rows=2,.predicate=-1,.limit=-1,.work_mem_bytes=run_budget};
    for(unsigned k=0;k<p->nkeys;++k) e->sort_plan.order[k]=(struct qs_order){.output=k,.nulls_first=1};
    e->sort_task.workers=workers;
    task->state=(uintptr_t)e; *out=e; return 0;
}
static int64_t partial_flush(struct external_state *e,struct femu_csd_args *call,void *io,long long io_bytes)
{
    struct state *s=&e->local; const struct qs_plan *p=&s->plan;
    if(!e->flushing) {
        e->partial->rows=s->count;
        for(unsigned i=0;i<s->count;++i) {
            memcpy(e->partial->data[i].cells,s->groups[i].keys,p->nkeys*sizeof(struct qs_cell));
            for(unsigned j=0;j<p->naggs;++j)
                memcpy(&e->partial->data[i].cells[p->nkeys+j],&s->groups[i].aggs[j],sizeof(struct accumulator));
        }
        e->flushing=1;
    }
    void *mr[]={e->partial,&e->sort_plan,&e->sort_task,io};
    long long len[]={sizeof(*e->partial),sizeof(e->sort_plan),sizeof(e->sort_task),io_bytes};
    call->numr=4; call->mr_addr=mr; call->mr_len=len;
    int64_t status=pg_sina_sort_sink_impl(call);
    if(!status) {
        call->cparam2=0; e->flushing=0; s->count=0;
        memset(s->buckets,0,sizeof(s->buckets)); memset(s->groups,0,sizeof(s->groups));
    }
    return status;
}
static int64_t external_sink(struct femu_csd_args *a)
{
    if(a->numr!=4 || a->mr_len[3]<QS_SORT_IO_BYTES) return -1;
    struct qs_chunk *c=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1]; struct qs_task *task=a->mr_addr[2];
    if(a->mr_len[0]<(long long)sizeof(*c) || c->rows>c->capacity || c->selected>c->rows ||
        qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0]) return -1;
    struct external_state *e=(void *)(uintptr_t)task->state;
    if(!e) { int status=external_init(p,task,&e); if(status) return status; }
    struct state *s=&e->local;
    if(!csIsReentry(a)) {
        e->position=0;
        s->metrics.scanned+=c->rows; s->metrics.matched+=c->selected;
        s->metrics.input_bytes+=task->input_bytes; ++s->metrics.chunks;
    }
    struct femu_csd_args call=*a; call.cparam1=0;
    if(e->flushing) { int64_t status=partial_flush(e,&call,a->mr_addr[3],a->mr_len[3]); if(status) return status; }
    while(e->position<c->selected) {
        unsigned count=c->selected-e->position; if(count>QS_VECTOR_ROWS) count=QS_VECTOR_ROWS;
        int status=accumulate(s,c,e->position,count);
        if(status<0) return status;
        if(status>0) { int64_t io=partial_flush(e,&call,a->mr_addr[3],a->mr_len[3]); if(io) return io; continue; }
        e->position+=count;
    }
    return 0;
}
static int same_keys(const struct qs_plan *p,const struct group *g,const struct qs_row *row)
{
    for(unsigned k=0;k<p->nkeys;++k) {
        const struct qs_cell *x=&g->keys[k],*y=&row->cells[k];
        if(x->isnull!=y->isnull || (!x->isnull && x->value!=y->value)) return 0;
    }
    return 1;
}
static int partial_combine(struct external_state *e,const struct qs_row *row)
{
    struct state *s=&e->local; const struct qs_plan *p=&s->plan; struct group *g=&s->groups[0];
    if(!e->have_group) { memset(g,0,sizeof(*g)); memcpy(g->keys,row->cells,p->nkeys*sizeof(struct qs_cell)); e->have_group=1; }
    for(unsigned j=0;j<p->naggs;++j) if(s->accumulator[j]==j) {
        struct accumulator value; memcpy(&value,&row->cells[p->nkeys+j],sizeof(value));
        struct accumulator *v=&g->aggs[j];
        if(__builtin_add_overflow(v->sum,value.sum,&v->sum) || __builtin_add_overflow(v->count,value.count,&v->count)) return -5;
        if(value.dscale>v->dscale) v->dscale=value.dscale;
    }
    return 0;
}
static int resolve_output_strings(struct external_state *e,struct qs_row *row)
{
    const struct qs_plan *p=&e->local.plan;
    for(unsigned j=0;j<p->nout;++j) {
        int expr=output_expr(p,j); if(expr<0 || p->exprs[expr].type!=Q_STRING) continue;
        struct qs_cell *cell=&row->cells[j];
        if(cell->isnull || cell->dscale!=2) continue;
        const char *bytes=NULL; unsigned length=0;
        if(!e->local.dict || qs_dict_get(e->local.dict,(unsigned)cell->value,&bytes,&length)) return -5;
        if(length<=QS_STRING_MAX) qs_cell_set_string_sort(cell,bytes,length);
    }
    return 0;
}
static int group_output(struct external_state *e,struct qs_row *row)
{
    int status=finalize_group(&e->local,&e->local.groups[0],row);
    if(status<0) return -5;
    if(status>0) return 1;
    return resolve_output_strings(e,row);
}
static void external_metrics(struct external_state *e,struct qs_result *out)
{
    out->metrics=e->local.metrics;
    out->metrics.runs=e->input_metrics.runs+e->output_metrics.runs;
    out->metrics.spilled_runs=e->input_metrics.spilled_runs+e->output_metrics.spilled_runs;
    out->metrics.spill_read_bytes=e->input_metrics.spill_read_bytes+e->output_metrics.spill_read_bytes;
    out->metrics.spill_write_bytes=e->input_metrics.spill_write_bytes+e->output_metrics.spill_write_bytes;
    struct cs_work_stats stats; if(!cs_work_get_stats(&stats)) out->metrics.work_peak=stats.peak_used_bytes;
}
static int64_t external_source(struct femu_csd_args *a)
{
    if(a->mr_len[1]<(long long)sizeof(struct qs_result) ||
       a->mr_len[a->numr-1]<QS_SORT_IO_BYTES) return -1;
    void *io=a->mr_addr[a->numr-1]; long long io_bytes=a->mr_len[a->numr-1];
    struct qs_group *group=a->mr_addr[0]; struct external_state *e=(void *)(uintptr_t)group->states[0];
    if(!e) return -1;
    if(a->numr==4) e->local.dict=a->mr_addr[2];
    struct state *s=&e->local; const struct qs_plan *p=&s->plan; struct qs_result *out=a->mr_addr[1];
    struct femu_csd_args call=*a; call.cparam1=0;
    if(!csIsReentry(a)) { out->status=0; out->rows=out->begin=out->finished=0; out->str_bytes=0; }
    /* Estimates choose costs, not correctness. If no local table has flushed
     * and their union certainly fits, retain the original small hash merge. */
    if(!p->nested && !e->finalize && !e->input_started && !csIsReentry(a)) {
        unsigned count=0; int small=1;
        for(unsigned i=0;i<group->count;++i) {
            struct external_state *owner=(void *)(uintptr_t)group->states[i];
            count+=owner->local.count; if(owner->sort_task.state) small=0;
        }
        if(small && count<=QS_GROUPS) {
            struct qs_plan plan=*p;
            void *mr[]={group,out}; long long len[]={sizeof(*group),sizeof(*out)};
            call.numr=2; call.mr_addr=mr; call.mr_len=len;
            int64_t status=pg_sina_aggregate_source(&call); if(status) return status;
            mr[0]=out; mr[1]=&plan; len[0]=sizeof(*out); len[1]=sizeof(plan);
            for(unsigned r=0;r<out->rows;++r) if(resolve_output_strings(e,&out->data[r])) return -5;
            if(plan.norder) { status=pg_sina_sort_impl(&call); if(status) return status; }
            out->begin=(uint64_t)plan.offset>out->rows?out->rows:(unsigned)plan.offset;
            out->rows-=out->begin;
            if(plan.limit>=0 && (uint64_t)plan.limit<out->rows) out->rows=plan.limit;
            if(resolve_result_strings(s,out)) return -5;
            return 0;
        }
    }
    /* Flush each local table once. Reentry continues the current owner's I/O. */
    while(e->finalize<group->count) {
        struct external_state *owner=(void *)(uintptr_t)group->states[e->finalize];
        int64_t status=partial_flush(owner,&call,io,io_bytes); if(status) return status;
        if(owner->local.vectors) { cs_work_free(owner->local.context,owner->local.vectors); owner->local.vectors=NULL; }
        e->inputs.states[e->finalize]=owner->sort_task.state;
        if(e->finalize) {
            s->metrics.scanned+=owner->local.metrics.scanned; s->metrics.matched+=owner->local.metrics.matched;
            s->metrics.input_bytes+=owner->local.metrics.input_bytes; s->metrics.chunks+=owner->local.metrics.chunks;
        }
        ++e->finalize;
    }
    if(!e->input_started) {
        e->inputs.count=group->count; e->input_started=1; e->partial->rows=0; s->metrics.workers=group->count;
        if(p->norder) {
            e->sort_plan=*p; e->sort_plan.ordered_rows=2; e->sort_plan.predicate=-1;
            e->sort_plan.nexprs=0; e->sort_plan.work_mem_bytes=e->output_budget;
            e->output_task.workers=1; e->output.count=1;
        }
    }
    for(;;) {
        if(e->output_pending) {
            void *mr[]={out,&e->sort_plan,&e->output_task,io};
            long long len[]={sizeof(*out),sizeof(e->sort_plan),sizeof(e->output_task),io_bytes};
            call.numr=4; call.mr_addr=mr; call.mr_len=len;
            int64_t status=pg_sina_sort_sink_impl(&call); e->output.states[0]=e->output_task.state;
            if(status) return status;
            call.cparam2=0; e->output_pending=0; out->rows=0;
        }
        if(e->nested_ready) {
            uint64_t keys[QS_RESULT_ROWS],counts[QS_RESULT_ROWS]; unsigned nr=0;
            for(unsigned k=0;k<1024;++k) if(e->nested_counts[k]) {
                if(nr==QS_RESULT_ROWS) return -5;
                keys[nr]=k; counts[nr]=e->nested_counts[k]; ++nr;
            }
            for(unsigned i=0;i<nr;++i) for(unsigned j=i+1;j<nr;++j) if(counts[j]>counts[i] || (counts[j]==counts[i] && keys[j]>keys[i])) {
                uint64_t t=keys[i]; keys[i]=keys[j]; keys[j]=t; t=counts[i]; counts[i]=counts[j]; counts[j]=t;
            }
            for(unsigned r=0;r<nr;++r) {
                out->data[r].cells[0]=(struct qs_cell){.value=keys[r]};
                out->data[r].cells[1]=(struct qs_cell){.value=counts[r]};
            }
            out->rows=nr; out->finished=1; e->nested_ready=0; external_metrics(e,out); external_drop(group); return 0;
        }
        if(e->finishing) {
            if(e->inputs.states[0]) {
                void *mr[]={&e->inputs,io}; long long len[]={sizeof(e->inputs),io_bytes};
                call.numr=2; call.mr_addr=mr; call.mr_len=len;
                int64_t status=pg_sina_sort_source_impl(&call); if(status) return status;
                call.cparam2=0;
            }
            out->finished=1; external_metrics(e,out);
            if(resolve_result_strings(s,out)) return -5;
            external_drop(group);
            return 0;
        }
        if(e->reducing_finished) {
            if(!p->norder) { e->finishing=1; continue; }
            void *mr[]={&e->output,out,io}; long long len[]={sizeof(e->output),sizeof(*out),io_bytes};
            call.numr=3; call.mr_addr=mr; call.mr_len=len;
            int64_t status=pg_sina_sort_source_impl(&call); if(status) return status;
            e->output_metrics=out->metrics; external_metrics(e,out);
            if(resolve_result_strings(s,out)) return -5;
            if(out->finished) external_drop(group);
            return 0;
        }
        if(e->pulling || (e->partial_position==e->partial->rows && !e->input_finished)) {
            void *mr[]={&e->inputs,e->partial,io}; long long len[]={sizeof(e->inputs),sizeof(*e->partial),io_bytes};
            call.numr=3; call.mr_addr=mr; call.mr_len=len;
            e->pulling=1;
            int64_t status=pg_sina_sort_source_impl(&call); if(status) return status;
            e->pulling=0; call.cparam2=0; e->partial_position=0; e->input_finished=e->partial->finished;
            e->input_metrics=e->partial->metrics;
        }
        const struct qs_row *row=e->partial_position<e->partial->rows?&e->partial->data[e->partial_position]:NULL;
        if(e->have_group && (!row || !same_keys(p,&s->groups[0],row))) {
            if(p->nested) {
                uint64_t key=(uint64_t)s->groups[0].aggs[p->nested_key_agg].count;
                if(key>=1024) return -5;
                ++e->nested_counts[key];
            } else {
                if(p->norder || e->skipped++>=(uint64_t)p->offset) {
                    if(p->norder || p->limit<0 || e->emitted<(uint64_t)p->limit) {
                        int keep=group_output(e,&out->data[out->rows]);
                        if(keep<0) return -5;
                        if(keep==0) { ++out->rows; ++e->emitted; }
                    }
                }
                if(!p->norder && p->limit>=0 && e->emitted>=(uint64_t)p->limit) e->finishing=1;
                if(out->rows==QS_RESULT_ROWS) {
                    if(p->norder) e->output_pending=1;
                    else { external_metrics(e,out); if(resolve_result_strings(s,out)) return -5; return 0; }
                }
            }
            e->have_group=0;
            if(e->output_pending || e->finishing) continue;
        }
        if(row) {
            if(partial_combine(e,row)) return -5;
            ++e->partial_position;
        } else if(e->input_finished) {
            e->reducing_finished=1;
            if(p->nested) e->nested_ready=1;
            else if(p->norder) e->output_pending=1; /* Also initialize the empty sort. */
        }
    }
}
