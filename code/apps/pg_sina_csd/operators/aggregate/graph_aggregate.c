#include "graph_state.h"
#include "graph_batch.h"

int accumulate_value(struct execution *s,const struct qp_node *n,
    const struct qs_agg *agg,struct accumulator *a,struct qs_cell v)
{
    if(agg->kind==Q_MIN || agg->kind==Q_MAX) {
        int cmp=a->count?compare_cell(s,v,a->extreme,n->expr.exprs[agg->expr].type):0;
        if(cmp==INT_MIN) return -5;
        if(!a->count || (agg->kind==Q_MIN?cmp<0:cmp>0)) a->extreme=v;
    } else if(agg->kind==Q_SUM || agg->kind==Q_AVG) {
        if(!a->count) a->sum=v;
        else {
            unsigned scale=v.scale>a->sum.scale?v.scale:a->sum.scale; __int128 left,right;
            if(qs_scale(a->sum.value,scale-a->sum.scale,&left) || qs_scale(v.value,scale-v.scale,&right) ||
                __builtin_add_overflow(left,right,&a->sum.value)) return -5;
            a->sum.scale=scale; if(v.dscale>a->sum.dscale) a->sum.dscale=v.dscale;
        }
    }
    ++a->count;
    return 0;
}

int group_update(struct execution *s,const struct qp_node *n,unsigned aggregate,
    struct group_value *g,struct qs_cell value,uint64_t count)
{
    const struct qs_agg *a=&n->expr.aggs[aggregate];
    if(count && (a->kind==Q_MIN || a->kind==Q_MAX)) {
        int cmp=g->count?compare_cell(s,value,g->sum,n->expr.exprs[a->expr].type):0;
        if(cmp==INT_MIN) return -5;
        if(!g->count || (a->kind==Q_MIN?cmp<0:cmp>0)) g->sum=value;
    }
    return group_add(g,value,count,a->kind);
}

static void group_update_lock(struct group_update_state *u)
{
    if(!u->shared) return;
    unsigned state=atomic_load_explicit(&u->entry->state,memory_order_acquire);
    for(;;) {
        if(!(state&GROUP_LOCK) && atomic_compare_exchange_weak_explicit(&u->entry->state,&state,state|GROUP_LOCK,
            memory_order_acquire,memory_order_relaxed)) return;
        state=atomic_load_explicit(&u->entry->state,memory_order_acquire);
    }
}

static void group_update_unlock(struct group_update_state *u)
{ atomic_store_explicit(&u->entry->state,u->occupied,u->shared?memory_order_release:memory_order_relaxed); }

int group_update_continue(struct execution *s,unsigned id)
{
    struct group_update_state *u=s->group_update;
    if(!u || !u->active || u->node!=id) return -5;
    const struct qp_node *n=&s->plan->nodes[id];
    size_t bytes=n->expr.naggs*sizeof(*u->before);
    for(;;) {
        for(;u->aggregate<n->expr.naggs;++u->aggregate) {
            unsigned a=u->aggregate;
            int status=group_update(s,n,a,&u->after[a],u->input[a].sum,u->input[a].count);
            if(status) return status;
        }
        group_update_lock(u);
        /* Another CU may have committed while this one loaded a string.
         * Rebase on that complete state, rather than overwrite its update. */
        if(memcmp(u->before,u->group->values,bytes)) {
            memcpy(u->before,u->group->values,bytes); memcpy(u->after,u->before,bytes);
            u->aggregate=0; group_update_unlock(u); continue;
        }
        memcpy(u->group->values,u->after,bytes); group_update_unlock(u);
        u->active=0; return 1;
    }
}

static int group_sink_hashed_values(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *keys,const struct group_value *values,int shared,uint64_t hash)
{
    const struct qp_node *n=&s->plan->nodes[id];
    int paged=group_text_extreme(s,n);
    if(paged) {
        if(!s->group_update) s->group_update=cs_work_calloc(s->context,1,sizeof(*s->group_update));
        if(!s->group_update) return -4;
        if(s->group_update->active) {
            if(s->group_update->owner!=o || s->group_update->hash!=hash) return -5;
            return group_update_continue(s,id);
        }
    }
    int wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
    struct qs_cell key=keys[0];
    if(!wide && !key.isnull && (key.scale || key.value<INT64_MIN || key.value>INT64_MAX)) return -5;
    unsigned occupied=key.isnull?2:1;
    if(wide && o->group_integer) {
        occupied=1;
        for(unsigned k=0;k<qp_group_keys(n);++k) {
            if(!keys[k].isnull && (keys[k].scale || keys[k].value<INT64_MIN || keys[k].value>INT64_MAX)) return -5;
            if(keys[k].isnull) occupied|=2U<<k;
        }
    }
    unsigned flags=occupied; occupied=group_entry_state(occupied,hash);
    unsigned mask=o->group_capacity-1,at=hash&mask;
    for(;;at=(at+1)&mask) {
        struct group_entry *entry=&o->group_entries[at];
retry_bucket:
        ;
        unsigned state=atomic_load_explicit(&entry->state,shared?memory_order_acquire:memory_order_relaxed);
        /* A creator has not published its keys yet. An updater keeps the
         * occupied bits, so other probes can already compare immutable keys. */
        if(state==GROUP_LOCK) goto retry_bucket;
        unsigned old=state&~GROUP_LOCK;
        if(old && old!=occupied) continue;
        struct compact_group *g=old?group_at(o,entry->row):NULL;
        int equal=!old;
        if(old && wide && o->group_integer) {
            const int64_t *key_values=(const void *)(g->values+n->expr.naggs); equal=old==occupied;
            for(unsigned k=0;equal && k<qp_group_keys(n);++k)
                if(!keys[k].isnull && key_values[k]!=(int64_t)keys[k].value) equal=0;
        } else if(old) equal=wide?group_equal(s,n,keys,(struct qs_cell *)(g->values+n->expr.naggs)):
            old==occupied && (key.isnull || g->key==(int64_t)key.value);
        if(equal<0) return equal;
        if(!equal) continue;
        /* A partition owns matching values; new buckets still require
         * publication because probes from other partitions can collide. */
        if(shared && (!old || shared==GROUP_SHARED || paged)) {
            if(state&GROUP_LOCK) goto retry_bucket;
            if(!atomic_compare_exchange_weak_explicit(&entry->state,&state,state|GROUP_LOCK,
                memory_order_acquire,memory_order_relaxed)) goto retry_bucket;
        }
        if(!old) {
            /* Reserve once instead of making independent bucket creators
             * retry CAS on the same cache line. Failed reservations are
             * returned before the CU reports its unconsumed input. */
            unsigned count=atomic_fetch_add_explicit(o->group_count,1,memory_order_relaxed);
            if(count>=o->group_capacity-o->group_capacity/8) {
                atomic_fetch_sub_explicit(o->group_count,1,memory_order_relaxed);
                if(shared) atomic_store_explicit(&entry->state,0,memory_order_release);
                return 0;
            }
            entry->row=count; g=group_at(o,count);
            g->key=wide?(int64_t)hash:key.isnull?0:key.value;
            for(unsigned a=0;a<n->expr.naggs;++a) g->values[a]=(struct group_value){.sum={.isnull=1}};
            g->key_flags=flags;
            if(wide && o->group_integer) {
                int64_t *key_values=(void *)(g->values+n->expr.naggs);
                for(unsigned k=0;k<qp_group_keys(n);++k) key_values[k]=keys[k].isnull?0:(int64_t)keys[k].value;
            } else if(wide) memcpy(g->values+n->expr.naggs,keys,qp_group_keys(n)*sizeof(*keys));
            unsigned char *payload=(void *)(g->values+n->expr.naggs);
            payload+=qp_group_keys(n)*(o->group_integer?8:sizeof(*keys));
            for(unsigned k=qp_group_keys(n);k<n->nkeys;++k) {
                unsigned bytes=group_payload_bytes(n,k);
                hash_store_value(payload,&keys[k],bytes==9?8:16); payload+=bytes;
            }
        }
        if(paged) {
            struct group_update_state *u=s->group_update;
            u->owner=o; u->entry=entry; u->group=g; u->node=id; u->hash=hash;
            u->shared=shared; u->occupied=occupied; u->aggregate=0; u->active=1;
            size_t bytes=n->expr.naggs*sizeof(*values);
            memcpy(u->before,g->values,bytes); memcpy(u->after,g->values,bytes); memcpy(u->input,values,bytes);
            group_update_unlock(u);
            return group_update_continue(s,id);
        }
        int status=0;
        for(unsigned a=0;a<n->expr.naggs && !status;++a)
            status=group_update(s,n,a,&g->values[a],values[a].sum,values[a].count);
        if(!old || shared!=GROUP_PARTITION)
            atomic_store_explicit(&entry->state,occupied,shared?memory_order_release:memory_order_relaxed);
        return status?status:1;
    }
}

int group_sink_values(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *keys,const struct group_value *values,int shared)
{ return group_sink_hashed_values(s,id,o,keys,values,shared,group_values_hash(&s->plan->nodes[id],keys)); }

int group_worker_values(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *keys,const struct group_value *values,int shared,uint64_t hash)
{
    if(s->group_update && s->group_update->active) return group_update_continue(s,id);
    struct operator_state *local=shared?group_worker_local(s,id):NULL;
    if(local) {
        int added=group_sink_hashed_values(s,id,local,keys,values,GROUP_LOCAL,hash); if(added) return added;
        if(local->group_radix) return 0;
    }
    return group_sink_hashed_values(s,id,o,keys,values,shared,hash);
}

static void group_prefetch(struct operator_state *local,uint64_t hash,int payload)
{
    struct group_entry *entry=&local->group_entries[hash&(local->group_capacity-1)];
    __builtin_prefetch(entry,1,1);
    if(payload) {
        unsigned state=atomic_load_explicit(&entry->state,memory_order_relaxed);
        if(state && (state&GROUP_TAG_MASK)==(group_entry_state(0,hash)&GROUP_TAG_MASK))
            __builtin_prefetch(group_at(local,entry->row),1,1);
    }
}

static int group_reduce_small_batch(struct execution *s,unsigned id,
    struct projection_batch *b,struct operator_state *local)
{
    enum { MAX_VECTOR_GROUPS=8 };
    const struct qp_node *n=&s->plan->nodes[id];
    if(!local || n->expr.naggs<2 || n->estimated_groups>MAX_VECTOR_GROUPS ||
       atomic_load_explicit(local->group_count,memory_order_relaxed)+MAX_VECTOR_GROUPS>
           local->group_capacity-local->group_capacity/8) return 0;
    for(unsigned a=0;a<n->expr.naggs;++a) {
        unsigned kind=n->expr.aggs[a].kind;
        int expr=b->group_values[a];
        if(kind==Q_MIN || kind==Q_MAX ||
           (expr>=0 && (b->vectors[expr].kind==QS_DYNAMIC || b->vectors[expr].kind==QS_CELLS))) return 0;
    }
    struct qs_cell keys[MAX_VECTOR_GROUPS][QS_KEYS];
    uint16_t rows[MAX_VECTOR_GROUPS][QS_VECTOR_ROWS],counts[MAX_VECTOR_GROUPS]={0};
    unsigned groups=0;
    for(unsigned r=b->position;r<b->count;++r) {
        struct qs_cell key[QS_KEYS];
        for(unsigned k=0;k<n->nkeys;++k) {
            unsigned expr=b->group_keys[k];
            if(b->vectors[expr].errors[r]) return -5;
            key[k]=qs_vector_cell(&b->plan,b->vectors,expr,r);
        }
        unsigned g=0;
        for(;g<groups;++g) {
            int equal=group_equal(s,n,keys[g],key); if(equal<0) return equal;
            if(equal) break;
        }
        if(g==groups) {
            if(groups==MAX_VECTOR_GROUPS) return 0;
            memcpy(keys[groups++],key,n->nkeys*sizeof(*key));
        }
        rows[g][counts[g]++]=r;
    }
    for(unsigned g=0;g<groups;++g) {
        struct group_value values[QS_AGGS]={0};
        int status=group_reduce_run(n,b,rows[g],counts[g],values); if(status) return status;
        status=group_sink_values(s,id,local,keys[g],values,GROUP_LOCAL);
        if(status<=0) return status?status:-4;
    }
    s->metrics.matched+=b->count-b->position;
    b->position=b->count;
    return 1;
}

int64_t group_sink_vectors(struct execution *s,unsigned id,struct qs_result *out,unsigned capacity)
{
    struct projection_batch *b=s->nodes[id].projection;
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->owner->nodes[id];
    /* Unclustered keys would pay for comparisons without saving updates.
     * Inspect at most 16 adjacent rows before choosing local run combine. */
    int combine=0;
    unsigned sample=b->count-b->position; if(sample>16) sample=16;
    struct qs_cell previous[QS_KEYS];
    for(unsigned r=0;r<sample && !combine;++r) {
        struct qs_cell keys[QS_KEYS];
        for(unsigned k=0;k<n->nkeys;++k) {
            int index=b->group_keys[k]; if(b->vectors[index].errors[b->position+r]) return -5;
            keys[k]=qs_vector_cell(&b->plan,b->vectors,index,b->position+r);
        }
        if(r) { combine=group_equal(s,n,previous,keys); if(combine<0) return combine; }
        memcpy(previous,keys,n->nkeys*sizeof(*keys));
    }
    struct operator_state *local=group_worker_local(s,id);
    if(local && local->group_radix && local->group_phase) {
        int64_t status=radix_flush(s,id); if(status) return status;
    }
    int reduced=group_reduce_small_batch(s,id,b,local);
    if(reduced) return reduced<0?reduced:0;
    int prefetch=local && !combine && n->nkeys==1 && n->expr.exprs[n->expr.keys[0]].type==Q_INTEGER;
    uint64_t hashes[QS_VECTOR_ROWS];
    if(prefetch) for(unsigned r=b->position;r<b->count;++r) {
        struct qs_cell key=qs_vector_cell(&b->plan,b->vectors,b->group_keys[0],r);
        hashes[r]=mix_hash(key.isnull?0:(uint64_t)key.value);
        if(r<b->position+16) group_prefetch(local,hashes[r],0);
    }
    while(b->position<b->count && out->rows<capacity) {
        unsigned first=b->position;
        struct group_batch_input stack,*input=&stack; stack.phase=0;
        if(group_text_extreme(s,n)) {
            if(!b->group_input) b->group_input=cs_work_calloc(s->context,1,sizeof(*b->group_input));
            if(!b->group_input) return -4;
            input=b->group_input;
        }
        if(!input->phase) {
            input->end=first; input->aggregate=0;
            for(unsigned k=0;k<n->nkeys;++k) {
                int index=b->group_keys[k]; if(b->vectors[index].errors[first]) return -5;
                input->keys[k]=qs_vector_cell(&b->plan,b->vectors,index,first);
            }
            for(unsigned a=0;a<n->expr.naggs;++a) input->values[a]=(struct group_value){.sum={.isnull=1}};
            input->phase=1;
        }
        if(prefetch) {
            if(first+16<b->count) group_prefetch(local,hashes[first+16],0);
            if(first+4<b->count) group_prefetch(local,hashes[first+4],1);
        }
        /* Keep this adjacent-key partial until its hash update commits. */
        if(input->phase==1) do {
            unsigned end=input->end;
            if(end!=first && !input->aggregate) {
                struct qs_cell next[QS_KEYS];
                for(unsigned k=0;k<n->nkeys;++k) {
                    int index=b->group_keys[k]; if(b->vectors[index].errors[end]) return -5;
                    next[k]=qs_vector_cell(&b->plan,b->vectors,index,end);
                }
                int equal=group_equal(s,n,input->keys,next); if(equal<0) return equal;
                if(!equal) break;
            }
            for(;input->aggregate<n->expr.naggs;++input->aggregate) {
                unsigned a=input->aggregate;
                struct qs_cell value={.value=1};
                if(b->group_values[a]>=0) {
                    int index=b->group_values[a]; if(b->vectors[index].errors[end]) return -5;
                    value=qs_vector_cell(&b->plan,b->vectors,index,end);
                }
                int status=group_update(s,n,a,&input->values[a],value,!value.isnull); if(status) return status;
            }
            input->aggregate=0; ++input->end;
        } while(combine && input->end<b->count && input->end-first<capacity-out->rows);
        input->phase=2;
        int added=group_worker_values(s,id,o,input->keys,input->values,GROUP_SHARED,
            prefetch?hashes[first]:group_values_hash(n,input->keys));
        if(added<0) return added;
        if(!added) {
            if(local && local->group_radix) {
                local->group_phase=1;
                int64_t status=radix_flush(s,id); if(status) return status;
                continue; /* Retry this uncommitted input against the empty table. */
            }
            for(unsigned r=first;r<input->end;++r) {
                struct qs_cell *row=(struct qs_cell *)out->data+(size_t)out->rows*b->plan.nout;
                for(unsigned col=0;col<b->plan.nout;++col) {
                    int index=b->plan.outputs[col].index; if(b->vectors[index].errors[r]) return -5;
                    row[col]=qs_vector_cell(&b->plan,b->vectors,index,r);
                }
                ++out->rows;
            }
        }
        s->metrics.matched+=input->end-first; b->position=input->end; input->phase=0;
    }
    return 0;
}

int aggregate_sink_vectors(struct execution *s,unsigned id)
{
    struct projection_batch *b=s->nodes[id].projection;
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    for(unsigned a=b->aggregate_position;a<n->expr.naggs;++a,b->aggregate_position=a) {
        const struct qs_agg *agg=&n->expr.aggs[a]; int index=b->plan.outputs[a].index;
        if(agg->kind==Q_COUNT_STAR) { o->accum[a].count+=b->count; b->aggregate_row=0; continue; }
        const struct qs_vector *v=&b->vectors[index];
        for(unsigned r=b->aggregate_row;r<b->count;++r,b->aggregate_row=r) {
            if(v->errors[r]) return -5;
            if(qs_vector_null(v,r)) continue;
            struct qs_cell value=qs_vector_cell(&b->plan,b->vectors,index,r);
            int status=accumulate_value(s,n,agg,&o->accum[a],value); if(status) return status;
        }
        b->aggregate_row=0;
    }
    b->aggregate_position=0;
    b->position=b->count; s->metrics.matched+=b->count; return 0;
}

static int64_t projection_group_fill_impl(struct execution *s,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[b->scan];
    if(b->error) { s->control->error=b->error; s->control->error_node=b->error_node; return b->error; }
    while(b->input_count<QS_VECTOR_ROWS) {
        struct qs_cell values[QS_COLS]; int64_t status;
        int previous=s->control->error; unsigned previous_node=s->control->error_node;
        if(s->worker && s->source_node==(int)b->scan) {
            if(s->owner->nodes[b->scan].group_radix) {
                status=radix_source_next(s,b->scan,values,1);
            } else if(s->owner->nodes[b->scan].group_phase==2) {
                status=aggregate_sorted_source_next(s,b->scan,values,1);
            } else {
                const struct compact_group *g=group_source_next(s,b->scan);
                status=g?2:0;
                if(g) {
                    group_keys(n,&s->owner->nodes[b->scan],g,values);
                    for(unsigned a=0;a<n->expr.naggs;++a) {
                        status=group_final_value(&n->expr.aggs[a],&g->values[a],&values[n->nkeys+a]);
                        if(status) break;
                        status=2;
                    }
                }
            }
        } else status=s->nodes[b->scan].finished?0:aggregate_next_impl(s,b->scan,values,1);
        if(status==QP_WAIT || status>2) return status;
        if(status<0) {
            if(s->expression_wait) return status;
            b->error=status; b->error_node=s->control->error?s->control->error_node:b->scan;
            s->control->error=previous; s->control->error_node=previous_node;
            break;
        }
        if(!status) { s->nodes[b->scan].finished=1; break; }
        for(unsigned col=0;col<b->plan.nattrs;++col) {
            int slot=b->plan.cols[col].slot;
            if(slot>=0) b->cells[(size_t)slot*QS_VECTOR_ROWS+b->input_count]=values[col];
        }
        ++b->input_count;
    }
    b->count=b->input_count; b->input_count=b->position=0;
    if(!b->count) {
        if(b->error) { s->control->error=b->error; s->control->error_node=b->error_node; }
        return b->error;
    }
    struct qs_vector_input input={.cells=b->cells,.dictionary=s->dictionary,
        .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    if(projection_execute(b,NULL,NULL,0,input)) return -5;
    return 2;
}

int64_t projection_group_fill(struct execution *s,struct projection_batch *b)
{
    if(!s->profiling || (s->worker && !s->owner->nodes[b->scan].group_radix) || !s->plan->nodes[b->scan].hash)
        return projection_group_fill_impl(s,b);
    /* Count external Source/expressions per vector, not per output group.
     * Millions of CLOCK_THREAD_CPUTIME_ID calls otherwise serialize profiling
     * itself. Nested Sink/Sort CPU remains excluded by blocking_end(). */
    struct blocking_timer timer=blocking_begin(s);
    ++s->group_source_profiling;
    int64_t status=projection_group_fill_impl(s,b);
    --s->group_source_profiling;
    blocking_end(s,QS_AGG_SOURCE,timer);
    return status;
}

int64_t group_combine_local(struct execution *s)
{
    unsigned id=s->group_combine-1;
    struct operator_state *local=s->nodes[id].group_local,*global=&s->owner->nodes[id];
    if(s->group_shard) {
        const struct qp_node *n=&s->plan->nodes[id];
        int wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
        struct operator_state *cursor=&s->nodes[id];
        for(;cursor->group_local_slot<global->group_local_count;++cursor->group_local_slot) {
            local=global->group_locals[cursor->group_local_slot]; unsigned rows=atomic_load(local->group_count);
            for(;cursor->group_local_row<rows;++cursor->group_local_row) {
                unsigned r=cursor->group_local_row;
                struct compact_group *g=group_at(local,r);
                uint64_t hash=wide?(uint64_t)g->key:mix_hash(g->key_flags==2?0:(uint64_t)g->key);
                unsigned workers=s->plan->workers;
                unsigned partition=(workers&(workers-1))?hash%workers:hash&(workers-1);
                if(partition!=s->worker-1) continue;
                struct qs_cell keys[QS_KEYS]; group_keys(n,local,g,keys);
                int added=group_sink_hashed_values(s,id,global,keys,g->values,GROUP_PARTITION,hash); if(added<0) return added;
                if(!added) { s->group_merge_full=true; return 0; }
            }
            cursor->group_local_row=0;
        }
        return 0;
    }
    unsigned rows=atomic_load_explicit(local->group_count,memory_order_relaxed);
    while(local->group_flush<rows) {
        struct compact_group *g=group_at(local,local->group_flush); struct qs_cell keys[QS_KEYS];
        group_keys(&s->plan->nodes[id],local,g,keys);
        int added=group_sink_values(s,id,global,keys,g->values,1); if(added<0) return added;
        if(!added) break; /* The owner resumes this partial after growth/spill. */
        ++local->group_flush;
    }
    return 0;
}

static int64_t aggregate_batch(struct execution *s,unsigned id,struct projection_batch *batch,struct qs_result *out)
{
    if(s->aggregate_sink) return aggregate_sink_vectors(s,id);
    if(out->rows==out->begin) {
        out->rows=out->begin=0;
        int64_t status=group_sink_vectors(s,id,out,QS_RESULT_ROWS*QS_OUT/batch->plan.nout);
        if(status) return status;
    }
    return graph_spool_result(s,out,batch->plan.nout);
}

static int64_t finish_aggregate(struct execution *s,unsigned sink)
{
    struct operator_state *local=s->nodes[sink].group_local;
    return s->group_sink && local && local->group_radix?radix_flush(s,sink):0;
}

static int64_t stream_aggregate(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    static const struct graph_sink_ops ops={.consume=aggregate_batch,.finish=finish_aggregate};
    return graph_stream_sink(s,scan,sink,out,&ops);
}

int64_t pg_sina_graph_block_aggregate(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_GROUP,stream_aggregate); }

static int aggregate_wave_node(const struct femu_csd_args *a)
{
    int id=graph_wave_node(a);
    if(id<0) return -1;
    const struct qp_plan *p=a->mr_addr[0];
    const struct qp_node *n=&p->nodes[id];
    if(n->kind!=QP_AGGREGATE || n->left<0 || (unsigned)n->left>=p->count ||
       (n->nkeys && !n->hash)) return -1;
    return id;
}

int64_t pg_sina_graph_aggregate_reserve(struct femu_csd_args *a)
{
    int id=aggregate_wave_node(a); if(id<0) return -1;
    struct execution *s;
    int64_t status=graph_wave_begin(a,&graph_project_ops,graph_index_init,&s); if(status) return status;
    struct operator_state *o=&s->nodes[id];
    if(!s->plan->nodes[id].nkeys) {
        o->phase=1;
        return QP_GRAPH_PHASE_DONE;
    }
    status=hash_aggregate_next_impl(s,id,NULL,GROUP_ALLOCATE,0);
    if(status!=3 || !o->groups) return status==3 || !status?-4:status;
    return QP_GRAPH_PHASE_DONE;
}

static int64_t aggregate_read_row(struct execution *s,unsigned id,struct qs_cell *row)
{
    if(s->worker && s->source_node==(int)id && s->owner->partition_join<0)
        return aggregate_worker_source(s,id,row);
    int status=projection_init(s,id,1); if(status) return status;
    if(s->nodes[id].source_projection) return projection_next(s,id,row,1);
    return aggregate_next_impl(s,id,row,0);
}

int64_t pg_sina_graph_aggregate_wave(struct femu_csd_args *a)
{
    int id=aggregate_wave_node(a); if(id<0) return -1;
    struct execution *s;
    int64_t status=graph_wave_begin(a,&graph_project_ops,graph_index_init,&s); if(status) return status;
    struct operator_state *o=&s->nodes[id];
    const struct qp_node *n=&s->plan->nodes[id];
    if(!o->graph_wave_drained && !o->graph_wave_input) {
        if(!s->parallel_started && n->nkeys && !o->groups) return -1;
        if(s->profiling) s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID);
        status=aggregate_wave_next(s,n->left,NULL,id);
        if(status==QP_WAIT) return 0;
        if(n->nkeys && status==2) o->graph_wave_input=true;
        else {
            if(status==2 || status==3) return -4;
            if(status) return status;
            o->graph_wave_drained=true;
        }
    }
    if(!n->nkeys) {
        o->input_done=1;
        o->read_row=aggregate_read_row;
        return QP_GRAPH_PHASE_DONE;
    }
    status=hash_aggregate_next_impl(s,id,NULL,GROUP_PREPARE,0);
    if(status==QP_WAIT && o->graph_wave_input) return 0;
    if(status!=3) return status==QP_WAIT || !status?-4:status;
    o->read_row=aggregate_read_row;
    return QP_GRAPH_PHASE_DONE;
}

int64_t pg_sina_graph_aggregate_combine(struct femu_csd_args *a)
{
    if(!graph_operator_args(a)) return -1;
    unsigned worker=((uint64_t)a->cparam1>>1)&127;
    const struct qp_plan *p=a->mr_addr[0];
    if(!worker || worker>p->workers) return -1;
    struct qp_control *control=(struct qp_control *)a->mr_addr[1]+worker;
    struct execution *s=(void *)(uintptr_t)control->state;
    if(!s || s->plan!=p || !s->group_combine) return -1;
    s->args=a; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
    struct qs_result *out=(struct qs_result *)a->mr_addr[2]+worker;
    int64_t status=group_combine_local(s);
    if(status) return status;
    out->finished=control->finished=1;
    plan_output_metrics(s,out,worker);
    return QP_WORKER_FINISHED;
}


static bool aggregate_result_ready(const struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id];
    const struct operator_state *o=&s->nodes[id];
    return n->kind==QP_AGGREGATE && (n->nkeys?o->group_phase>=2:o->input_done);
}

static int64_t aggregate_result_next(struct execution *s,unsigned id,struct qs_cell *row)
{ return aggregate_next_impl(s,id,row,0); }

int64_t pg_sina_graph_aggregate_result(struct femu_csd_args *a)
{
    static const struct graph_result_source source={.ready=aggregate_result_ready,.next=aggregate_result_next};
    qs_dictionary_graph_context(a);
    int64_t status=graph_emit(a,GRAPH_EMIT_AGGREGATE,&source);
    qs_dictionary_graph_context(NULL);
    return status;
}

static int64_t aggregate_source_step(struct execution *s,struct femu_csd_args *a,unsigned worker,unsigned id)
{
    struct qp_node *n=&s->plan->nodes[id];
    int64_t status;
    (void)a;
    struct execution *owner=worker?s->owner:s;
    if(n->kind!=QP_AGGREGATE || (worker && s->source_node!=(int)id) ||
       owner->nodes[id].group_phase<2) return -1;
    const struct operator_state *source=&owner->nodes[id];
    if(source->group_radix ? worker>source->group_radix->sources :
       (worker>1 && source->group_phase==2))
        return graph_finish_stream();
    unsigned parallel=n->parallel;
    if(!worker) n->parallel=0;
    status=projection_init(s,id,1);
    n->parallel=parallel;
    if(status) return status;
    if(!s->nodes[id].source_projection) return -4;
    status=projection_fill(s,id,1);
    if(s->text_wait) return s->text_wait==QS_DICTIONARY_BUSY?csGraphWait():s->text_wait;
    if(status==2) {
        s->nodes[id].published_batch=&s->nodes[id].source_projection;
        return graph_publish_output();
    }
    return status?status:graph_finish_stream();
}

int64_t pg_sina_graph_aggregate_source(struct femu_csd_args *a)
{ return graph_tail_call(a,aggregate_source_step,false); }

static int valid_batch(const struct femu_csd_args *a)
{
    return a && a->numr==2 && a->mr_len[0]>=(long long)sizeof(struct qs_result) &&
        a->mr_len[1]>=(long long)sizeof(struct qs_graph_tail);
}

int64_t pg_sina_graph_sum(struct femu_csd_args *a)
{
    if(!valid_batch(a)) return -1;
    struct qs_result *out=a->mr_addr[0];
    struct qs_graph_tail *state=a->mr_addr[1];
    if(out->status || out->begin>QS_RESULT_ROWS || out->rows>QS_RESULT_ROWS-out->begin) return -1;
    for(unsigned i=out->begin;i<out->begin+out->rows;++i) {
        const struct qs_cell *cell=&out->data[i].cells[0];
        if(cell->isnull) continue;
        if(state->sum.isnull) state->sum=*cell;
        else {
            if(cell->scale!=state->sum.scale ||
                __builtin_add_overflow(state->sum.value,cell->value,&state->sum.value)) return -1;
            if(cell->dscale>state->sum.dscale) state->sum.dscale=cell->dscale;
        }
    }
    if(!out->finished) return 0;
    out->begin=0;
    out->rows=1;
    out->data[0].cells[0]=state->sum;
    return 1;
}

/* Legacy pulling paths share the same accumulator and vector sink as EXEC. */
int64_t aggregate_accept_vectors(struct execution *s,unsigned id,struct qs_result *out,unsigned capacity)
{
    return out?group_sink_vectors(s,id,out,capacity):aggregate_sink_vectors(s,id);
}
