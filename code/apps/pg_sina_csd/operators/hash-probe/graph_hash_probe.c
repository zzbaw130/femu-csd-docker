#include "graph_state.h"
#include "graph_batch.h"

void external_advance(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    hash_release(s->context,o);
    o->hash_rows=o->hash_capacity=o->hash_phase=o->hash_match=0;
    o->have_left=0; o->external->probe_at=0; o->external->phase=o->external->direct?4:2;
    if(o->external->direct) o->finished=1;
}
int64_t external_probe(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct external_hash *e=s->nodes[id].external;
    const struct qs_plan *p=&s->plan->nodes[n->left].expr;
    if(e->direct) return next_row(s,n->left,row);
    struct spool *input=&e->probe[e->partition];
    if(e->probe_at==input->rows) return 0;
    unsigned char packed[QS_OUT*19];
    int64_t status=spool_row_bytes(s,input,e->probe_at,hash_row_bytes(p),packed); if(status) return status;
    hash_load(row,packed,p); ++e->probe_at; return 2;
}
/* Hash matching only needs equality; canonical text IDs avoid reading
 * dictionary bodies for candidates that already passed the hash lookup. */
int keys_equal(struct execution *s,const struct qp_node *n,const struct qs_cell *left,const struct qs_cell *right)
{
    const struct qs_plan *types=&s->plan->nodes[n->left].expr;
    for(unsigned i=0;i<n->nkeys;++i) {
        unsigned col=n->left_keys[i];
        int equal=equal_cell(s,left[col],right[n->right_keys[i]],types->exprs[types->outputs[col].index].type);
        if(equal<=0) return equal;
    }
    return 1;
}
int keys_null(const struct qp_node *n,const struct qs_cell *left)
{ for(unsigned i=0;i<n->nkeys;++i) if(left[n->left_keys[i]].isnull) return 1; return 0; }

static int64_t hash_probe_input(struct execution *,unsigned,const uint32_t *);

static void borrow_hash_build(struct execution *s,unsigned id,struct operator_state *local)
{
    const struct operator_state *build=graph_state_owner(s,id);
    if(!s->owner || !build->hash_phase) return;
    local->hash_data=build->hash_data; local->hash_blocks=build->hash_blocks;
    local->hash_rows=build->hash_rows; local->hash_capacity=build->hash_capacity;
    local->hash_buckets=build->hash_buckets; local->hash_phase=1; local->hash_shared=1;
}

static void hash_qual_close(struct execution *s,struct operator_state *o)
{
    if(!o->hash_qual) return;
    cs_work_free(s->context,o->hash_qual->expr.cells);
    cs_work_free(s->context,o->hash_qual->expr.vectors);
    cs_work_free(s->context,o->hash_qual); o->hash_qual=NULL;
}
/* Batch residual conditions over a duplicate-key chain. Single candidates
 * keep the scalar path; retained indices let one probe produce many output
 * batches without advancing its input. Errors surface only when consumed. */
static int hash_qual_next(struct execution *s,unsigned id,uint32_t first,const uint32_t *next)
{
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    struct operator_state *o=&s->nodes[id]; unsigned stride=hash_row_bytes(&right->expr);
    struct hash_qual_batch *q=o->hash_qual;
    if(q && q->expr.retry) goto evaluate_hash;
    if(first!=UINT32_MAX) {
        if(!o->hash_match || (o->hash_qual_checked && !q)) return 3;
        uint32_t rows[QS_VECTOR_ROWS],cursor=o->hash_match; unsigned count=1;
        rows[0]=first;
        unsigned capacity=n->join_type==QP_SEMI || n->join_type==QP_ANTI?64:QS_VECTOR_ROWS;
        while(cursor && count<capacity) {
            unsigned at=cursor-1; cursor=next[at]==UINT_MAX?0:next[at]+1;
            hash_load(o->right,hash_row(o,at,stride),&right->expr);
            int equal=keys_equal(s,n,o->left,o->right); if(equal<0) return -5;
            if(!equal) continue;
            int null=0; for(unsigned k=0;k<n->nkeys;++k) null|=o->right[n->right_keys[k]].isnull;
            if(!null) rows[count++]=at;
        }
        if(count==1) { o->hash_match=cursor; goto scalar; }
        if(!q) {
            o->hash_qual_checked=1;
            q=cs_work_calloc(s->context,1,sizeof(*q)); if(!q) goto scalar;
            if(join_qual_init(s,id,&q->expr,NULL)) { cs_work_free(s->context,q); goto scalar; }
            o->hash_qual=q;
        }
        struct projection_batch *b=&q->expr;
        int status=hash_left_payload(s,id); if(status) return status;
        unsigned ln=s->plan->nodes[n->left].expr.nout;
        for(unsigned r=0;r<count;++r) {
            q->rows[r]=rows[r];
            hash_load(o->right,hash_row(o,rows[r],stride),&right->expr);
            for(unsigned col=0;col<b->plan.nattrs;++col) {
                int slot=b->plan.cols[col].slot; if(slot<0) continue;
                b->cells[(size_t)slot*QS_VECTOR_ROWS+r]=col<ln?o->left[col]:o->right[col-ln];
            }
        }
        b->count=count; b->position=0; q->cursor=cursor;
    evaluate_hash:;
        struct qs_vector_input input={.cells=q->expr.cells,.dictionary=s->dictionary,
            .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
        if(projection_execute(&q->expr,NULL,NULL,0,input)) return -5;
        q->expr.position=0; o->hash_match=q->cursor;
    }
    struct projection_batch *b=&q->expr;
    while(b->position<b->count) {
        unsigned r=b->position++,index=b->plan.predicate;
        if(b->vectors[index].errors[r]) return -5;
        struct qs_cell keep=qs_vector_cell(&b->plan,b->vectors,index,r);
        if(keep.isnull || !keep.value) continue;
        hash_load(o->right,hash_row(o,q->rows[r],stride),&right->expr);
        return 2;
    }
    return 0;
scalar:
    hash_load(o->right,hash_row(o,first,stride),&right->expr);
    return 3;
}
static int hash_input_boundary(struct execution *s,unsigned id)
{
    int scan=qp_probe_scan(s->plan,id); if(scan<0 || s->source_node>=0 || s->block_input || s->stream_edge) return 0;
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,&s->plan->nodes[scan]));
    if(!chunk->rows || s->nodes[scan].scan_position<chunk->selected) return 0;
    for(int at=id;at>=0;at=s->plan->nodes[at].left) {
        const struct operator_state *o=&s->nodes[at];
        if(o->projection && ((at!=(int)id && o->projection->input_count) ||
            o->projection->position<o->projection->count)) return 0;
        /* An Index cursor owns lookup batches, not the scan's row window.
         * Its continuation must finish before another consumer uses that
         * boundary; do not infer an Index boundary from the distant scan. */
        if(s->plan->nodes[at].kind==QP_INDEX_JOIN) return 0;
        if(o->hash_qual && o->hash_qual->expr.position<o->hash_qual->expr.count) return 0;
        if(o->hash_match || (o->have_left && !o->hash_matched &&
            (s->plan->nodes[at].join_type==QP_LEFT || s->plan->nodes[at].join_type==QP_ANTI))) return 0;
        if(at==scan) return 1;
    }
    return 0;
}

int64_t hash_match_next(struct execution *s,unsigned id,int boundary)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned rn=s->plan->nodes[n->right].expr.nout,ln=s->plan->nodes[n->left].expr.nout;
    const struct qs_plan *right_plan=&s->plan->nodes[n->right].expr;
    unsigned row_bytes=hash_row_bytes(right_plan);
restart:
    if(!o->hash_phase && o->hash_fallback!=1) {
        borrow_hash_build(s,id,o);
#ifdef QP_SPLIT_RUNTIME
        if(!o->hash_phase) return -1;
#else
        if(!o->hash_phase) { int64_t status=hash_build(s,id); if(status) return status; }
#endif
    }
    if(o->finished) { hash_qual_close(s,o); return 0; }
    if(o->hash_fallback==1) return 3;
    o->hash_null_right=0;
    uint32_t *next=hash_chain(o,row_bytes),*buckets=next+o->hash_capacity;
    for(;;) {
        if(!o->have_left) {
            if(boundary && hash_input_boundary(s,id)) return QP_BATCH_END;
            int64_t status=hash_probe_input(s,id,buckets);
            if(status!=2) {
                if(!status && o->external) { external_advance(s,id); goto restart; }
                if(!status) {
                    if(!o->hash_shared) hash_release(s->context,o);
                    o->hash_data=NULL; o->finished=1;
                    hash_qual_close(s,o);
                }
                return status;
            }
            o->have_left=1; o->hash_matched=o->hash_qual_rejected=0;
        }
        while(o->hash_match || (o->hash_qual && o->hash_qual->expr.position<o->hash_qual->expr.count)) {
            if(o->hash_qual && o->hash_qual->expr.position<o->hash_qual->expr.count) {
                int status=hash_qual_next(s,id,UINT32_MAX,next);
                if(status<0) return status;
                if(!status) continue;
                goto matched;
            }
            unsigned at=o->hash_match-1; o->hash_match=next[at]==UINT_MAX?0:next[at]+1;
            hash_load(o->right,hash_row(o,at,row_bytes),right_plan);
            const struct qs_cell *right=o->right;
            int equal=keys_equal(s,n,o->left,right); if(equal<0) return -5;
            if(!equal) continue;
            int null=0; for(unsigned i=0;i<n->nkeys;++i) null|=right[n->right_keys[i]].isnull;
            if(null) continue;
            if(n->join_type==QP_ANTI && n->join_qual<0) {
                o->hash_matched=1; o->hash_match=0; ++s->metrics.join_rows; break;
            }
            struct qs_cell match={.value=1};
            if(n->join_qual>=0) {
                /* EXISTS often accepts its first candidate. Keep that fast
                 * path; vectorize only after a residual condition rejects. */
                int pending=o->join_condition && o->join_condition->active;
                int status=pending || ((n->join_type==QP_SEMI || n->join_type==QP_ANTI) && !o->hash_qual_rejected)?
                    3:hash_qual_next(s,id,at,next);
                if(status<0) return status;
                if(!status) continue;
                if(status==2) goto matched;
                int payload=hash_left_payload(s,id); if(payload) return payload;
                struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values)); memcpy(values+ln,right,rn*sizeof(*values));
                if(join_condition(s,id,values,&match)) {
                    if(s->expression_wait) o->hash_match=at+1;
                    return -5;
                }
            }
            if(match.isnull || !match.value) { o->hash_qual_rejected=1; continue; }
matched:
            o->hash_matched=1; ++s->metrics.join_rows;
            if(o->hash_qual && (n->join_type==QP_ANTI || n->join_type==QP_SEMI))
                o->hash_qual->expr.count=o->hash_qual->expr.position=0;
            if(n->join_type==QP_ANTI) { o->hash_match=0; break; }
            if(n->join_type==QP_SEMI) { o->hash_match=0; o->have_left=0; }
            return 2;
        }
        int unmatched=!o->hash_matched && (n->join_type==QP_LEFT || n->join_type==QP_ANTI);
        o->have_left=0;
        if(unmatched) { o->hash_null_right=1; return 2; }
    }
}

static void reject_integer_hash_collisions(struct execution *s,const struct qp_node *n,
    const struct projection_batch *b,const struct operator_state *o,uint32_t *matches)
{
    const struct qp_node *left=&s->plan->nodes[n->left],*right=&s->plan->nodes[n->right];
    if(n->join_type!=QP_INNER || n->nkeys!=1 ||
       n->estimated_rows>=left->estimated_rows/2) return;
    int index=b->plan.outputs[n->left_keys[0]].index;
    int build=right->expr.outputs[n->right_keys[0]].index;
    if(b->plan.exprs[index].type!=Q_INTEGER || right->expr.exprs[build].type!=Q_INTEGER ||
       b->plan.exprs[index].scale || b->vectors[index].kind==QS_DYNAMIC) return;
    struct hash_key_layout layout;
    hash_key_layout(&layout,n,&right->expr,1);
    unsigned width=layout.widths[0];
    if(width!=4 && width!=8) return;
    unsigned stride=hash_row_bytes(&right->expr);
    const uint32_t *next=hash_chain(o,stride);
    const struct qs_vector *keys=&b->vectors[index];
    bool narrow=keys->kind==QS_REFERENCE || keys->kind==QS_NARROW;
    for(unsigned r=0;r<b->count;++r) {
        unsigned match=matches[r];
        if(!match) continue;
        __int128 key=qs_vector_value(keys,r);
        if(!narrow && qs_vector_cell(&b->plan,b->vectors,index,r).scale) continue;
        while(match) {
            unsigned at=match-1;
            const unsigned char *value=hash_row(o,at,stride)+layout.offsets[0];
            int64_t candidate;
            if(width==4) { int32_t narrow; memcpy(&narrow,value,4); candidate=narrow; }
            else memcpy(&candidate,value,8);
            if(!value[width] && key==candidate) break;
            match=next[at]==UINT_MAX?0:next[at]+1;
        }
        matches[r]=match;
    }
}

static bool hash_narrow_probe_slots(const struct qp_node *n,const struct projection_batch *b,
    unsigned buckets,unsigned evaluated,struct hash_probe_batch *probe)
{
    if(n->nkeys!=1) return false;
    int index=b->plan.outputs[n->left_keys[0]].index;
    const struct qs_vector *keys=&b->vectors[index];
    if(b->plan.exprs[index].type!=Q_INTEGER || b->plan.exprs[index].scale ||
       (keys->kind!=QS_NARROW && keys->kind!=QS_REFERENCE)) return false;
    memset(probe->errors,0,b->count*sizeof(probe->errors[0]));
    for(;evaluated;evaluated&=evaluated-1) {
        unsigned col=__builtin_ctz(evaluated);
        const struct qs_vector *v=&b->vectors[b->plan.outputs[col].index];
        for(unsigned r=0;r<b->count;++r) probe->errors[r]|=v->errors[r];
    }
    for(unsigned r=0;r<b->count;++r) {
        if(probe->errors[r] || (!keys->all_valid && qs_vector_null(keys,r))) {
            probe->matches[r]=UINT_MAX;
            continue;
        }
        int64_t value=keys->kind==QS_REFERENCE?keys->column[qs_vector_row(keys,r)]:keys->narrow[r];
        uint64_t hash=mix_hash(UINT64_C(0x9e3779b97f4a7c15)^(uint64_t)value^-(uint64_t)(value<0));
        probe->matches[r]=hash&(buckets-1);
    }
    return true;
}

static int64_t graph_probe_input(struct execution *s,unsigned id,const uint32_t *buckets)
{
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->nodes[id];
    unsigned at=s->hash_input_position,cols=s->plan->nodes[n->left].expr.nout;
    if(at==s->hash_input->rows) return s->hash_input->finished?0:QP_WAIT;
    if(!o->hash_probe) o->hash_probe=cs_work_calloc(s->context,1,sizeof(*o->hash_probe));
    if(!o->hash_probe) {
        int64_t status=hash_build_input(s,n->left,o->left);
        if(status!=2) return status;
        unsigned match=keys_null(n,o->left)?UINT_MAX:buckets[row_hash(n,o->left,0)&(o->hash_buckets-1)];
        o->hash_match=match==UINT_MAX?0:match+1;
    } else {
        struct hash_probe_batch *probe=o->hash_probe;
        const struct qs_cell *rows=(const struct qs_cell *)s->hash_input->data+(size_t)s->hash_input->begin*cols;
        if(!o->graph_probe_count || at-o->graph_probe_first>=o->graph_probe_count) {
            unsigned count=s->hash_input->rows-at;
            if(count>QS_VECTOR_ROWS) count=QS_VECTOR_ROWS;
            for(unsigned i=0;i<count;++i) {
                const struct qs_cell *row=rows+(size_t)(at+i)*cols;
                probe->matches[i]=keys_null(n,row)?UINT_MAX:row_hash(n,row,0)&(o->hash_buckets-1);
            }
            for(unsigned i=0;i<count;++i) {
                unsigned slot=probe->matches[i];
                unsigned match=slot==UINT_MAX?UINT_MAX:buckets[slot];
                probe->matches[i]=match==UINT_MAX?0:match+1;
            }
            o->graph_probe_first=at; o->graph_probe_count=count;
        }
        o->hash_match=probe->matches[at-o->graph_probe_first];
        memcpy(o->left,rows+(size_t)at*cols,cols*sizeof(*o->left));
        ++s->hash_input_position;
    }
    ++s->metrics.matched;
    return 2;
}

static int64_t hash_probe_input(struct execution *s,unsigned id,const uint32_t *buckets)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    o->hash_payload=0;
    if(s->hash_input && (!o->external || o->external->direct)) return graph_probe_input(s,id,buckets);
    const struct qp_node *left=&s->plan->nodes[n->left];
    /* Aggregate Source columns are separate from its Sink input vectors. */
    int source=left->kind==QP_AGGREGATE;
    bool borrowed=s->stream_edge && s->stream_edge_node==(unsigned)n->left;
    int eligible=(left->kind==QP_SCAN || left->kind==QP_PROJECT || left->kind==QP_JOIN || left->kind==QP_INDEX_JOIN || source) &&
        !o->external && (s->source_node<0 || borrowed) && !(!s->worker && s->plan->workers>1 && left->parallel);
    if(eligible) {
        if(borrowed) {
            if(s->stream_edge->position==s->stream_edge->count) {
                s->scan_drained=true;
                return QP_WAIT;
            }
            if(!o->hash_probe) o->hash_probe=cs_work_calloc(s->context,1,sizeof(*o->hash_probe));
        } else {
            int status=hash_probe_projection(s,id,source); if(status) return status;
        }
        struct projection_batch *b=source?s->nodes[n->left].source_projection:s->nodes[n->left].projection;
        if(b && o->hash_probe) {
            int64_t ready=borrowed?2:projection_fill(s,n->left,source);
            if(ready!=2) {
                if(ready!=3) {
                    if(!ready) { cs_work_free(s->context,o->hash_probe); o->hash_probe=NULL; }
                    return ready;
                }
                cs_work_free(s->context,o->hash_probe); o->hash_probe=NULL;
            } else {
                b=source?s->nodes[n->left].source_projection:s->nodes[n->left].projection;
                struct hash_probe_batch *probe=o->hash_probe;
                if(!b->position || !probe->ready) {
                    for(unsigned k=0;k<n->nkeys;++k) {
                        int index=b->plan.outputs[n->left_keys[k]].index;
                        if(materialize_vector(s,&b->vectors[index],b->count)) return -3;
                    }
                    unsigned evaluated=((1U<<b->plan.nout)-1)&~b->deferred_numbers;
                    if(!hash_narrow_probe_slots(n,b,o->hash_buckets,evaluated,probe)) for(unsigned r=0;r<b->count;++r) {
                        uint64_t hash=UINT64_C(0x9e3779b97f4a7c15); unsigned null=0,error=0;
                        for(unsigned columns=evaluated;columns;columns&=columns-1) {
                            unsigned col=__builtin_ctz(columns);
                            error|=b->vectors[b->plan.outputs[col].index].errors[r];
                        }
                        for(unsigned k=0;!error && k<n->nkeys;++k) {
                            int index=b->plan.outputs[n->left_keys[k]].index;
                            struct qs_cell key=qs_vector_cell(&b->plan,b->vectors,index,r);
                            null|=key.isnull; hash=hash_key(hash,key);
                        }
                        probe->matches[r]=null || error?UINT_MAX:hash&(o->hash_buckets-1);
                        probe->errors[r]=error;
                    }
                    /* Separate hashing from random bucket loads so the CPU
                     * can overlap independent lookups across this vector. */
                    for(unsigned r=0;r<b->count;++r) {
                        unsigned slot=probe->matches[r],at=slot==UINT_MAX?UINT_MAX:buckets[slot];
                        probe->matches[r]=at==UINT_MAX?0:at+1;
                    }
                    reject_integer_hash_collisions(s,n,b,o,probe->matches);
                    probe->ready=1;
                }
                /* An empty bucket cannot emit a row for INNER/SEMI. Skip
                 * consecutive misses in the vector before copying any key
                 * cells. Let the final row cross the existing input boundary
                 * and stop at deferred errors to keep their evaluation order. */
                unsigned r=b->position;
                if(n->join_type==QP_INNER || n->join_type==QP_SEMI) {
                    while(r+1<b->count && !probe->matches[r] && !probe->errors[r]) ++r;
                    s->metrics.matched+=r-b->position;
                }
                b->position=r+1;
                /* Report expression errors only when this input row is
                 * requested: a LIMIT may never consume later error rows. */
                if(probe->errors[r]) return -5;
                o->hash_payload=(1U<<b->plan.nout)-1;
                for(unsigned k=0;k<n->nkeys;++k) {
                    unsigned col=n->left_keys[k]; int index=b->plan.outputs[col].index;
                    o->left[col]=qs_vector_cell(&b->plan,b->vectors,index,r);
                    o->hash_payload&=~(1U<<col);
                }
                o->hash_match=probe->matches[r];
                ++s->metrics.matched; return 2;
            }
        }
    }
    int64_t status=o->external?external_probe(s,id,o->left):next_row(s,n->left,o->left);
#ifdef QP_SPLIT_RUNTIME
    if(!status && o->external && !o->external->direct && s->stream_block) {
        s->scan_drained=true;
        return QP_WAIT; /* This table ended. The Source still owns the join EOF. */
    }
#endif
    if(status==2) {
        unsigned at=keys_null(n,o->left)?UINT_MAX:buckets[row_hash(n,o->left,0)&(o->hash_buckets-1)];
        o->hash_match=at==UINT_MAX?0:at+1;
    }
    return status;
}

static int hash_left_cell(struct execution *s,unsigned id,unsigned col,struct qs_cell *value)
{
    struct operator_state *o=&s->nodes[id];
    if(o->hash_payload&(1U<<col)) {
        unsigned left=s->plan->nodes[id].left;
        struct projection_batch *b=s->plan->nodes[left].kind==QP_AGGREGATE?
            s->nodes[left].source_projection:s->nodes[left].projection;
        unsigned r=b->position-1; int index=b->plan.outputs[col].index;
        const struct qs_vector *v=&b->vectors[index];
        if(b->deferred_numbers&(1U<<col)) {
            struct qs_vector_input input=b->retry_input;
            input.plan=&b->plan; input.vectors=b->vectors; input.count=b->count;
            uint16_t row=r;
            if(qs_vector_evaluate(&input,index,&row,1)) return -5;
        }
        if(v->errors[r]) return -5;
        if(v->kind==QS_TEXT) {
            struct qs_cell cell={.isnull=v->nulls[r]};
            if(!cell.isnull) {
                unsigned key; if(intern_view(s,v->strings[r],&key)) return -3;
                cell.value=key;
            }
            o->left[col]=cell;
        } else o->left[col]=qs_vector_cell(&b->plan,b->vectors,index,r);
        o->hash_payload&=~(1U<<col);
    }
    *value=o->left[col]; return 0;
}

int hash_left_payload(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    for(unsigned columns=o->hash_payload;columns;columns&=columns-1) {
        unsigned col=__builtin_ctz(columns); struct qs_cell value;
        int status=hash_left_cell(s,id,col,&value); if(status) return status;
    }
    return 0;
}

int64_t projection_join_fill(struct execution *s,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[b->scan]; struct operator_state *o=&s->nodes[b->scan];
    unsigned ln=s->plan->nodes[n->left].expr.nout;
    b->stream_input_drained=false;
    if(b->error) { s->control->error=b->error; s->control->error_node=b->error_node; return b->error; }
    while(b->input_count<QS_VECTOR_ROWS) {
        int previous=s->control->error; unsigned previous_node=s->control->error_node;
#ifndef QP_SPLIT_RUNTIME
        int merge=!n->hash || o->hash_fallback==1;
        struct qs_cell values[QS_COLS];
        int64_t status=b->joined_row?2:merge?merge_match_next(s,b->scan,values):hash_match_next(s,b->scan,b->keep_window && b->input_count);
        /* Hash construction can choose the existing Merge fallback before
         * producing rows. Keep this DAG and begin collecting Merge pairs. */
        if(status==3 && !merge && o->hash_fallback==1) continue;
#else
        if(!n->hash || o->hash_fallback==1) return -1;
        int64_t status=b->joined_row?2:hash_match_next(s,b->scan,b->keep_window && b->input_count);
#endif
        if(status==QP_BATCH_END) break;
        if(status!=2) {
            if(s->expression_wait) return status;
            /* The shared page request must be serviced before another
             * operator can issue I/O. Keep this partial column batch across
             * the wait instead of exposing it and requesting the page twice. */
            if(status==QP_WAIT && (s->hash_input || (s->block_graph && s->scan_drained)) && b->input_count) {
                /* The match chain is exhausted as well as the inlet.
                 * E9 can return its credit when this last output drains. */
                b->stream_input_drained=s->stream_block && s->scan_drained;
                break;
            }
            if(status==QP_WAIT || status>2) return status;
            if(status<0) {
                b->error=status; b->error_node=s->control->error?s->control->error_node:b->scan;
                s->control->error=previous; s->control->error_node=previous_node;
                if(!b->input_count) { s->control->error=status; s->control->error_node=b->error_node; return status; }
            }
            if(!b->input_count) return status;
            break;
        }
#ifndef QP_SPLIT_RUNTIME
        if(merge) { projection_append(b,values); continue; }
#endif
        /* A matching pair is already selected. Dictionary waits while
         * materializing its lazy payload must not advance the join again. */
        b->joined_row=1;
        for(unsigned col=b->joined_column;col<b->plan.nattrs;++col) {
            int slot=b->plan.cols[col].slot; if(slot<0) continue;
            struct qs_cell value;
            if(col<ln) {
                int payload=hash_left_cell(s,b->scan,col,&value);
                if(payload) {
                    if(s->expression_wait) { b->joined_column=col; return payload; }
                    b->error=payload; b->error_node=b->scan;
                    if(!b->input_count) return payload;
                    goto evaluate_batch;
                }
            } else value=o->hash_null_right?(struct qs_cell){.isnull=1}:o->right[col-ln];
            b->cells[(size_t)slot*QS_VECTOR_ROWS+b->input_count]=value;
        }
        b->joined_row=b->joined_column=0;
        ++b->input_count;
    }
evaluate_batch:
    b->count=b->input_count; b->input_count=b->position=0;
    struct qs_vector_input input={.cells=b->cells,.dictionary=s->dictionary,
        .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    /* COUNT(*) can have no payload columns; it still consumes cardinality. */
    if(b->plan.nexprs && projection_execute(b,NULL,NULL,0,input)) return -5;
    return 2;
}

/* Scalar adapter for unsupported projections and very small budgets. */
int64_t hash_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned ln=s->plan->nodes[n->left].expr.nout,rn=s->plan->nodes[n->right].expr.nout;
    for(;;) {
        int64_t status=o->hash_output_pending?2:hash_match_next(s,id,0);
#ifdef QP_SPLIT_RUNTIME
        if(status==3) return -1;
#else
        if(status==3) return join_next(s,id,row);
#endif
        if(status!=2) return status;
        o->hash_output_pending=1;
        int payload=hash_left_payload(s,id); if(payload) return payload;
        struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
        for(unsigned col=0;col<rn;++col) values[ln+col]=o->hash_null_right?(struct qs_cell){.isnull=1}:o->right[col];
        int count=project(s,n,values,NULL,row); if(count<0) return count;
        o->hash_output_pending=0; if(count) return 2;
    }
}
static int64_t stream_probe(struct execution *s,unsigned scan,unsigned sink,struct qs_result *out)
{
    (void)out;
    bool chained=s->block_chain || graph_device_source(&s->plan->nodes[scan]);
    int id=chained?graph_stream_node(s):(int)sink;
    if(id<0) return -1;
    if(s->args->graph_input==FEMU_GRAPH_INPUT_EOF) return graph_finish_stream();
    struct operator_state *state=&s->nodes[chained?(unsigned)id:s->root];
    state->read_projection=projection_join_fill;
    state->read_row=hash_next;
    if(chained) {
        struct operator_state *op=&s->nodes[id];
        op->stream_inlet=true;
        int64_t status=graph_stream_project(s,scan,id);
        if(status!=2) return status;
        if(op->projection->stream_input_drained && !op->projection->error) {
            op->stream_inlet=false;
            return graph_borrow_input();
        }
        return graph_publish_output();
    }
    int status=projection_init(s,sink,0); if(status) return status;
    if(!s->nodes[sink].projection) return -4;
    int64_t ready=projection_fill(s,sink,0);
    if(ready==2) {
        const struct projection_batch *b=s->nodes[sink].projection;
        return b->stream_input_drained && !b->error?graph_borrow_input():graph_publish_output();
    }
    if((ready==QP_WAIT || !ready) && s->scan_drained)
        return graph_consume_input();
    return ready==3 || !ready?-4:ready;
}

int64_t pg_sina_graph_block_probe(struct femu_csd_args *a)
{ return graph_block_call(a,BLOCK_PROBE,stream_probe); }


static int64_t partition_probe(struct execution *s,struct femu_csd_args *a,unsigned worker,unsigned id)
{
    if(!worker) return -1;
    if(a->graph_input==FEMU_GRAPH_INPUT_EOF) return graph_finish_stream();
    struct operator_state *o=&s->nodes[id];
    if(!o->external || !o->hash_phase) return -1;
    s->scan_drained=false;
    o->read_projection=projection_join_fill;
    o->read_row=hash_next;
    int64_t status=projection_init(s,id,0); if(status) return status;
    if(!o->projection) return -4;
    status=projection_fill(s,id,0);
    if(status==2) {
        o->published_batch=&o->projection;
        return o->projection->stream_input_drained && !o->projection->error?
            graph_borrow_input():graph_publish_output();
    }
    if(status==QP_WAIT && s->scan_drained) return graph_consume_input();
    wait_io(s,status);
    return !status || status==3?-4:status;
}

int64_t pg_sina_graph_partition_probe(struct femu_csd_args *a)
{ return graph_tail_call(a,partition_probe,false); }

#ifndef QP_SPLIT_RUNTIME
int64_t pg_sina_graph_hash_probe(struct femu_csd_args *a)
{ return graph_emit(a,GRAPH_EMIT_PROBE,NULL); }
#endif
