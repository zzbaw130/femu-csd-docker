/* Per-CU workmem hash aggregation, then device merge and exact finalization. */
#include "femu-csd-kernel.h"
#include "cs_workmem.h"
#include "query_vector.h"
struct accumulator { __int128 sum; uint64_t count; uint8_t dscale; };
struct group { struct qs_cell keys[QS_KEYS]; struct accumulator aggs[QS_AGGS]; };
struct state { cs_work_context *context; struct qs_plan plan; struct qs_metrics metrics;
    struct qs_vector *vectors;
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
int64_t pg_sina_aggregate_sink(struct femu_csd_args *a)
{
    if(!a || a->numr!=3 || a->mr_len[1]<(long long)sizeof(struct qs_plan) || a->mr_len[2]<(long long)sizeof(struct qs_task)) return -1;
    struct qs_chunk *c=a->mr_addr[0]; const struct qs_plan *p=a->mr_addr[1]; struct qs_task *task=a->mr_addr[2];
    if(p->nkeys>QS_KEYS || p->naggs>QS_AGGS || p->nexprs>QS_EXPRS ||
        a->mr_len[0]<(long long)sizeof(*c) || qs_chunk_bytes(c->capacity,c->ncols)>(uint64_t)a->mr_len[0] ||
        c->selected>c->rows || c->rows>c->capacity) return -1;
    struct state *s=(void *)(uintptr_t)task->state;
    if(!s) {
        cs_work_context *context=cs_work_begin(); if(!context) return -4;
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
        if(qs_vector_execute(p,c,qs_selection(c),first,count,s->needed,s->vectors)) return -5;
        struct group *groups[QS_VECTOR_ROWS];
        for(unsigned i=0;i<count;++i) {
            if(!p->nkeys) { groups[i]=&s->groups[0]; continue; }
            struct qs_cell keys[QS_KEYS];
            for(unsigned k=0;k<p->nkeys;++k) {
                if(s->vectors[p->keys[k]].errors[i]) return -5;
                keys[k]=qs_vector_cell(p,s->vectors,p->keys[k],i);
            }
            groups[i]=get_group(s,keys); if(!groups[i]) return -6;
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
                    if(values->nulls[i]) continue;
                    struct accumulator *v=&groups[i]->aggs[j]; ++v->count;
                    if(__builtin_add_overflow(v->sum,values->values[i],&v->sum)) return -5;
                    if(values->dscales[i]>v->dscale) v->dscale=values->dscales[i];
                }
            } else {
                for(unsigned i=0;i<count;++i) {
                    if(values->errors[i]) return -5;
                    if(!values->nulls[i]) ++groups[i]->aggs[j].count;
                }
            }
        }
    }
    return 0;
}
int64_t pg_sina_aggregate_source(struct femu_csd_args *a)
{
    if(!a || (a->numr!=1 && a->numr!=2) || a->mr_len[0]<(long long)sizeof(struct qs_group)) return -1;
    struct qs_group *group=a->mr_addr[0];
    if(!group->count || group->count>QS_WORKERS) return -1;
    if(a->numr==1) {
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
    out->rows=s->count; out->metrics=s->metrics; out->metrics.workers=group->count;
    struct cs_work_stats stats; if(!cs_work_get_stats(&stats)) out->metrics.work_peak=stats.peak_used_bytes;
    for(unsigned i=0;i<s->count;++i) for(unsigned j=0;j<p->nout;++j) {
        const struct qs_output *o=&p->outputs[j]; struct qs_cell value={0};
        if(!o->aggregate) value=s->groups[i].keys[o->index];
        else {
            const struct qs_agg *agg=&p->aggs[o->index]; const struct accumulator *v=&s->groups[i].aggs[s->accumulator[o->index]];
            if(agg->kind==Q_COUNT || agg->kind==Q_COUNT_STAR) value.value=v->count;
            else {
                value=(struct qs_cell){.value=v->sum,.scale=p->exprs[agg->expr].scale,.dscale=v->dscale,.isnull=!v->count};
                if(agg->kind==Q_AVG && v->count && qs_average(value,v->count,&value)) return -5;
            }
        }
        out->data[i].cells[j]=value;
    }
    cs_work_end(s->context); group->states[0]=0; return 0;
}
