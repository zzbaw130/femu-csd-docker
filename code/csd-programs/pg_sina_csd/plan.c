/* Execute a PG physical tree. PG submits page LOADs and SNIA handles spill;
 * tuple bytes and intermediate rows never cross back to the PG executor. */
#include "femu-csd-kernel.h"
#include "cs_csf.h"
#include "cs_workmem.h"
#include "plan.h"
#include "query_vector.h"
#include "index_scan.h"
#include "scan.h"
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>
#include <endian.h>

extern int64_t pg_sina_scan_impl(struct femu_csd_args *);
extern int64_t pg_sina_filter_impl(struct femu_csd_args *);
extern int64_t pg_sina_sort_sink_impl(struct femu_csd_args *);
extern int64_t pg_sina_sort_source_impl(struct femu_csd_args *);
extern void qs_sort_dictionary(uint64_t,struct qs_dict *);
extern int qs_sort_paged_dictionary(uint64_t,struct qs_dictionary *);
extern const char *qs_sort_output_text(const struct qs_result *,uint32_t,unsigned *);
extern int qs_sort_prepare(uint64_t);
extern int64_t qs_sort_finish_sink(uint64_t,struct femu_csd_args *);
extern int64_t qs_sort_sink_columns(struct femu_csd_args *,const struct qs_plan *,const struct qs_vector *,unsigned,unsigned);
#define QP_WAIT 1
/* Store PG integer/date widths directly; numeric and other values retain all
 * 128 bits. Keep scale/dscale/NULL, and avoid unaligned integer accesses. */
static unsigned hash_value_bytes(const struct qs_plan *p,unsigned column)
{
    switch(p->outputs[column].type_oid) {
    case 16:case 21:case 23:case 1082:return 4;
    case 20:return 8;
    default:return 16;
    }
}
static unsigned hash_row_bytes(const struct qs_plan *p)
{
    unsigned bytes=0; for(unsigned i=0;i<p->nout;++i) {
        unsigned value=hash_value_bytes(p,i); bytes+=value+(value<16?1:3);
    }
    return bytes;
}
static void hash_store_value(unsigned char *to,const struct qs_cell *value,unsigned bytes)
{
    memcpy(to,&value->value,bytes);
    if(bytes<16) to[bytes]=value->isnull;
    else { to[bytes]=value->scale; to[bytes+1]=value->dscale; to[bytes+2]=value->isnull; }
}
static void hash_store(unsigned char *to,const struct qs_cell *from,const struct qs_plan *p)
{
    for(unsigned i=0;i<p->nout;++i) {
        unsigned bytes=hash_value_bytes(p,i);
        hash_store_value(to,&from[i],bytes); to+=bytes+(bytes<16?1:3);
    }
}
static void hash_load(struct qs_cell *to,const unsigned char *from,const struct qs_plan *p)
{
    for(unsigned i=0;i<p->nout;++i) {
        unsigned bytes=hash_value_bytes(p,i);
        if(bytes==4) { int32_t v; memcpy(&v,from,4); to[i].value=v; }
        else if(bytes==8) { int64_t v; memcpy(&v,from,8); to[i].value=v; }
        else memcpy(&to[i].value,from,16);
        if(bytes<16) { to[i].scale=to[i].dscale=0; to[i].isnull=from[bytes]; from+=bytes+1; }
        else { to[i].scale=from[bytes]; to[i].dscale=from[bytes+1]; to[i].isnull=from[bytes+2]; from+=bytes+3; }
    }
}
struct projection_batch;
struct sorted_rows {
    struct qs_plan plan;
    struct qs_task task;
    struct qs_group group;
    struct qs_result *input,*output;
    struct qs_result *batch; /* Borrowed CU output, retained across spill. */
    unsigned batch_worker;
    struct projection_batch *columns; /* Borrowed producer vectors, retained across spill. */
    unsigned position;
    int initialized, complete, drained, input_pending, producer_status,finishing_sink,sink_finished;
    unsigned char *prefix;
    unsigned prefix_rows,prefix_position;
    struct qs_metrics metrics;
};
struct spool {
    unsigned char *tail,*cache;
    uint64_t *tokens;
    unsigned blocks, capacity, used, loaded;
    uint64_t rows;
};
#define HASH_PARTITIONS 4
/* Bounded radix partitions use the existing SNIA spill protocol. A skewed
 * partition is probed in build batches, without sorting or rereading Heap. */
struct external_hash {
    struct spool build[HASH_PARTITIONS],probe[HASH_PARTITIONS];
    unsigned char *prefix;
    uint64_t prefix_rows,prefix_at,build_at,probe_at;
    unsigned phase,partition,direct;
};
struct pending_projection {
    struct qs_cell input[QS_COLS],aggregates[QS_AGGS];
    int has_input,has_aggregates,filtered,inline_only;
    struct qs_eval evaluation; /* Completed subexpressions survive dictionary I/O. */
};
struct join_condition_state {
    struct qs_eval evaluation;
    struct qs_cell input[QS_COLS];
    int active;
};
struct accumulator { struct qs_cell sum,extreme,last; uint64_t count; int has_last; };
/* Scalar aggregate arguments must finish before any accumulator changes. */
struct aggregate_input {
    struct qs_eval evaluation;
    struct qs_cell values[QS_AGGS];
    unsigned argument,aggregate;
    int active,ready;
};
/* sum holds the total for SUM/AVG, or the selected value for MIN/MAX. */
struct group_value { struct qs_cell sum; uint64_t count; };
struct compact_group { int64_t key; unsigned key_flags; struct group_value values[]; };
/* Hash slots publish indices into dense records, not whole aggregate states. */
struct group_entry { atomic_uint state; unsigned row; };
/* One unfinished hash update per CU. Never retain a bucket lock across I/O.
 * The input and old/new values stay here until the complete update commits. */
struct group_update_state {
    struct operator_state *owner;
    struct group_entry *entry;
    struct compact_group *group;
    struct group_value before[QS_AGGS],after[QS_AGGS],input[QS_AGGS];
    uint64_t hash;
    unsigned node,aggregate,occupied;
    int active,shared;
};
struct group_batch_input {
    struct group_value values[QS_AGGS];
    struct qs_cell keys[QS_KEYS];
    unsigned end,aggregate;
    int phase;
};
struct set_key { int64_t value; unsigned occupied; };
/* A spilled set is immutable after initialization. Each consumer owns its
 * read cache and the active expression batch, while tokens/tail are shared. */
struct set_probe {
    struct set_probe *next;
    const struct qs_expr *expression;
    struct spool pages;
    struct qs_cell keys[QS_VECTOR_ROWS],values[QS_VECTOR_ROWS];
    unsigned rows[QS_VECTOR_ROWS],count,block;
    int ready;
};
/* Private per-CU expression state; referenced input columns stay in AFDM. */
struct projection_batch {
    struct qs_plan plan;
    struct qs_vector *vectors;
    uint8_t needed[QS_EXPRS];
    unsigned scan,count,position,input_count;
    unsigned aggregate_position,aggregate_row;
    struct group_batch_input *group_input;
    struct qs_cell *cells;
    const struct qs_cell *borrowed_cells;
    const uint32_t *borrowed_selection;
    int64_t error;
    unsigned error_node;
    int joined,sorted,group_source,keep_window,lazy_text,filtered_scan;
    int retry,materializing;
    unsigned joined_row,joined_column;
    unsigned materialized_column;
    struct qs_vector_input retry_input;
    struct qs_chunk *retry_chunk;
    const uint32_t *retry_selection;
    unsigned retry_first,retry_count;
    int group_keys[QS_KEYS],group_values[QS_AGGS];
    struct { unsigned node; int expression; } filters[QP_NODES];
    unsigned nfilters;
};
static int projection_pending(const struct projection_batch *b)
{ return b && (b->position<b->count || b->input_count || b->error); }
struct hash_probe_batch {
    uint32_t matches[QS_VECTOR_ROWS];
    uint8_t errors[QS_VECTOR_ROWS];
    int ready;
};
struct hash_qual_batch {
    struct projection_batch expr;
    uint32_t rows[QS_VECTOR_ROWS];
    uint32_t cursor;
};
struct merge_qual_batch {
    struct projection_batch expr;
    uint64_t begin;
};
struct index_qual_batch {
    struct projection_batch expr;
    int status[QS_VECTOR_ROWS];
    unsigned preparing,prepared;
    uint32_t selection[QS_VECTOR_ROWS];
};
struct operator_state {
    struct sorted_rows left_sort,right_sort;
    struct qs_cell left[QS_OUT],right[QS_OUT],first[QS_OUT],keys[QS_KEYS];
    struct accumulator accum[QS_AGGS];
    struct aggregate_input *aggregate_input;
    unsigned aggregate_position;
    struct spool group;
    uint64_t position, skipped, emitted;
    unsigned scan_position, scan_input, scan_bytes,scan_raw,scan_raw_bytes;
    struct qs_scan_cursor *scan_cursor;
    int defer_strings;
    struct qs_plan *scan_filter;
    struct qs_dict *filter_dictionary; /* Immutable literals for raw-Heap filtering. */
    int scan_filter_checked,projection_checked;
    struct projection_batch *projection;
    struct projection_batch *source_projection;
    int source_checked;
    unsigned projection_deferred,source_deferred;
    int phase, have_left, have_right, input_done, group_ready, matched, finished;
    struct join_condition_state *join_condition;
    unsigned merge_key;
    unsigned merge_ordered; /* left/right input order verified; bit 2 marks checked. */
    struct merge_qual_batch *merge_qual;
    int merge_qual_checked;
    struct qs_plan *index_plan;
    struct projection_batch *index_projection;
    struct index_qual_batch *index_qual;
    int index_qual_checked;
    struct qs_join_control index_control;
    struct qs_task index_task;
    struct qs_result *index_output;
    struct qs_cell *index_left;
    uint64_t index_matches[(QS_INDEX_KEYS+63)/64];
    unsigned index_count, index_unmatched;
    unsigned index_position;
    int index_resume, index_flush,index_active,index_projected;
    unsigned index_consume_position;
    unsigned char *hash_data;
    atomic_uint hash_rows;
    unsigned hash_capacity,hash_match,hash_phase,hash_buckets;
    int hash_matched,hash_fallback,hash_shared,hash_null_right,hash_output_pending;
    unsigned hash_payload; /* Columns still borrowed from the probe vector. */
    struct hash_probe_batch *hash_probe;
    struct hash_qual_batch *hash_qual;
    int hash_qual_checked,hash_qual_rejected;
    struct external_hash *external;
    struct compact_group *groups;
    struct group_entry *group_entries;
    unsigned group_capacity,group_flush,group_phase,group_stride,group_integer;
    atomic_uint *group_count;
    struct operator_state *group_local,**group_locals;
    unsigned group_local_count,group_local_slot,group_local_row;
    int group_partial;
    unsigned group_batch_kept;
    struct set_key *set;
    unsigned set_capacity,set_count;
    int set_null,set_ready;
    struct spool set_pages;
    unsigned set_convert;
    int set_external;
    struct set_probe *set_probe;
    struct pending_projection *project_retry;
};
struct output_string { uint32_t id; uint16_t row,column; };
struct execution {
    cs_work_context *context;
    struct qp_plan *plan;
    struct qp_control *control;
    struct qs_dict *dictionary;
    struct qs_dictionary *paged;
    struct qs_dictionary_access *initial_access;
    unsigned *literal_ids;
    unsigned imported,literal_node,literal_string;
    int dictionary_ready,operators_closed,closing,buffered;
    unsigned buffered_rows,buffered_finished;
    int64_t initial_wait,dictionary_wait;
    struct qs_dictionary_text *text;
    struct group_update_state *group_update;
    int64_t text_wait;
    unsigned output_column;
    int output_pending;
    unsigned host_building,host_pending,host_column,host_var_bytes;
    unsigned host_source_rows,host_source_worker;
    const struct qs_cell *host_source;
    unsigned char *host_columns[QS_OUT],*host_nulls[QS_OUT];
    unsigned host_widths[QS_OUT];
    int raw_projection;
    struct qs_cell host_row[QS_OUT];
    uint64_t host_values[QS_OUT];
    unsigned char host_var[QS_OUT*264];
    struct output_string *output_order;
    unsigned output_count,output_position,output_exported;
    int output_active,output_status;
    struct operator_state *nodes;
    /* One scalar projection can wait per CU. Reserve its continuation before
     * the dictionary cache fills the query budget, rather than during a miss. */
    struct pending_projection projection_retry;
    struct qs_cell params[QP_PARAMS];
    unsigned char *arena;
    void *io;
    size_t io_bytes, sort_limit;
    struct sorted_rows *pending_sort;
    struct sorted_rows *sort_sink; /* CU-local state; runs are owned by the query. */
    struct spool *pending_spool;
    unsigned pending_block;
    int pending_store;
    int expression_wait;
    unsigned pending_nvm;
    struct qs_metrics metrics;
    struct qs_profile profile;
    uint64_t local_profile_cpu, pipeline_start, pipeline_tick;
    uint64_t sink_ns,sink_rows;
    bool profiling, host_scan, scan_drained;
    struct femu_csd_args *args;
    unsigned initializer,initializer_phase;
    unsigned worker, parallel_position[QS_WORKERS];
    uint64_t parallel_done;
    int parallel_started,parallel_combining;
    unsigned parallel_combine_worker;
    unsigned parallel_node, root, used_workers;
    /* One-based parent receiving rows directly from parallel workers. */
    unsigned hash_sink,group_sink,aggregate_sink,group_combine;
    bool group_shard,group_merge_full;
    atomic_uint_fast64_t next_offset;
    atomic_uint active_cus, peak_cus;
    struct execution *owner, *workers[QS_WORKERS];
    struct qs_result *outputs;
    int partition_join,source_node;
    struct spool source;
    uint64_t source_at,source_end;
};
static uint64_t now_ns(clockid_t clock)
{ struct timespec t; clock_gettime(clock,&t); return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }
static size_t input_offset(const struct execution *s,const struct qp_node *n)
{ return s->worker>1?n->parallel_input+(s->worker-2)*s->plan->parallel_stride:n->input_offset; }
static size_t vector_offset(const struct execution *s,const struct qp_node *n)
{ return s->worker>1?n->parallel_vector+(s->worker-2)*s->plan->parallel_stride:n->vector_offset; }
static int64_t parallel_next(struct execution *,unsigned,struct qs_cell *);
static int projection_init(struct execution *,unsigned,int);
static int64_t projection_next(struct execution *,unsigned,struct qs_cell *,int);
static int64_t projection_fill(struct execution *,unsigned,int);
static int64_t sort_sink_columns(struct execution *,struct sorted_rows *,unsigned);
static int64_t projection_group_fill(struct execution *,struct projection_batch *);
static int aggregate_sink_vectors(struct execution *,unsigned);
static int64_t timed_call(struct execution *s,int64_t (*call)(struct femu_csd_args *),struct femu_csd_args *a,unsigned op)
{
    if(!s->profiling) return call(a);
    uint64_t wall=now_ns(CLOCK_MONOTONIC),cpu=now_ns(CLOCK_THREAD_CPUTIME_ID);
    int64_t status=call(a); struct qs_timing *t=&s->profile.operators[op];
    uint64_t elapsed=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
    ++t->calls; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-wall; t->cpu_ns+=elapsed;
    s->local_profile_cpu+=elapsed;
    return status;
}
/* Blocking-call boundaries, never per input tuple. Child Scan/Sort and nested
 * blocking CPU is subtracted; merged worker counters are not thread-local CPU. */
struct blocking_timer { uint64_t wall,cpu,children; };
static struct blocking_timer blocking_begin(struct execution *s)
{
    return (struct blocking_timer){now_ns(CLOCK_MONOTONIC),now_ns(CLOCK_THREAD_CPUTIME_ID),s->local_profile_cpu};
}
static void blocking_end(struct execution *s,unsigned op,struct blocking_timer begin)
{
    uint64_t elapsed=now_ns(CLOCK_THREAD_CPUTIME_ID)-begin.cpu,children=s->local_profile_cpu-begin.children;
    uint64_t own=elapsed>children?elapsed-children:0;
    struct qs_timing *t=&s->profile.operators[op]; ++t->calls;
    t->cpu_ns+=own; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-begin.wall;
    s->local_profile_cpu+=own;
}
static int64_t next_row_impl(struct execution *,unsigned,struct qs_cell *);
static int retry_project(struct execution *,unsigned,struct qs_cell *);
static inline int64_t next_row(struct execution *s,unsigned id,struct qs_cell *row)
{
    int64_t status=3;
    if(row && id<s->plan->count && s->nodes[id].project_retry && !s->nodes[id].project_retry->inline_only) status=retry_project(s,id,row);
    if(status==3) status=next_row_impl(s,id,row);
    if(status<0 && !s->expression_wait && !s->control->error) { s->control->error=status; s->control->error_node=id; }
    return status;
}
static int64_t join_next(struct execution *,unsigned,struct qs_cell *);
static int64_t merge_qual_next(struct execution *,unsigned,struct qs_cell *);
static void merge_qual_close(struct execution *,struct operator_state *);
static int64_t hash_probe_input(struct execution *,unsigned,const uint32_t *);
static int hash_left_payload(struct execution *,unsigned);
static int hash_left_cell(struct execution *,unsigned,unsigned,struct qs_cell *);
static int64_t external_build(struct execution *,unsigned);
static int64_t external_probe(struct execution *,unsigned,struct qs_cell *);
static void external_advance(struct execution *,unsigned);
static int key_compare(struct execution *,const struct qp_node *,const struct qs_cell *,const struct qs_cell *);
static int keys_equal(struct execution *,const struct qp_node *,const struct qs_cell *,const struct qs_cell *);
static int keys_null(const struct qp_node *,const struct qs_cell *);
static uint64_t mix_hash(uint64_t);
static int64_t build_set(struct execution *,unsigned);
static _Thread_local struct execution *index_execution;
static _Thread_local unsigned index_node;
static _Thread_local struct projection_batch *index_columns;
static int64_t index_next(struct execution *,unsigned,struct qs_cell *,struct projection_batch *);
#define QP_INDEX_YIELD 4
static void init_eval(struct qs_eval *e,const struct qs_plan *plan,const struct qs_cell *row,const struct qs_cell *aggregates)
{
    e->plan=plan; e->tuple=row; e->aggregates=aggregates;
    memset(e->valid,0,plan->nexprs);
}
static int text_get(struct execution *,unsigned,const char **,unsigned *);
static int text_release(struct execution *);
static int compare_cell(struct execution *s,struct qs_cell a,struct qs_cell b,unsigned type)
{
    if(a.isnull || b.isnull) return (a.isnull!=0)-(b.isnull!=0);
    if(type==Q_STRING) {
        const char *x,*y; unsigned xn,yn;
        if(text_get(s,(unsigned)a.value,&x,&xn) || text_get(s,(unsigned)b.value,&y,&yn)) return INT_MIN;
        int cmp=memcmp(x,y,xn<yn?xn:yn); if(!cmp) cmp=(xn>yn)-(xn<yn);
        if(text_release(s)) return INT_MIN;
        return cmp;
    }
    int cmp=0; return qs_compare(a,b,&cmp)?INT_MIN:cmp;
}
static int equal_cell(struct execution *s,struct qs_cell a,struct qs_cell b,unsigned type)
{
    if(a.isnull || b.isnull) return !!a.isnull==!!b.isnull;
    if(type==Q_STRING) return a.value==b.value;
    int cmp=compare_cell(s,a,b,type); return cmp==INT_MIN?-1:!cmp;
}
static inline struct qs_cell set_contains(const struct operator_state *set,struct qs_cell a)
{
    struct qs_cell v={0};
    if(a.isnull) v.isnull=set->set_count || set->set_null;
    else {
        if(a.value<=INT64_MAX && a.value>=INT64_MIN && set->set_capacity) {
            unsigned slot=mix_hash((uint64_t)a.value)&(set->set_capacity-1);
            while(set->set[slot].occupied) {
                if(set->set[slot].value==a.value) { v.value=1; break; }
                slot=(slot+1)&(set->set_capacity-1);
            }
        }
        if(!v.value) v.isnull=set->set_null;
    }
    return v;
}
static int64_t spool_row_bytes(struct execution *,struct spool *,uint64_t,unsigned,void *);
static int64_t external_set_probe(struct execution *s,unsigned id,const struct qs_expr *expression,
    const struct qs_cell *keys,const unsigned *rows,unsigned count,struct qs_cell **values)
{
    struct operator_state *o=&s->nodes[id];
    struct set_probe *p=o->set_probe;
    while(p && p->expression!=expression) p=p->next;
    if(!p) {
        p=cs_work_calloc(s->context,1,sizeof(*p)); if(!p) return -4;
        p->pages=o->set_pages; p->pages.cache=NULL; p->pages.loaded=UINT_MAX;
        p->expression=expression; p->next=o->set_probe; o->set_probe=p;
    }
    int same=p->count==count;
    for(unsigned i=0;same && i<count;++i)
        same=p->rows[i]==rows[i] && p->keys[i].value==keys[i].value && p->keys[i].isnull==keys[i].isnull;
    if(!same) {
        p->count=count; p->block=0; p->ready=0;
        for(unsigned i=0;i<count;++i) {
            p->keys[i]=keys[i]; p->rows[i]=rows[i];
            p->values[i]=(struct qs_cell){.isnull=keys[i].isnull?(o->set_count || o->set_null):o->set_null};
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
static int inset_vectors(struct qs_vector_input *input,unsigned index,const uint16_t *rows,unsigned count)
{
    if(!count) return 0;
    if(count>QS_VECTOR_ROWS) return -1;
    struct execution *s=input->set_state; const struct qs_expr *e=&input->plan->exprs[index];
    if(e->column<0 || (unsigned)e->column>=s->plan->count || !s->nodes[e->column].set_ready) return -1;
    const struct operator_state *set=&s->nodes[e->column];
    if(set->set_external) {
        struct qs_cell keys[QS_VECTOR_ROWS],*values; unsigned selection[QS_VECTOR_ROWS];
        for(unsigned j=0;j<count;++j) { selection[j]=rows[j]; keys[j]=input->vectors[e->left].errors[rows[j]]?(struct qs_cell){.isnull=1}:qs_vector_cell(input->plan,input->vectors,e->left,rows[j]); }
        int64_t status=external_set_probe(s,e->column,e,keys,selection,count,&values);
        if(status) { s->expression_wait=status>2; return -1; }
        struct qs_vector *out=&input->vectors[index]; out->kind=QS_NARROW;
        for(unsigned j=0;j<count;++j) { unsigned i=rows[j]; out->narrow[i]=values[j].value; out->nulls[i]=values[j].isnull;
            out->errors[i]=input->vectors[e->left].errors[i]; out->dscales[i]=0; out->all_valid&=!out->errors[i] && !values[j].isnull; }
        return 0;
    }
    const struct qs_vector *left=&input->vectors[e->left]; struct qs_vector *out=&input->vectors[index];
    out->kind=QS_NARROW;
    for(unsigned j=0;j<count;++j) {
        unsigned i=rows[j]; struct qs_cell v={0}; unsigned error=left->errors[i];
        if(!error) v=set_contains(set,qs_vector_cell(input->plan,input->vectors,e->left,i));
        out->narrow[i]=v.value; out->nulls[i]=v.isnull; out->errors[i]=error; out->dscales[i]=0;
        out->all_valid&=!error && !v.isnull;
    }
    return 0;
}
/* Both scalar expressions and final output release borrowed bytes after
 * completing their operation. A wait retains the same operands and memo. */
static int text_status(struct execution *s,int64_t status)
{
    if(status>0) { s->text_wait=status; s->expression_wait=1; }
    return status?-3:0;
}
static const char *sort_text_get(struct execution *s,unsigned id,unsigned *length)
{
    for(unsigned i=0;s->plan && i<s->plan->count;++i) {
        const struct sorted_rows *sorts[]={&s->nodes[i].left_sort,&s->nodes[i].right_sort};
        for(unsigned j=0;j<2;++j) if(sorts[j]->output && sorts[j]->output->rows) {
            const char *bytes=qs_sort_output_text(sorts[j]->output,id,length);
            if(bytes) return bytes;
        }
    }
    return NULL;
}
static int text_get(struct execution *s,unsigned id,const char **bytes,unsigned *length)
{
    *bytes=sort_text_get(s,id,length); if(*bytes) return 0;
    if(!s->text) return qs_dict_get(s->dictionary,id,bytes,length);
    return text_status(s,qs_dictionary_text_get(s->text,id,bytes,length,s->io));
}
static int text_insert(struct execution *s,const char *bytes,unsigned length,unsigned *id)
{
    if(!s->text) {
        int value=qs_dict_insert(s->dictionary,bytes,length); if(value<0) return -3;
        *id=value; return 0;
    }
    return text_status(s,qs_dictionary_text_insert(s->text,bytes,length,id,s->io));
}
static int text_release(struct execution *s)
{ return s->text?qs_dictionary_text_release(s->text):0; }
static int evaluate(struct execution *s,struct qs_eval *e,int index,struct qs_cell *out)
{
    if(index<0 || (unsigned)index>=e->plan->nexprs) return -1;
    if(e->valid[index]) { *out=e->cells[index]; return 0; }
    const struct qs_expr *x=&e->plan->exprs[index]; struct qs_cell a={0},b={0},v={0};
    if(x->op==QP_PARAMETER) v=s->params[x->column];
    else if(x->op==QP_INSET) {
        if(x->column<0 || (unsigned)x->column>=s->plan->count || !s->nodes[x->column].set_ready ||
            evaluate(s,e,x->left,&a)) return -5;
        if(s->nodes[x->column].set_external) {
            struct qs_cell *values; unsigned row=0;
            int64_t status=external_set_probe(s,x->column,x,&a,&row,1,&values);
            if(status) { s->expression_wait=status>2; return -5; }
            v=values[0];
        } else v=set_contains(&s->nodes[x->column],a);
    }
    else if(x->op==Q_CONST && x->type==Q_STRING && !x->isnull) {
        const struct qs_string *literal=&e->plan->strings[x->constant];
        uintptr_t at=(uintptr_t)e->plan,first=(uintptr_t)&s->plan->nodes[0].expr;
        unsigned node=at>=first?(at-first)/sizeof(struct qp_node):s->plan->count;
        unsigned id=s->literal_ids && node<s->plan->count && e->plan==&s->plan->nodes[node].expr?
            s->literal_ids[node*QS_STRINGS+(unsigned)x->constant]:0;
        if(!id && (text_insert(s,literal->bytes,literal->length,&id) || text_release(s))) return -3;
        v.value=id;
    } else if(x->op==Q_COLUMN || x->op==Q_CONST || x->op==Q_AGGREF) return qs_evaluate(e,index,out);
    else {
        if(evaluate(s,e,x->left,&a)) return -5;
        if(x->op==Q_CASE) {
            if(evaluate(s,e,!a.isnull && a.value?x->right:x->otherwise,&v)) return -5;
            /* Preserve the actual numeric scale of an intermediate value. */
        } else if(x->op==QP_COALESCE) {
            v=a; if(a.isnull && evaluate(s,e,x->right,&v)) return -5;
        } else if(x->op==Q_AND && !a.isnull && !a.value) v=a;
        else if(x->op==Q_OR && !a.isnull && a.value) v=a;
        else if(x->op==Q_NOT || x->op==Q_IS_NULL || x->op==Q_IS_NOT_NULL || x->op==Q_YEAR)
            return qs_evaluate(e,index,out);
        else {
            if(evaluate(s,e,x->right,&b)) return -5;
            v.isnull=a.isnull || b.isnull;
            if(x->op==QP_LIKE || x->op==QP_SUBSTRING ||
               (x->op>=Q_EQ && x->op<=Q_GE && e->plan->exprs[x->left].type==Q_STRING)) {
                if(!v.isnull) {
                    const char *left,*right; unsigned ln,rn;
                    if(x->op==QP_SUBSTRING) {
                        struct qs_cell length; if(evaluate(s,e,x->otherwise,&length)) return -5;
                        if(length.isnull) v.isnull=1;
                        else {
                            if(length.value<0 || b.value<INT_MIN || b.value>INT_MAX || length.value>INT_MAX) return -5;
                            if(text_get(s,(unsigned)a.value,&left,&ln)) return -3;
                            int64_t begin=b.value-1,end=begin+length.value;
                            if(begin<0) begin=0;
                            if(end<begin) end=begin;
                            unsigned first=0,last=0; int64_t character=0;
                            while(first<ln && character<begin) { first+=qs_text_character(left+first,ln-first); ++character; }
                            last=first;
                            while(last<ln && character<end) { last+=qs_text_character(left+last,ln-last); ++character; }
                            unsigned id; if(text_insert(s,left+first,last-first,&id)) return -3;
                            v.value=id;
                        }
                    } else {
                        if(text_get(s,(unsigned)a.value,&left,&ln) ||
                           text_get(s,(unsigned)b.value,&right,&rn)) return -3;
                        if(x->op==QP_LIKE) { int match=qs_text_like(right,rn,left,ln); if(match<0) return -5; v.value=match; }
                        else {
                            int cmp=memcmp(left,right,ln<rn?ln:rn); if(!cmp) cmp=(ln>rn)-(ln<rn);
                            v.value=x->op==Q_EQ?cmp==0:x->op==Q_NE?cmp!=0:x->op==Q_LT?cmp<0:x->op==Q_LE?cmp<=0:x->op==Q_GT?cmp>0:cmp>=0;
                        }
                    }
                    if(text_release(s)) return -3;
                }
            } else return qs_evaluate(e,index,out);
        }
    }
    e->valid[index]=1; e->cells[index]=v; *out=v; return 0;
}
/* Materialization retains its own position after expression evaluation has
 * completed. Do not rebuild vectors or advance a producer on an I/O wait. */
static int materialize_vector(struct execution *s,struct qs_vector *v,unsigned count)
{
    if(!s->paged) return qs_vector_materialize_text(v,count,s->dictionary);
    return text_status(s,qs_vector_materialize_text_paged(v,count,s->paged,s->text,s->io,0));
}
static int materialize_outputs(struct execution *s,struct projection_batch *b)
{
    b->materializing=1;
    for(;b->materialized_column<b->plan.nout;++b->materialized_column) {
        int index=b->plan.outputs[b->materialized_column].index;
        if(index>=0 && materialize_vector(s,&b->vectors[index],b->count)) return -3;
    }
    b->materializing=0; b->materialized_column=0; return 0;
}
static int intern_view(struct execution *s,struct qs_text_view view,unsigned *id)
{
    if(!s->paged) {
        int value=qs_text_intern(s->dictionary,view); if(value<0) return -3;
        *id=value; return 0;
    }
    if(view.id && !view.offset && view.length==UINT_MAX) {
        if(!qs_dictionary_valid_id(s->paged,view.id)) return -3;
        *id=view.id; return 0;
    }
    const char *bytes=view.data; unsigned length=view.length;
    if(view.id) {
        if(text_get(s,view.id,&bytes,&length)) return -3;
        if(view.offset>length || (view.length!=UINT_MAX && view.length>length-view.offset)) return -3;
        bytes+=view.offset; length=view.length==UINT_MAX?length-view.offset:view.length;
    }
    if(text_insert(s,bytes,length,id)) return -3;
    return text_release(s);
}
static int join_condition(struct execution *s,unsigned id,const struct qs_cell *values,struct qs_cell *match)
{
    const struct qp_node *n=&s->plan->nodes[id];
    if(!s->text) {
        struct qs_eval e; init_eval(&e,&n->expr,values,NULL);
        return evaluate(s,&e,n->join_qual,match);
    }
    struct operator_state *o=&s->nodes[id];
    if(!o->join_condition) o->join_condition=cs_work_calloc(s->context,1,sizeof(*o->join_condition));
    struct join_condition_state *q=o->join_condition; if(!q) return -4;
    if(!q->active) {
        memcpy(q->input,values,n->expr.nattrs*sizeof(*values));
        init_eval(&q->evaluation,&n->expr,q->input,NULL); q->active=1;
    }
    int status=evaluate(s,&q->evaluation,n->join_qual,match);
    if(!status) q->active=0;
    return status;
}
static int project_filtered_impl(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row,int filtered,struct qs_eval *e)
{
    struct qs_cell keep;
    if(!filtered && n->expr.predicate>=0) {
        if(!e->plan) init_eval(e,&n->expr,input,aggregates);
        if(evaluate(s,e,n->expr.predicate,&keep)) return -5;
        if(keep.isnull || !keep.value) return 0;
    }
    for(unsigned i=0;i<n->expr.nout;++i) {
        int index=n->expr.outputs[i].index; const struct qs_expr *x=&n->expr.exprs[index];
        if(input && x->op==Q_COLUMN) { row[i]=input[x->column]; continue; }
        if(!e->plan) init_eval(e,&n->expr,input,aggregates);
        if(evaluate(s,e,index,&row[i])) return -5;
    }
    return 1;
}
/* A scalar caller can have advanced its source before projection. Retain
 * that row only on an I/O wait and deliver its result before fetching again. */
static int project_filtered(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row,int filtered)
{
    struct operator_state *o=&s->nodes[n-s->plan->nodes];
    struct pending_projection *p=o->project_retry;
    if(p) { input=p->has_input?p->input:NULL; aggregates=p->has_aggregates?p->aggregates:NULL; filtered=p->filtered; }
    struct qs_eval e; if(p) e=p->evaluation; else e.plan=NULL;
    int count=project_filtered_impl(s,n,input,aggregates,row,filtered,&e);
    if(count<0 && s->expression_wait) {
        if(!p) {
            p=&s->projection_retry; memset(p,0,sizeof(*p));
            p->has_input=input!=NULL; p->has_aggregates=aggregates!=NULL; p->filtered=filtered;
            for(unsigned i=0;i<s->plan->count;++i) if(s->nodes[i].index_active &&
                (&s->plan->nodes[i]==n || s->plan->nodes[i].right==n-s->plan->nodes)) p->inline_only=1;
            if(input) memcpy(p->input,input,n->expr.nattrs*sizeof(*input));
            if(aggregates) memcpy(p->aggregates,aggregates,n->expr.naggs*sizeof(*aggregates));
            o->project_retry=p;
        }
        p->evaluation=e; p->evaluation.tuple=p->has_input?p->input:NULL;
        p->evaluation.aggregates=p->has_aggregates?p->aggregates:NULL;
    } else if(p) o->project_retry=NULL;
    return count;
}
static int retry_project(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id];
    int count=project_filtered(s,n,NULL,NULL,row,0);
    if(count<0) return count;
    if(!count) return 3;
    if(n->kind==QP_SCAN) ++s->metrics.matched;
    else if(n->kind==QP_SORT || n->kind==QP_LIMIT || n->kind==QP_CTE_SCAN || (n->kind==QP_PROJECT && n->left>=0))
        ++s->nodes[id].emitted;
    return 2;
}
static int project(struct execution *s,const struct qp_node *n,const struct qs_cell *input,
    const struct qs_cell *aggregates,struct qs_cell *row)
{ return project_filtered(s,n,input,aggregates,row,0); }
static int64_t sort_columns_call(struct femu_csd_args *a)
{
    const struct projection_batch *b=a->mr_addr[0];
    return qs_sort_sink_columns(a,&b->plan,b->vectors,b->position,b->count);
}
static int64_t sort_finish_call(struct femu_csd_args *a)
{ return qs_sort_finish_sink(((struct qs_task *)a->mr_addr[2])->state,a); }
static int64_t sort_call(struct execution *s,struct sorted_rows *sort,int source)
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
static int64_t sort_init(struct execution *s,struct sorted_rows *sort,const struct qs_plan *input,
    unsigned keys,const int *columns,const struct qs_order *order)
{
    if(sort->initialized) return 0;
    sort->input=cs_work_calloc(s->context,1,sizeof(*sort->input));
    sort->output=cs_work_calloc(s->context,1,sizeof(*sort->output));
    if(!sort->input || !sort->output) return -4;
    sort->plan.nout=input->nout; sort->plan.ordered_rows=2; sort->plan.limit=-1;
    sort->plan.work_mem_bytes=s->sort_limit;
    /* The plan depth supplies a conservative fallback reserve, not a fixed
     * partition of all memory. Borrow from actual free query memory when
     * fewer sorts are live, retaining workspace for consumers and merge. */
    struct execution *owner=s->owner?s->owner:s;
    struct cs_work_context_stats memory;
    if(cs_work_get_context_stats(owner->context,&memory)) return -4;
    unsigned active=0;
    for(unsigned i=0;i<owner->plan->count;++i) {
        const struct operator_state *o=&owner->nodes[i];
        active+=o->left_sort.initialized && !o->left_sort.drained;
        active+=o->right_sort.initialized && !o->right_sort.drained;
    }
    size_t available=(memory.limit_bytes-memory.used_bytes)/(active+2);
    size_t free_bytes=memory.limit_bytes-memory.used_bytes,reserve=2*s->sort_limit;
    if(free_bytes>reserve && free_bytes-reserve>available) available=free_bytes-reserve;
    if(available>sort->plan.work_mem_bytes) sort->plan.work_mem_bytes=available;
    sort->plan.work_mem_parent=(uintptr_t)s->context;
    memcpy(sort->plan.outputs,input->outputs,input->nout*sizeof(input->outputs[0]));
    for(unsigned i=0;i<keys;++i) sort->plan.order[sort->plan.norder++]=order?order[i]:(struct qs_order){.output=columns[i]};
    sort->task.workers=1;
    int64_t status=sort_call(s,sort,0); if(status) return status;
    sort->initialized=1;
    if(s->paged) { int bound=qs_sort_paged_dictionary(sort->task.state,s->paged); if(bound) return bound; }
    else qs_sort_dictionary(sort->task.state,s->dictionary);
    return 0;
}
static int64_t sorted_input(struct execution *s,struct sorted_rows *sort,unsigned child,
    unsigned keys,const int *columns,const struct qs_order *order,struct qs_cell *row)
{
    const struct qp_node *input=&s->plan->nodes[child];
    int64_t status=sort_init(s,sort,&input->expr,keys,columns,order); if(status) return status;
    if(sort->drained && !sort->output) return 0;
    while(!sort->complete) {
        if(sort->input_pending) {
            status=sort_call(s,sort,0); if(status) return status;
            sort->input_pending=0; sort->input->rows=0;
        }
        if(sort->batch) {
            status=sort_call(s,sort,0); if(status) return status;
            s->parallel_position[sort->batch_worker]=sort->batch->rows;
            sort->batch=NULL; continue;
        }
        if(sort->prefix && sort->prefix_position==sort->prefix_rows) {
            cs_work_free(s->context,sort->prefix); sort->prefix=NULL;
        }
        if(!s->worker && s->plan->workers>1 && !sort->input->rows && sort->prefix_position==sort->prefix_rows &&
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
                        sort->batch->begin=s->parallel_position[w]; break;
                    }
                if(!sort->batch) return -1;
                continue;
            }
            /* 3 means this producer selected its ordinary scalar fallback. */
        }
        if(!sort->input->rows && sort->prefix_position==sort->prefix_rows &&
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
static uint64_t mix_hash(uint64_t x)
{ x^=x>>33; x*=UINT64_C(0xff51afd7ed558ccd); x^=x>>33; x*=UINT64_C(0xc4ceb9fe1a85ec53); return x^(x>>33); }
static uint64_t hash_key(uint64_t hash,struct qs_cell v)
{
    while(v.scale && !(v.value%10)) { v.value/=10; --v.scale; }
    return mix_hash(hash^(uint64_t)v.value^(uint64_t)((unsigned __int128)v.value>>64)^((uint64_t)v.scale<<56));
}
static uint64_t row_hash(const struct qp_node *n,const struct qs_cell *row,int right)
{
    uint64_t hash=0x9e3779b97f4a7c15ULL;
    for(unsigned i=0;i<n->nkeys;++i) hash=hash_key(hash,row[right?n->right_keys[i]:n->left_keys[i]]);
    return hash;
}
/* Row storage and bucket links have one writer per appended row. Bucket heads
 * are published atomically while the build CU tasks run; probing starts only
 * after all build tasks have completed. Reallocation happens at a wave barrier. */
static void hash_link(const struct qp_node *n,struct operator_state *o,
    const struct qs_plan *right,const struct qs_cell *row,unsigned at)
{
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*hash_row_bytes(right));
    uint32_t *buckets=next+o->hash_capacity;
    unsigned bucket=row_hash(n,row,1)&(o->hash_buckets-1);
    next[at]=__atomic_exchange_n(&buckets[bucket],at,__ATOMIC_RELAXED);
}
/* Keep allocator timing at table boundaries, never per build tuple. */
static void *hash_memory(struct execution *s,unsigned id,void *old,size_t bytes)
{
    uint64_t started=s->profiling && !s->worker?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
    uintptr_t previous=(uintptr_t)old;
    void *data=old?cs_work_realloc(s->context,old,bytes):cs_work_alloc(s->context,bytes);
    if(started) {
        struct qp_pipeline_profile *p=&s->plan->pipelines[id];
        p->hash_memory_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-started;
        if(previous && data && (uintptr_t)data!=previous) {
            const struct qp_node *n=&s->plan->nodes[id];
            p->hash_moved_bytes+=(size_t)s->nodes[id].hash_capacity*
                (hash_row_bytes(&s->plan->nodes[n->right].expr)+3*sizeof(uint32_t));
        }
    }
    return data;
}
static void hash_reindex(struct execution *s,unsigned id)
{
    uint64_t started=s->profiling && !s->worker?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    unsigned rb=hash_row_bytes(right);
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*rb);
    o->hash_buckets=2*o->hash_capacity;
    memset(next+o->hash_capacity,0xff,(size_t)o->hash_buckets*sizeof(uint32_t));
    unsigned offsets[QS_KEYS],widths[QS_KEYS];
    for(unsigned k=0;k<n->nkeys;++k) {
        unsigned col=n->right_keys[k],offset=0;
        for(unsigned c=0;c<col;++c) {
            unsigned bytes=hash_value_bytes(right,c); offset+=bytes+(bytes<16?1:3);
        }
        offsets[k]=offset; widths[k]=hash_value_bytes(right,col);
    }
    uint32_t *buckets=next+o->hash_capacity;
    /* Initial setup and growth run before CUs start or after their wave
     * barrier. Only this owner writes buckets here; concurrent Sink inserts
     * retain hash_link()'s atomic publication. Decode keys, not payloads. */
    for(unsigned at=0;at<o->hash_rows;++at) {
        const unsigned char *row=o->hash_data+(size_t)at*rb;
        uint64_t hash=0x9e3779b97f4a7c15ULL;
        for(unsigned k=0;k<n->nkeys;++k) {
            const unsigned char *from=row+offsets[k]; unsigned bytes=widths[k];
            struct qs_cell value={0};
            if(bytes==4) { int32_t v; memcpy(&v,from,4); value.value=v; }
            else if(bytes==8) { int64_t v; memcpy(&v,from,8); value.value=v; }
            else { memcpy(&value.value,from,16); value.scale=from[16]; }
            value.isnull=from[bytes+(bytes==16?2:0)];
            hash=hash_key(hash,value);
        }
        unsigned bucket=hash&(o->hash_buckets-1);
        next[at]=buckets[bucket]; buckets[bucket]=at;
    }
    if(started) {
        struct qp_pipeline_profile *p=&s->plan->pipelines[id];
        p->hash_reindex_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-started;
        ++p->hash_reindexes; p->hash_reindexed_rows+=o->hash_rows;
    }
}
/* Reserve a disjoint append range; leave one slot for the owner's next_row(). */
static unsigned hash_reserve_rows(struct operator_state *o,unsigned rows,unsigned *at)
{
    *at=atomic_load_explicit(&o->hash_rows,memory_order_relaxed);
    unsigned count;
    do {
        count=o->hash_capacity-1-*at;
        if(count>rows) count=rows;
        if(!count) return 0;
    } while(!atomic_compare_exchange_weak_explicit(&o->hash_rows,at,*at+count,
        memory_order_relaxed,memory_order_relaxed));
    return count;
}
static void hash_sink_batch(struct execution *s,struct qs_result *out)
{
    unsigned id=s->hash_sink-1;
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->owner->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    if(!o->hash_data || o->hash_fallback) return;
    unsigned at,count=hash_reserve_rows(o,out->rows,&at);
    if(!count) return;
    unsigned rb=hash_row_bytes(right);
    for(unsigned i=0;i<count;++i) {
        const struct qs_cell *row=(struct qs_cell *)out->data+(size_t)i*right->nout;
        hash_store(o->hash_data+(size_t)(at+i)*rb,row,right);
        hash_link(n,o,right,row,at+i);
    }
    s->metrics.build_rows+=count; out->rows-=count;
    memmove(out->data,(struct qs_cell *)out->data+(size_t)count*right->nout,
        (size_t)out->rows*right->nout*sizeof(struct qs_cell));
}
static int64_t hash_build_impl(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    unsigned row_bytes=hash_row_bytes(right);
    if(o->external) return external_build(s,id);
    if(o->hash_fallback) return 0;
    size_t stride=row_bytes+3*sizeof(uint32_t);
    /* Hash build is not a sort run: allow it to use the query budget while
     * retaining two sort quotas for a Merge fallback. The parent allocator
     * still enforces actual usage across all active nodes and workers. */
    struct cs_work_context_stats memory;
    if(cs_work_get_context_stats(s->context,&memory)) return -4;
    size_t hash_limit=memory.limit_bytes-2*s->sort_limit;
    if(!o->hash_capacity) {
        uint64_t estimate=s->plan->nodes[n->right].estimated_rows;
        /* Blocking children need their working space before producing rows. */
        if(s->plan->nodes[n->right].kind!=QP_SCAN) estimate=128;
        unsigned max=1; while((size_t)max*2*stride<=hash_limit && max<UINT_MAX/4) max*=2;
        unsigned capacity=128; while(capacity<estimate && capacity<max) capacity*=2;
        if(capacity>max) capacity=max;
        o->hash_data=hash_memory(s,id,NULL,capacity*stride);
        if(!o->hash_data) { o->hash_fallback=1; return 0; }
        o->hash_capacity=capacity; hash_reindex(s,id);
    }
    while(!o->hash_phase) {
        if(o->hash_rows==o->hash_capacity) {
            size_t bytes=(size_t)o->hash_capacity*2*stride;
            void *data=bytes<=hash_limit?hash_memory(s,id,o->hash_data,bytes):NULL;
            if(!data) {
                /* Preserve already-built projected rows. The fallback sorts
                 * this prefix plus the remaining child stream, never rereads
                 * or decodes the original heap for another build batch. */
                size_t used=(size_t)o->hash_rows*row_bytes;
                void *prefix=cs_work_realloc(s->context,o->hash_data,used);
                unsigned char *retained=prefix?prefix:o->hash_data;
                if(n->join_type==QP_INNER)
                    o->external=cs_work_calloc(s->context,1,sizeof(*o->external));
                if(o->external) {
                    o->external->prefix=retained; o->external->prefix_rows=o->hash_rows;
                    o->hash_data=NULL; o->hash_capacity=o->hash_rows=0; o->hash_fallback=2;
                    return external_build(s,id);
                }
                o->right_sort.prefix=retained; o->right_sort.prefix_rows=o->hash_rows;
                o->hash_data=NULL; o->hash_fallback=1; return 0;
            }
            o->hash_data=data; o->hash_capacity*=2; hash_reindex(s,id);
        }
        int64_t status=next_row(s,n->right,o->right);
        if(status==QP_WAIT || status<0 || status>2) return status;
        if(status==2) {
            unsigned at=o->hash_rows++;
            hash_store(o->hash_data+(size_t)at*row_bytes,o->right,right);
            hash_link(n,o,right,o->right,at); ++s->metrics.build_rows; continue;
        }
        o->hash_phase=1;
    }
    return 0;
}
static int64_t hash_build(struct execution *s,unsigned id)
{
    if(!s->profiling || s->nodes[id].hash_phase) return hash_build_impl(s,id);
    struct blocking_timer timer=blocking_begin(s);
    int64_t status=hash_build_impl(s,id); blocking_end(s,QS_JOIN_BUILD,timer); return status;
}
/* An Index Join above this producer can reuse the retained Heap window.
 * Flush before advancing that window, but finish any live vector/match chain. */
static int hash_input_boundary(struct execution *s,unsigned id)
{
    int scan=qp_probe_scan(s->plan,id); if(scan<0 || s->source_node>=0) return 0;
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
#define QP_BATCH_END 4
static int hash_qual_next(struct execution *,unsigned,uint32_t,const uint32_t *);
static void hash_qual_close(struct execution *s,struct operator_state *o)
{
    if(!o->hash_qual) return;
    cs_work_free(s->context,o->hash_qual->expr.cells);
    cs_work_free(s->context,o->hash_qual->expr.vectors);
    cs_work_free(s->context,o->hash_qual); o->hash_qual=NULL;
}
static int64_t hash_match_next(struct execution *s,unsigned id,int boundary)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned rn=s->plan->nodes[n->right].expr.nout,ln=s->plan->nodes[n->left].expr.nout;
    const struct qs_plan *right_plan=&s->plan->nodes[n->right].expr;
    unsigned row_bytes=hash_row_bytes(right_plan);
restart:
    if(!o->hash_phase && o->hash_fallback!=1) { int64_t status=hash_build(s,id); if(status) return status; }
    if(o->finished) { hash_qual_close(s,o); return 0; }
    if(o->hash_fallback==1) return 3;
    o->hash_null_right=0;
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*row_bytes),*buckets=next+o->hash_capacity;
    for(;;) {
        if(!o->have_left) {
            if(boundary && hash_input_boundary(s,id)) return QP_BATCH_END;
            int64_t status=hash_probe_input(s,id,buckets);
            if(status!=2) {
                if(!status && o->external) { external_advance(s,id); goto restart; }
                if(!status) {
                    if(!o->hash_shared) cs_work_free(s->context,o->hash_data);
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
            hash_load(o->right,o->hash_data+(size_t)at*row_bytes,right_plan);
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
/* Scalar adapter for unsupported projections and very small budgets. */
static int64_t hash_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned ln=s->plan->nodes[n->left].expr.nout,rn=s->plan->nodes[n->right].expr.nout;
    for(;;) {
        int64_t status=o->hash_output_pending?2:hash_match_next(s,id,0);
        if(status==3) return join_next(s,id,row);
        if(status!=2) return status;
        o->hash_output_pending=1;
        int payload=hash_left_payload(s,id); if(payload) return payload;
        struct qs_cell values[QS_COLS]; memcpy(values,o->left,ln*sizeof(*values));
        for(unsigned col=0;col<rn;++col) values[ln+col]=o->hash_null_right?(struct qs_cell){.isnull=1}:o->right[col];
        int count=project(s,n,values,NULL,row); if(count<0) return count;
        o->hash_output_pending=0; if(count) return 2;
    }
}
static int64_t spool_append_bytes(struct execution *s,struct spool *p,const void *row,unsigned row_bytes)
{
    unsigned block_rows=QS_SORT_IO_BYTES/row_bytes;
    if(!p->tail) {
        p->tail=cs_work_alloc(s->context,QS_SORT_IO_BYTES); p->loaded=UINT_MAX;
        if(!p->tail) return -4;
    }
    if(p->used==block_rows) {
        if(p->blocks==p->capacity) {
            unsigned capacity=p->capacity?p->capacity*2:8;
            void *tokens=cs_work_realloc(s->context,p->tokens,capacity*sizeof(*p->tokens)); if(!tokens) return -4;
            p->tokens=tokens; p->capacity=capacity;
        }
        memcpy(s->io,p->tail,QS_SORT_IO_BYTES); s->pending_spool=p; s->pending_store=1;
        return csNeedStore(QS_SORT_IO_BYTES);
    }
    memcpy(p->tail+(size_t)p->used*row_bytes,row,row_bytes); ++p->used; ++p->rows; return 0;
}
static int64_t spool_row_bytes(struct execution *s,struct spool *p,uint64_t index,unsigned row_bytes,void *row)
{
    unsigned block_rows=QS_SORT_IO_BYTES/row_bytes,block=index/block_rows;
    if(index>=p->rows) return -1;
    unsigned char *data=p->tail;
    if(block<p->blocks) {
        if(!p->cache) { p->cache=cs_work_alloc(s->context,QS_SORT_IO_BYTES); if(!p->cache) return -4; }
        if(p->loaded!=block) {
            s->pending_spool=p; s->pending_store=0; s->pending_block=block;
            return csNeedLoad(p->tokens[block]);
        }
        data=p->cache;
    }
    memcpy(row,data+(index%block_rows)*row_bytes,row_bytes); return 0;
}
static int64_t spool_append(struct execution *s,struct spool *p,const struct qs_cell *row,unsigned cols)
{ return spool_append_bytes(s,p,row,cols*sizeof(*row)); }
static int64_t spool_row(struct execution *s,struct spool *p,uint64_t index,unsigned cols,struct qs_cell *row)
{ return spool_row_bytes(s,p,index,cols*sizeof(*row),row); }
static void external_advance(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    if(o->hash_data) cs_work_free(s->context,o->hash_data);
    o->hash_data=NULL; o->hash_rows=o->hash_capacity=o->hash_phase=o->hash_match=0;
    o->have_left=0; o->external->probe_at=0; o->external->phase=o->external->direct?4:2;
    if(o->external->direct) o->finished=1;
}
static int64_t external_probe(struct execution *s,unsigned id,struct qs_cell *row)
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
static int64_t external_build(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    struct external_hash *e=o->external;
    if(e->phase>=3) return 0;
    const struct qs_plan *right=&s->plan->nodes[n->right].expr,*left=&s->plan->nodes[n->left].expr;
    unsigned rb=hash_row_bytes(right),lb=hash_row_bytes(left);
    unsigned char packed[QS_OUT*19];
    while(e->phase<2) {
        int build=e->phase==0;
        if(!o->have_right) {
            int64_t status;
            if(build && e->prefix_at<e->prefix_rows) {
                hash_load(o->right,e->prefix+(size_t)e->prefix_at++*rb,right); status=2;
            } else {
                if(e->prefix) { cs_work_free(s->context,e->prefix); e->prefix=NULL; }
                status=next_row(s,build?n->right:n->left,o->right);
            }
            if(status==QP_WAIT || status<0 || status>2) return status;
            if(!status) {
                ++e->phase;
                if(build) {
                    /* The blocking child has now released its table. If all
                     * partitions fit, load them and Probe the original
                     * input in parallel without partitioning it first. Only
                     * the bucket count needs a power-of-two capacity. */
                    uint64_t rows=0;
                    for(unsigned i=0;i<HASH_PARTITIONS;++i) rows+=e->build[i].rows;
                    if(!rows) { e->phase=4; o->finished=1; return 0; }
                    if(rows<=UINT_MAX/4) {
                        unsigned capacity=(rows+127)&~127U,buckets=256;
                        while(buckets<capacity*2) buckets*=2;
                        size_t bytes=(size_t)capacity*(rb+4)+(size_t)buckets*4;
                        struct cs_work_context_stats m;
                        if(cs_work_get_context_stats(s->context,&m)) return -4;
                        size_t reserve=2*QS_SORT_IO_BYTES;
                        if(s->plan->workers>1) reserve+=s->plan->workers*(sizeof(*s)+s->plan->count*sizeof(*s->nodes)+QS_SORT_IO_BYTES+256);
                        size_t available=m.limit_bytes-m.used_bytes;
                        if(available>reserve && bytes<=available-reserve &&
                           (o->hash_data=cs_work_alloc(s->context,bytes))) {
                            o->hash_capacity=capacity; o->hash_buckets=buckets; e->direct=1; e->phase=2;
                        }
                    }
                }
                continue;
            }
            o->have_right=1;
        }
        /* INNER join NULL keys cannot match, on either side. */
        int null=0;
        for(unsigned k=0;k<n->nkeys;++k) null|=o->right[build?n->right_keys[k]:n->left_keys[k]].isnull;
        if(!null) {
            unsigned part=(row_hash(n,o->right,build)>>32)&(HASH_PARTITIONS-1);
            hash_store(packed,o->right,build?right:left);
            int64_t status=spool_append_bytes(s,build?&e->build[part]:&e->probe[part],packed,build?rb:lb);
            if(status) return status;
        }
        o->have_right=0;
    }
    while(e->phase==2) {
        if(e->partition==HASH_PARTITIONS) {
            if(e->direct) break;
            e->phase=4; o->finished=1; return 0;
        }
        struct spool *build=&e->build[e->partition],*probe=&e->probe[e->partition];
        if(e->build_at==build->rows || (!e->direct && !probe->rows)) {
            if(build->cache) { cs_work_free(s->context,build->cache); build->cache=NULL; }
            if(build->tail) { cs_work_free(s->context,build->tail); build->tail=NULL; }
            if(probe->cache) { cs_work_free(s->context,probe->cache); probe->cache=NULL; }
            if(probe->tail) { cs_work_free(s->context,probe->tail); probe->tail=NULL; }
            ++e->partition; e->build_at=e->probe_at=0; continue;
        }
        if(!o->hash_data) {
            struct cs_work_context_stats m;
            if(cs_work_get_context_stats(s->context,&m)) return -4;
            size_t reserve=2*QS_SORT_IO_BYTES;
            if(s->plan->workers>1) reserve+=s->plan->workers*(sizeof(*s)+s->plan->count*sizeof(*s->nodes)+QS_SORT_IO_BYTES+256);
            size_t available=m.limit_bytes-m.used_bytes;
            available=available>reserve?available-reserve:available/2;
            unsigned capacity=128;
            while(capacity<build->rows-e->build_at && (size_t)capacity*2*(rb+12)<=available && capacity<UINT_MAX/4) capacity*=2;
            while(!(o->hash_data=cs_work_alloc(s->context,(size_t)capacity*(rb+12)))) {
                if(capacity==128) return -4;
                capacity/=2;
            }
            o->hash_capacity=capacity;
        }
        while(o->hash_rows<o->hash_capacity && e->build_at<build->rows) {
            int64_t status=spool_row_bytes(s,build,e->build_at,rb,o->hash_data+(size_t)o->hash_rows*rb);
            if(status) return status;
            ++e->build_at; ++o->hash_rows;
        }
        if(!e->direct) { o->hash_buckets=2*o->hash_capacity; break; }
    }
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*rb),*buckets=next+o->hash_capacity;
    memset(buckets,0xff,(size_t)o->hash_buckets*sizeof(*buckets));
    for(unsigned i=0;i<o->hash_rows;++i) {
        hash_load(o->right,o->hash_data+(size_t)i*rb,right);
        unsigned bucket=row_hash(n,o->right,1)&(o->hash_buckets-1);
        next[i]=buckets[bucket]; buckets[bucket]=i;
    }
    e->phase=3; o->hash_phase=1;
    return 0;
}
static int64_t materialized_row(struct execution *s,unsigned id,uint64_t position,struct qs_cell *row)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned cols=s->plan->nodes[n->left].expr.nout;
    while(!o->group_ready) {
        if(!o->have_left) {
            int64_t status=next_row(s,n->left,o->left);
            if(status==QP_WAIT || status<0 || status>2) return status;
            if(!status) { o->group_ready=1; break; }
            o->have_left=1;
        }
        int64_t status=spool_append(s,&o->group,o->left,cols); if(status) return status;
        o->have_left=0;
    }
    if(position==o->group.rows) return 0;
    int64_t status=spool_row(s,&o->group,position,cols,row); return status?status:2;
}
static int set_order(const void *a,const void *b)
{ int64_t x=*(const int64_t *)a,y=*(const int64_t *)b; return (x>y)-(x<y); }
static int64_t set_append(struct execution *s,struct operator_state *o,int64_t value)
{
    if(o->set_pages.used==QS_SORT_IO_BYTES/sizeof(value))
        qsort(o->set_pages.tail,o->set_pages.used,sizeof(value),set_order);
    return spool_append_bytes(s,&o->set_pages,&value,sizeof(value));
}
static int64_t build_set(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id]; const struct qp_node *n=&s->plan->nodes[id];
    while(!o->set_ready) {
        if(o->set_external && o->set) {
            while(o->set_convert<o->set_capacity) {
                struct set_key *key=&o->set[o->set_convert];
                if(key->occupied) { int64_t status=set_append(s,o,key->value); if(status) return status; }
                ++o->set_convert;
            }
            cs_work_free(s->context,o->set); o->set=NULL; o->set_capacity=0;
        }
        if(!o->have_left) {
            int64_t status=next_row(s,n->left,o->left);
            if(status==QP_WAIT || status<0 || status>2) return status;
            if(!status) {
                if(o->set_external && o->set_pages.used) qsort(o->set_pages.tail,o->set_pages.used,sizeof(int64_t),set_order);
                o->set_ready=1; return 0;
            }
            o->have_left=1;
        }
        struct qs_cell v=o->left[0];
        if(v.isnull) { o->set_null=1; o->have_left=0; continue; }
        if(v.value>INT64_MAX || v.value<INT64_MIN) return -5;
        if(o->set_external) {
            int64_t status=set_append(s,o,(int64_t)v.value); if(status) return status;
            ++o->set_count; o->have_left=0; continue;
        }
        if(!o->set_capacity || o->set_count*2>=o->set_capacity) {
            unsigned capacity=o->set_capacity?o->set_capacity*2:64;
            struct set_key *next=cs_work_calloc(s->context,capacity,sizeof(*next));
            if(!next) { o->set_external=1; continue; }
            for(unsigned i=0;i<o->set_capacity;++i) if(o->set[i].occupied) {
                unsigned slot=mix_hash((uint64_t)o->set[i].value)&(capacity-1);
                while(next[slot].occupied) slot=(slot+1)&(capacity-1);
                next[slot]=o->set[i];
            }
            if(o->set) cs_work_free(s->context,o->set);
            o->set=next; o->set_capacity=capacity;
        }
        unsigned slot=mix_hash((uint64_t)v.value)&(o->set_capacity-1);
        while(o->set[slot].occupied && o->set[slot].value!=v.value) slot=(slot+1)&(o->set_capacity-1);
        if(!o->set[slot].occupied) { o->set[slot]=(struct set_key){.value=v.value,.occupied=1}; ++o->set_count; }
        o->have_left=0;
    }
    return 0;
}
static int key_compare(struct execution *s,const struct qp_node *n,const struct qs_cell *left,const struct qs_cell *right)
{
    const struct qs_plan *types=&s->plan->nodes[n->left].expr;
    struct operator_state *o=&s->nodes[n-s->plan->nodes];
    for(unsigned i=o->merge_key;i<n->nkeys;++i) {
        unsigned col=n->left_keys[i];
        unsigned type=types->exprs[types->outputs[col].index].type;
        struct qs_cell a=left[col],b=right[n->right_keys[i]];
        int cmp=compare_cell(s,a,b,type);
        if(cmp==INT_MIN) { o->merge_key=i; return cmp; }
        if(n->expr.norder) {
            if(a.isnull || b.isnull) { if(n->expr.order[i].nulls_first) cmp=-cmp; }
            else if(n->expr.order[i].descending) cmp=-cmp;
        }
        if(cmp) { o->merge_key=0; return cmp; }
    }
    o->merge_key=0; return 0;
}
/* Hash matching only needs equality; canonical text IDs avoid reading
 * dictionary bodies for candidates that already passed the hash lookup. */
static int keys_equal(struct execution *s,const struct qp_node *n,const struct qs_cell *left,const struct qs_cell *right)
{
    const struct qs_plan *types=&s->plan->nodes[n->left].expr;
    for(unsigned i=0;i<n->nkeys;++i) {
        unsigned col=n->left_keys[i];
        int equal=equal_cell(s,left[col],right[n->right_keys[i]],types->exprs[types->outputs[col].index].type);
        if(equal<=0) return equal;
    }
    return 1;
}
static int keys_null(const struct qp_node *n,const struct qs_cell *left)
{ for(unsigned i=0;i<n->nkeys;++i) if(left[n->left_keys[i]].isnull) return 1; return 0; }
/* Prove a Sort/Merge key prefix through key-preserving projections/filters.
 * A NULL order accepts either direction, as required by streaming grouping. */
static int input_ordered(const struct qp_plan *p,unsigned child,unsigned count,
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
/* Keep duplicate-group and spill cursors here; projection consumes matching
 * pairs separately, either as a column batch or through the scalar adapter. */
static int64_t merge_match_next(struct execution *s,unsigned id,struct qs_cell *values)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned ln=s->plan->nodes[n->left].expr.nout,rn=s->plan->nodes[n->right].expr.nout;
    struct qs_order left_order[QS_KEYS],right_order[QS_KEYS];
    for(unsigned i=0;i<n->nkeys;++i) {
        left_order[i]=n->expr.norder?n->expr.order[i]:(struct qs_order){.output=n->left_keys[i]};
        right_order[i]=left_order[i]; right_order[i].output=n->right_keys[i];
    }
    if(!o->merge_ordered) {
        o->merge_ordered=4;
        if(!o->left_sort.prefix && input_ordered(s->plan,n->left,n->nkeys,n->left_keys,left_order)) o->merge_ordered|=1;
        if(!o->right_sort.prefix && input_ordered(s->plan,n->right,n->nkeys,n->right_keys,right_order)) o->merge_ordered|=2;
    }
    for(;;) {
        int64_t status;
        if(o->phase==2) {
            status=o->merge_ordered&1?next_row(s,n->left,o->left):sorted_input(s,&o->left_sort,n->left,n->nkeys,n->left_keys,left_order,o->left);
            if(status!=2) { if(!status) merge_qual_close(s,o); return status; }
            o->have_left=1; o->position=0; o->matched=0; o->phase=0;
            const struct qs_plan *types=&s->plan->nodes[n->left].expr;
            int same=1;
            for(unsigned i=0;i<n->nkeys;++i) {
                unsigned col=n->left_keys[i];
                int equal=equal_cell(s,o->left[col],o->first[col],types->exprs[types->outputs[col].index].type);
                if(equal<0) return -5;
                if(!equal) same=0;
            }
            if(!same) { o->group_ready=0; o->group.rows=o->group.blocks=o->group.used=0; o->group.loaded=UINT_MAX; }
        }
        if(!o->have_left) {
            status=o->merge_ordered&1?next_row(s,n->left,o->left):sorted_input(s,&o->left_sort,n->left,n->nkeys,n->left_keys,left_order,o->left);
            if(status!=2) { if(!status) merge_qual_close(s,o); return status; }
            o->have_left=1; o->position=0; o->matched=0;
        }
        if(!o->group_ready) {
            if(!o->have_right && !o->input_done) {
                status=o->merge_ordered&2?next_row(s,n->right,o->right):
                    sorted_input(s,&o->right_sort,n->right,n->nkeys,n->right_keys,right_order,o->right);
                if(status==QP_WAIT || status<0 || status>2) return status;
                o->have_right=status==2; o->input_done=!status;
            }
            if(o->have_right && !keys_null(n,o->left)) {
                int cmp=key_compare(s,n,o->left,o->right); if(cmp==INT_MIN) return -5;
                if(cmp>0) { o->have_right=0; continue; }
                if(!cmp) {
                    status=spool_append(s,&o->group,o->right,rn); if(status) return status;
                    o->have_right=0; o->phase=1; continue;
                }
            }
            if(o->phase==1) { o->group_ready=1; o->phase=0; }
            else {
                o->have_left=0;
                if(n->join_type==QP_LEFT || n->join_type==QP_ANTI) {
                    memcpy(values,o->left,ln*sizeof(*values));
                    for(unsigned i=0;i<rn;++i) values[ln+i]=(struct qs_cell){.isnull=1};
                    return 2;
                }
                continue;
            }
        }
        if(o->position<o->group.rows || (o->merge_qual && o->merge_qual->expr.position<o->merge_qual->expr.count)) {
            int qualified=0;
            if(n->join_qual>=0 && !(o->join_condition && o->join_condition->active)) {
                status=merge_qual_next(s,id,values);
                if(!status) continue;
                if(status!=3) { if(status!=2) return status; qualified=1; }
            }
            if(!qualified) {
                if(o->join_condition && o->join_condition->active)
                    memcpy(values,o->join_condition->input,n->expr.nattrs*sizeof(*values));
                else {
                    memcpy(values,o->left,ln*sizeof(*values));
                    status=spool_row(s,&o->group,o->position,rn,values+ln); if(status) return status;
                }
                struct qs_cell match={.value=1};
                if(n->join_qual>=0 && join_condition(s,id,values,&match)) return -5;
                ++o->position;
                if(match.isnull || !match.value) continue;
            }
            o->matched=1;
            if((n->join_type==QP_SEMI || n->join_type==QP_ANTI) && o->merge_qual)
                o->merge_qual->expr.count=o->merge_qual->expr.position=0;
            if(n->join_type==QP_ANTI) { o->position=o->group.rows; continue; }
            if(n->join_type==QP_SEMI) o->position=o->group.rows;
            ++s->metrics.join_rows; return 2;
        }
        if(!o->matched && (n->join_type==QP_LEFT || n->join_type==QP_ANTI)) {
            memcpy(values,o->left,ln*sizeof(*values));
            for(unsigned i=0;i<rn;++i) values[ln+i]=(struct qs_cell){.isnull=1};
            o->matched=1;
            return 2;
        }
        /* Keep the right duplicate group for another left row with this key. */
        memcpy(o->first,o->left,ln*sizeof(*o->first)); o->have_left=0; o->phase=2;
    }
}
static int64_t join_next(struct execution *s,unsigned id,struct qs_cell *row)
{
    struct qs_cell values[QS_COLS];
    for(;;) {
        int64_t status=merge_match_next(s,id,values); if(status!=2) return status;
        int count=project(s,&s->plan->nodes[id],values,NULL,row);
        if(count<0) return count;
        if(count) return 2;
    }
}
/* Filter retained Heap values by selection vectors. Byte views avoid interning
 * rejected strings; InitPlan parameters retain their full numeric precision. */
static int scan_filter_init(struct execution *s,const struct qs_plan *p,struct operator_state *o)
{
    int raw=s->raw_projection && p==&s->plan->nodes[s->plan->root].expr;
    if(!o->scan_filter_checked && raw && p->predicate<0) {
        o->scan_filter=cs_work_alloc(s->context,sizeof(*p)); if(!o->scan_filter) return -4;
        *o->scan_filter=*p;
        for(unsigned col=0;col<p->nattrs;++col)
            if(p->cols[col].slot>=0 && (p->cols[col].type==Q_DECIMAL || p->cols[col].type==Q_STRING))
                o->scan_filter->cols[col].deferred=QS_FILTER_ONLY;
        o->scan_filter_checked=1;
    }
    if(!o->scan_filter_checked) {
        o->scan_filter_checked=1; uint8_t needed[QS_EXPRS]={0};
        if(p->predicate<0 && !o->defer_strings) return 0;
        struct qs_plan *filter=cs_work_alloc(s->context,sizeof(*p));
        if(!filter) { if(raw) s->raw_projection=0; return 0; }
        *filter=*p; filter->work_mem_parent=(uintptr_t)s->context;
        if(p->predicate<0) {
            for(unsigned col=0;col<p->nattrs;++col)
                if(filter->cols[col].slot>=0 && filter->cols[col].type==Q_STRING) filter->cols[col].deferred=QS_DEFERRED;
            o->scan_filter=filter; return 0;
        }
        /* Parameter values keep int128 payloads and their actual numeric scale.
         * The original PG plan remains unchanged; this copy belongs to a CU. */
        for(unsigned i=0;i<filter->nexprs;++i) {
            struct qs_expr *e=&filter->exprs[i];
            if(e->op==QP_INSET) goto scalar; /* Query-owned state is filtered by projection batches. */
            if(e->op==QP_PARAMETER) {
                if(e->column<0 || e->column>=QP_PARAMS) goto scalar;
                e->scale=s->params[e->column].scale; e->dscale=s->params[e->column].dscale;
            } else if(e->op==Q_ADD || e->op==Q_SUB || e->op==Q_MUL || e->op==Q_CASE) {
                if(e->left<0 || e->right<0 || e->left>=(int)i || e->right>=(int)i) goto scalar;
                unsigned left=filter->exprs[e->left].scale,right=filter->exprs[e->right].scale;
                unsigned scale=e->op==Q_MUL?left+right:left>right?left:right;
                if(e->op==Q_CASE) {
                    if(e->otherwise<0 || e->otherwise>=(int)i) goto scalar;
                    scale=right>filter->exprs[e->otherwise].scale?right:filter->exprs[e->otherwise].scale;
                }
                if(scale>UINT8_MAX) goto scalar;
                e->scale=scale;
            }
        }
        if(qs_vector_require(filter,filter->predicate,needed)) goto scalar;
        uint8_t projected[QS_EXPRS]={0}; int known=1;
        for(unsigned i=0;i<filter->nout;++i)
            if(qs_vector_require(filter,filter->outputs[i].index,projected)) { known=0; break; }
        for(unsigned col=0;col<filter->nattrs;++col) {
            struct qs_column *c=&filter->cols[col];
            if((c->type!=Q_DECIMAL && c->type!=Q_STRING) || c->slot<0) continue;
            c->deferred=raw || known?QS_FILTER_ONLY:QS_DEFERRED;
            for(unsigned i=0;!raw && i<filter->nexprs;++i) if(projected[i] && filter->exprs[i].op==Q_COLUMN && filter->exprs[i].column==(int)col)
                c->deferred=QS_DEFERRED;
        }
        /* Reuse the baseline Filter path on borrowed Heap strings. Only its
         * immutable literals need a resident dictionary; survivors are still
         * interned by the resumable projection, not by the flat Filter CSF. */
        if(s->paged) for(unsigned col=0;col<filter->nattrs;++col)
            if(filter->cols[col].type==Q_STRING && filter->cols[col].slot>=0)
                filter->cols[col].deferred=QS_FILTER_ONLY;
        for(unsigned i=0;i<filter->nexprs;++i) if(needed[i] && filter->exprs[i].op==Q_CONST && filter->exprs[i].type==Q_STRING && !filter->exprs[i].isnull) {
            const struct qs_string *literal=&filter->strings[filter->exprs[i].constant];
            struct qs_dict *dictionary=s->dictionary;
            if(s->paged) {
                if(!o->filter_dictionary) {
                    unsigned bytes=0;
                    for(unsigned j=0;j<filter->nstrings;++j) bytes+=filter->strings[j].length;
                    o->filter_dictionary=cs_work_calloc(s->context,1,qs_dict_bytes(512,bytes));
                    if(!o->filter_dictionary) goto scalar;
                    *o->filter_dictionary=(struct qs_dict){.entries=512,.capacity=512,.data=bytes};
                }
                dictionary=o->filter_dictionary;
            }
            int id=qs_dict_insert(dictionary,literal->bytes,literal->length);
            if(id<0) { cs_work_free(s->context,filter); return -3; }
            filter->exprs[i].constant=id;
        }
        o->scan_filter=filter; return 0;
scalar:
        if(raw) s->raw_projection=0;
        cs_work_free(s->context,o->filter_dictionary); o->filter_dictionary=NULL;
        cs_work_free(s->context,filter);
    }
    return 0;
}
static int64_t scan_chunk_next(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; const struct qs_plan *p=&n->expr; struct operator_state *o=&s->nodes[id];
    struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n));
    if(n->ordered_index && !o->index_control.state) {
        o->index_plan=cs_work_alloc(s->context,sizeof(*p)); if(!o->index_plan) return -4;
        *o->index_plan=*p; o->index_plan->work_mem_parent=(uintptr_t)s->context;
        o->index_plan->join.nattrs=p->nattrs; o->index_plan->join.work_mem_bytes=QS_INDEX_WORK_BYTES;
        int status=qs_index_init(o->index_plan,&o->index_control); if(status) return status;
        if(s->paged) { int bound=qs_index_paged_dictionary(&o->index_control,s->paged); if(bound) return bound; }
        else qs_index_dictionary(&o->index_control,s->dictionary);
    }
    for(;;) {
        if(!o->index_task.index_pending && (!n->ordered_index || !qs_index_dictionary_pending(&o->index_control,0)) &&
           !s->dictionary_wait && o->scan_position<chunk->selected) return 2;
        if(n->ordered_index) {
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
            continue;
        }
        if(s->control->ready && s->control->node==id && !s->control->index_nreads) {
            /* The retained heap input must not own the shared page-request
             * record: an Index Join may use that record between vectors. */
            o->scan_bytes=s->control->bytes;
            s->control->ready=s->control->bytes=0;
        }
        if(o->scan_bytes) {
            /* Keep the I/O block and fill vectors with complete heap pages.
             * Line-pointer counts conservatively include unused entries;
             * one page always fits the minimum capacity (QS_PAGE/24). */
            if(chunk->capacity<QS_PAGE/24 || o->scan_input>=o->scan_bytes) return -1;
            unsigned bytes=0,rows=0,remaining=o->scan_bytes-o->scan_input;
            if(remaining%QS_PAGE) return -1;
            const unsigned char *input=s->arena+input_offset(s,n)+o->scan_input;
            while(bytes<remaining) {
                uint16_t lower; memcpy(&lower,input+bytes+12,sizeof(lower));
                unsigned items=lower>=24?(lower-24)/4:0;
                if(bytes && rows+items>chunk->capacity) break;
                bytes+=QS_PAGE; rows+=items;
            }
            struct qs_task task={.input_bytes=bytes};
            int64_t status=scan_filter_init(s,p,o); if(status) return status;
            void *mr[]={s->arena+input_offset(s,n)+o->scan_input,o->scan_filter?o->scan_filter:(void *)p,chunk,&task,s->dictionary};
            long long len[]={bytes,sizeof(*p),qs_chunk_bytes(chunk->capacity,chunk->ncols),sizeof(task),0};
            struct femu_csd_args a={.numr=5,.mr_addr=mr,.mr_len=len};
            o->scan_raw=o->scan_input; o->scan_raw_bytes=bytes;
            const struct qs_plan *decoder=mr[1]; int eager_text=0;
            for(unsigned col=0;col<decoder->nattrs;++col)
                eager_text|=decoder->cols[col].slot>=0 && decoder->cols[col].type==Q_STRING && !decoder->cols[col].deferred;
            /* Numeric and borrowed-text scans cannot fault on a dictionary.
             * Keep the baseline decoder instead of saving a paging cursor. */
            if(s->paged && eager_text) {
                if(!o->scan_cursor) o->scan_cursor=qs_scan_cursor_create(s->context,s->paged);
                if(!o->scan_cursor) return -4;
                mr[4]=s->io; len[4]=s->io_bytes;
                struct blocking_timer timer={0}; if(s->profiling) timer=blocking_begin(s);
                status=qs_scan_columns(o->scan_cursor,&a,s->dictionary_wait?csReentryValue(s->args):0);
                if(s->profiling) blocking_end(s,QS_SCAN,timer);
                s->dictionary_wait=status>0?status:0;
            } else status=timed_call(s,pg_sina_scan_impl,&a,QS_SCAN);
            if(status) return status;
            s->metrics.scanned+=chunk->rows; s->metrics.input_bytes+=task.input_bytes; ++s->metrics.chunks;
            if(o->scan_filter && o->scan_filter->predicate>=0) {
                void *fm[]={chunk,o->scan_filter,mr[0],o->filter_dictionary?o->filter_dictionary:s->dictionary,s->params};
                long long fl[]={len[2],sizeof(*p),task.input_bytes,0,sizeof(s->params)};
                struct femu_csd_args filter={.numr=5,.mr_addr=fm,.mr_len=fl};
                status=timed_call(s,pg_sina_filter_impl,&filter,QS_FILTER);
                if(status==-4) {
                    /* Keep the query budget: if vector scratch does not fit,
                     * decode this retained input eagerly and use scalar SQL. */
                    s->raw_projection=0;
                    cs_work_free(s->context,o->scan_filter); o->scan_filter=NULL; mr[1]=(void *)p;
                    if(s->paged) {
                        if(!o->scan_cursor) o->scan_cursor=qs_scan_cursor_create(s->context,s->paged);
                        if(!o->scan_cursor) return -4;
                        mr[4]=s->io; len[4]=s->io_bytes;
                        status=qs_scan_columns(o->scan_cursor,&a,0);
                        s->dictionary_wait=status>0?status:0;
                    } else status=timed_call(s,pg_sina_scan_impl,&a,QS_SCAN);
                }
                if(status) return status;
            }
            o->scan_position=0; o->position+=task.input_bytes; o->scan_input+=task.input_bytes;
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
static int64_t scan_next(struct execution *s,unsigned id,struct qs_cell *row)
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
/* Translate a projection chain into one expression DAG over its producer's
 * columns. This preserves PG's tree and avoids row materialization
 * between adjacent projections. Unsupported expressions keep the old path. */
struct projection_import {
    struct execution *execution;
    struct projection_batch *batch;
    unsigned nodes[QS_EXPRS],expressions[QS_EXPRS];
};
static int projection_import(struct projection_import *in,unsigned node,int index)
{
    const struct qp_node *n=&in->execution->plan->nodes[node];
    if(index<0 || (unsigned)index>=n->expr.nexprs) return -1;
    struct qs_plan *p=&in->batch->plan;
    for(unsigned i=0;i<p->nexprs;++i) if(in->nodes[i]==node && in->expressions[i]==(unsigned)index) return i;
    struct qs_expr e=n->expr.exprs[index];
    if(in->batch->group_source && node==in->batch->scan) {
        /* Source expressions reference finalized group keys and aggregate
         * values, never the original aggregate input below this boundary. */
        if(e.op==Q_AGGREF) {
            if(e.column<0 || (unsigned)e.column>=n->expr.naggs) return -1;
            e.op=Q_COLUMN; e.column+=n->nkeys;
        } else if(e.op==Q_COLUMN) {
            unsigned k=0; while(k<n->nkeys && n->left_keys[k]!=e.column) ++k;
            if(k==n->nkeys) return -1;
            e.column=k;
        }
    }
    if(e.op>Q_CASE && e.op!=Q_YEAR && e.op!=Q_LIKE && e.op!=Q_SUBSTRING && e.op!=QP_PARAMETER && e.op!=QP_COALESCE && e.op!=Q_DIV && e.op!=QP_INSET) return -1;
    if(e.op==Q_COLUMN && (n->kind==QP_PROJECT || n->kind==QP_AGGREGATE) &&
       !(in->batch->group_source && node==in->batch->scan)) {
        const struct qp_node *child=&in->execution->plan->nodes[n->left];
        if(e.column<0 || (unsigned)e.column>=child->expr.nout) return -1;
        return projection_import(in,n->left,child->expr.outputs[e.column].index);
    }
    if(e.op==QP_PARAMETER) {
        if(e.column<0 || e.column>=QP_PARAMS) return -1;
        e.scale=in->execution->params[e.column].scale;
        e.dscale=in->execution->params[e.column].dscale;
    }
    if(e.op!=Q_COLUMN && e.op!=Q_CONST && e.op!=QP_PARAMETER) {
        if(e.left<0 || e.left>=index) return -1;
        e.left=projection_import(in,node,e.left); if(e.left<0) return -1;
        if(e.op!=Q_NOT && e.op!=Q_IS_NULL && e.op!=Q_IS_NOT_NULL && e.op!=Q_YEAR && e.op!=QP_INSET) {
            if(e.right<0 || e.right>=index) return -1;
            e.right=projection_import(in,node,e.right); if(e.right<0) return -1;
        }
        if(e.op==Q_CASE || e.op==Q_SUBSTRING) {
            if(e.otherwise<0 || e.otherwise>=index) return -1;
            e.otherwise=projection_import(in,node,e.otherwise); if(e.otherwise<0) return -1;
        }
    }
    /* PG numeric expressions carry a generic declared scale. The imported
     * vector DAG needs the actual scale of its rebound children, just as the
     * scalar evaluator derives it from runtime values. */
    if(e.op==Q_ADD || e.op==Q_SUB || e.op==Q_MUL || e.op==Q_CASE) {
        unsigned left=p->exprs[e.left].scale,right=p->exprs[e.right].scale;
        unsigned scale=e.op==Q_MUL?left+right:left>right?left:right;
        if(e.op==Q_CASE) scale=right>p->exprs[e.otherwise].scale?right:p->exprs[e.otherwise].scale;
        if(scale>UINT8_MAX) return -1;
        e.scale=scale;
    }
    if(e.op==Q_CONST && e.type==Q_STRING && !e.isnull) {
        if(e.constant<0 || e.constant>=n->expr.nstrings) return -1;
        const struct qs_string *v=&n->expr.strings[(unsigned)e.constant];
        if(in->execution->paged) {
            unsigned id=in->execution->literal_ids?in->execution->literal_ids[node*QS_STRINGS+(unsigned)e.constant]:0;
            if(!id) return -1;
            e.constant=id;
        } else {
            int id=qs_dict_insert(in->execution->dictionary,v->bytes,v->length); if(id<0) return -1;
            e.constant=id;
        }
    }
    if(p->nexprs==QS_EXPRS) return -1;
    unsigned at=p->nexprs++; p->exprs[at]=e; in->nodes[at]=node; in->expressions[at]=index;
    return at;
}
/* Keep condition dependencies and, for Index consumption, the columns needed
 * by the output DAG. Both consumers use the same query-budgeted vector state. */
static int join_qual_init(struct execution *s,unsigned id,struct projection_batch *b,
    const struct projection_batch *output)
{
    const struct qp_node *n=&s->plan->nodes[id];
    b->plan=n->expr; b->plan.nexprs=b->plan.nout=b->plan.ncols=0;
    struct projection_import import={.execution=s,.batch=b};
    int predicate=projection_import(&import,id,n->join_qual);
    if(predicate<0 || qs_vector_require(&b->plan,predicate,b->needed)) return -1;
    b->plan.predicate=predicate;
    if(output) b->plan.ncols=output->plan.ncols;
    for(unsigned col=0;col<b->plan.nattrs;++col)
        b->plan.cols[col].slot=output?output->plan.cols[col].slot:-1;
    for(unsigned i=0;i<b->plan.nexprs;++i) if(b->plan.exprs[i].op==Q_COLUMN) {
        int col=b->plan.exprs[i].column;
        if(col<0 || (unsigned)col>=b->plan.nattrs) return -1;
        if(b->plan.cols[col].slot<0) b->plan.cols[col].slot=b->plan.ncols++;
    }
    if(b->plan.ncols) b->cells=cs_work_alloc(s->context,(size_t)b->plan.ncols*QS_VECTOR_ROWS*sizeof(*b->cells));
    b->vectors=cs_work_alloc(s->context,b->plan.nexprs*sizeof(*b->vectors));
    if((b->plan.ncols && !b->cells) || !b->vectors) {
        cs_work_free(s->context,b->cells); cs_work_free(s->context,b->vectors);
        cs_work_free(s->context,b->group_input); return -1;
    }
    return 0;
}
static void merge_qual_close(struct execution *s,struct operator_state *o)
{
    if(!o->merge_qual) return;
    cs_work_free(s->context,o->merge_qual->expr.cells);
    cs_work_free(s->context,o->merge_qual->expr.vectors);
    cs_work_free(s->context,o->merge_qual); o->merge_qual=NULL;
}
/* Qualify consecutive candidates within one spool block. Retain the block
 * until its selected payloads drain; no speculative load crosses its end. */
static int projection_execute(struct projection_batch *,struct qs_chunk *,const uint32_t *,unsigned,struct qs_vector_input);
static int64_t merge_qual_next(struct execution *s,unsigned id,struct qs_cell *values)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    unsigned ln=s->plan->nodes[n->left].expr.nout,rn=s->plan->nodes[n->right].expr.nout;
    struct merge_qual_batch *q=o->merge_qual;
    if(q && q->expr.retry) goto evaluate_merge;
    if(!q || q->expr.position==q->expr.count) {
        int semi=n->join_type==QP_SEMI || n->join_type==QP_ANTI;
        if((o->merge_qual_checked && !q) || (semi && !o->position)) return 3;
        unsigned block_rows=QS_SORT_IO_BYTES/(rn*sizeof(*values));
        uint64_t remaining=o->group.rows-o->position;
        unsigned count=semi?64:QS_VECTOR_ROWS;
        if(remaining<count) count=remaining;
        if(count>block_rows-o->position%block_rows) count=block_rows-o->position%block_rows;
        if(count<2) return 3;
        struct qs_cell right[QS_OUT];
        int64_t status=spool_row(s,&o->group,o->position,rn,right);
        if(status) {
            if(status==-4 && q) { merge_qual_close(s,o); return 3; }
            return status;
        }
        if(!q) {
            o->merge_qual_checked=1;
            q=cs_work_calloc(s->context,1,sizeof(*q)); if(!q) return 3;
            if(join_qual_init(s,id,&q->expr,NULL)) { cs_work_free(s->context,q); return 3; }
            o->merge_qual=q;
        }
        struct projection_batch *b=&q->expr; q->begin=o->position;
        for(unsigned r=0;r<count;++r) {
            if(r) { status=spool_row(s,&o->group,q->begin+r,rn,right); if(status) return status; }
            for(unsigned col=0;col<b->plan.nattrs;++col) {
                int slot=b->plan.cols[col].slot;
                if(slot>=0) b->cells[(size_t)slot*QS_VECTOR_ROWS+r]=col<ln?o->left[col]:right[col-ln];
            }
        }
        b->count=count; b->position=0;
    evaluate_merge:;
        struct qs_vector_input input={.cells=q->expr.cells,.dictionary=s->dictionary,
            .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
        if(projection_execute(&q->expr,NULL,NULL,0,input)) return -5;
        q->expr.position=0; o->position+=q->expr.count;
    }
    struct projection_batch *b=&q->expr;
    while(b->position<b->count) {
        unsigned r=b->position,index=b->plan.predicate;
        if(b->vectors[index].errors[r]) return -5;
        struct qs_cell keep=qs_vector_cell(&b->plan,b->vectors,index,r);
        if(!keep.isnull && keep.value) {
            int64_t status=spool_row(s,&o->group,q->begin+r,rn,values+ln); if(status) return status;
            memcpy(values,o->left,ln*sizeof(*values)); ++b->position; return 2;
        }
        ++b->position;
    }
    b->position=b->count=0; return 0;
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
            hash_load(o->right,o->hash_data+(size_t)at*stride,&right->expr);
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
            hash_load(o->right,o->hash_data+(size_t)rows[r]*stride,&right->expr);
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
        hash_load(o->right,o->hash_data+(size_t)q->rows[r]*stride,&right->expr);
        return 2;
    }
    return 0;
scalar:
    hash_load(o->right,o->hash_data+(size_t)first*stride,&right->expr);
    return 3;
}
static int projection_init(struct execution *s,unsigned id,int source)
{
    struct operator_state *o=&s->nodes[id];
    struct projection_batch **slot=source?&o->source_projection:&o->projection;
    int *checked=source?&o->source_checked:&o->projection_checked;
    unsigned *deferred=source?&o->source_deferred:&o->projection_deferred;
    if(*slot || (*checked && (!*deferred || s->plan->nodes[*deferred-1].parallel ||
        projection_pending(s->nodes[*deferred-1].source_projection)))) return 0;
    *checked=1; *deferred=0;
    const struct qp_node *root=&s->plan->nodes[id];
    int aggregate=root->kind==QP_AGGREGATE && !source;
    if(aggregate && (root->distinct || root->distinct_aggs || root->left<0 ||
        (root->nkeys && ((!root->hash && !root->sorted_input) || !qp_hash_aggregate_supported(root))))) return 0;
    int grouped=aggregate && root->nkeys;
    int scan=aggregate?root->left:(int)id;
    unsigned projections[QP_NODES],nprojections=0;
    while(s->plan->nodes[scan].kind==QP_PROJECT) {
        const struct qp_node *n=&s->plan->nodes[scan];
        if(n->left<0) return 0;
        if(!s->worker && s->plan->workers>1 && n->parallel) { *deferred=scan+1; return 0; }
        projections[nprojections++]=scan;
        scan=n->left;
    }
    const struct qp_node *leaf=&s->plan->nodes[scan];
    int group_source=leaf->kind==QP_AGGREGATE && (!s->worker || (s->source_node==scan &&
        s->owner->partition_join<0 && s->owner->nodes[scan].group_phase==3));
    int joined=leaf->kind==QP_INDEX_JOIN || leaf->kind==QP_JOIN;
    int sorted=leaf->kind==QP_SORT && leaf->left>=0 &&
        leaf->expr.nattrs==s->plan->nodes[leaf->left].expr.nout;
    if(leaf->kind!=QP_SCAN && !joined && !sorted && !group_source) return 0;
    if(!s->worker && s->plan->workers>1 && leaf->parallel) { *deferred=scan+1; return 0; }
    if(group_source && !s->worker && projection_pending(s->nodes[scan].source_projection)) {
        *deferred=scan+1; return 0;
    }
    if(!group_source && !joined && !sorted && leaf->expr.predicate>=0) {
        if(leaf->ordered_index) return 0;
        int status=scan_filter_init(s,&leaf->expr,&s->nodes[scan]); if(status) return status;
    }
    struct projection_batch *b=cs_work_calloc(s->context,1,sizeof(*b)); if(!b) return 0;
    b->plan=leaf->expr; b->plan.nexprs=0; b->plan.nout=grouped?s->plan->nodes[root->left].expr.nout:
        aggregate?root->expr.naggs:root->expr.nout; b->scan=scan; b->joined=joined; b->sorted=sorted;
    b->group_source=group_source;
    b->filtered_scan=!joined && !sorted && !group_source && s->nodes[scan].scan_filter &&
        s->nodes[scan].scan_filter->predicate>=0;
    if(group_source) b->plan.nattrs=leaf->nkeys+leaf->expr.naggs;
    if(sorted) {
        b->plan.ncols=b->plan.nattrs;
        for(unsigned col=0;col<b->plan.nattrs;++col) b->plan.cols[col].slot=col;
    }
    struct projection_import import={.execution=s,.batch=b};
    /* Apply each node's WHERE/HAVING in input-to-output order. A NULL result
     * removes the row before any higher filter or projection is evaluated. */
    for(unsigned i=0;i<=nprojections;++i) {
        unsigned node=i?projections[nprojections-i]:(unsigned)scan;
        if(!i && b->filtered_scan) continue; /* Scan already filters. */
        int predicate=s->plan->nodes[node].expr.predicate;
        if(predicate<0) continue;
        int index=projection_import(&import,node,predicate);
        if(index<0 || qs_vector_dependency(&b->plan,index,b->needed)) { cs_work_free(s->context,b); return 0; }
        b->filters[b->nfilters].node=node; b->filters[b->nfilters++].expression=index;
    }
    if(grouped) {
        for(unsigned k=0;k<root->nkeys;++k) {
            int index=projection_import(&import,id,root->expr.keys[k]);
            if(index<0 || qs_vector_require(&b->plan,index,b->needed)) { cs_work_free(s->context,b); return 0; }
            b->group_keys[k]=index;
        }
        for(unsigned a=0;a<root->expr.naggs;++a) {
            b->group_values[a]=-1;
            if(root->expr.aggs[a].kind==Q_COUNT_STAR) continue;
            int index=projection_import(&import,id,root->expr.aggs[a].expr);
            if(index<0 || qs_vector_require(&b->plan,index,b->needed)) { cs_work_free(s->context,b); return 0; }
            b->group_values[a]=index;
        }
    }
    for(unsigned i=0;i<b->plan.nout;++i) {
        if(aggregate && !grouped && root->expr.aggs[i].kind==Q_COUNT_STAR) { b->plan.outputs[i].index=-1; continue; }
        const struct qp_node *source=grouped?&s->plan->nodes[root->left]:root;
        int index=projection_import(&import,grouped?(unsigned)root->left:id,
            aggregate && !grouped?root->expr.aggs[i].expr:source->expr.outputs[i].index);
        if(index<0 || qs_vector_require(&b->plan,index,b->needed)) { cs_work_free(s->context,b); return 0; }
        b->plan.outputs[i]=source->expr.outputs[i]; b->plan.outputs[i].index=index;
    }
    if(joined || group_source) {
        for(unsigned i=0;i<s->plan->count;++i) if(s->plan->nodes[i].kind==QP_INDEX_JOIN)
            for(int at=s->plan->nodes[i].left;at>=0;at=s->plan->nodes[at].left)
                if(at==scan) { b->keep_window=1; break; }
        b->plan.ncols=0;
        for(unsigned col=0;col<b->plan.nattrs;++col) b->plan.cols[col].slot=-1;
        for(unsigned i=0;i<b->plan.nexprs;++i) if(b->plan.exprs[i].op==Q_COLUMN) {
            int column=b->plan.exprs[i].column;
            if(column<0 || (unsigned)column>=b->plan.nattrs) { cs_work_free(s->context,b); return 0; }
            struct qs_column *col=&b->plan.cols[column];
            if(col->slot<0) col->slot=b->plan.ncols++;
        }
        if(b->plan.ncols) b->cells=cs_work_alloc(s->context,(size_t)b->plan.ncols*QS_VECTOR_ROWS*sizeof(*b->cells));
        if(b->plan.ncols && !b->cells) { cs_work_free(s->context,b); return 0; }
    }
    if(b->plan.nexprs) b->vectors=cs_work_alloc(s->context,b->plan.nexprs*sizeof(*b->vectors));
    if(b->plan.nexprs && !b->vectors) { cs_work_free(s->context,b->cells); cs_work_free(s->context,b); return 0; }
    /* Once this vector DAG is known to fit, let it filter raw Heap text.
     * Rejected values never enter the dictionary. A negative predicate on
     * this decoder copy means the DAG, rather than the flat Filter CSF,
     * owns filtering and the resumable materialization of survivors. */
    if(s->paged && !joined && !sorted && !group_source && !leaf->ordered_index &&
       !s->nodes[scan].scan_filter) {
        int text=0;
        for(unsigned col=0;col<leaf->expr.nattrs;++col) text|=leaf->expr.cols[col].type==Q_STRING;
        if(text) {
            struct qs_plan *decoder=cs_work_alloc(s->context,sizeof(*decoder));
            if(decoder) {
                *decoder=leaf->expr; decoder->predicate=-1;
                for(unsigned col=0;col<decoder->nattrs;++col) if(decoder->cols[col].type==Q_STRING && decoder->cols[col].slot>=0)
                    decoder->cols[col].deferred=QS_DEFERRED;
                s->nodes[scan].scan_filter=decoder;
            }
        }
    }
    *slot=b; return 0;
}
static void projection_append(struct projection_batch *b,const struct qs_cell *values)
{
    for(unsigned col=0;col<b->plan.nattrs;++col) {
        int slot=b->plan.cols[col].slot;
        if(slot>=0) b->cells[(size_t)slot*QS_VECTOR_ROWS+b->input_count]=values[col];
    }
    ++b->input_count;
}
/* Compact only requested results, after filters and output expressions have
 * run on surviving rows. Text stays a borrowed view for lazy Join payloads. */
static void projection_select(struct projection_batch *b,const uint16_t *rows,unsigned count)
{
    struct qs_cell values[QS_VECTOR_ROWS]; uint8_t errors[QS_VECTOR_ROWS];
    for(unsigned index=0;index<b->plan.nexprs;++index) if(b->needed[index]&2) {
        struct qs_vector *v=&b->vectors[index];
        if(v->kind==QS_CONSTANT) { v->done=count; continue; }
        if(v->kind==QS_TEXT) {
            v->all_valid=1;
            for(unsigned i=0;i<count;++i) {
                unsigned row=rows[i],error=v->errors[row],null=error || qs_vector_null(v,row);
                if(!null) v->strings[i]=v->strings[row];
                else { v->strings[i]=(struct qs_text_view){0}; }
                v->nulls[i]=null; v->errors[i]=error; v->all_valid&=!null;
            }
            v->done=count; continue;
        }
        int narrow=1;
        for(unsigned i=0;i<count;++i) {
            errors[i]=v->errors[rows[i]];
            values[i]=errors[i]?(struct qs_cell){.isnull=1}:qs_vector_cell(&b->plan,b->vectors,index,rows[i]);
            if(!values[i].isnull && (values[i].scale!=b->plan.exprs[index].scale ||
                values[i].value<INT64_MIN || values[i].value>INT64_MAX)) narrow=0;
        }
        v->kind=narrow?QS_NARROW:QS_DYNAMIC; v->all_valid=1; v->done=count;
        for(unsigned i=0;i<count;++i) {
            if(narrow) v->narrow[i]=values[i].isnull?0:(int64_t)values[i].value;
            else v->values[i]=values[i].value;
            v->nulls[i]=values[i].isnull; v->errors[i]=errors[i];
            v->scales[i]=values[i].scale; v->dscales[i]=values[i].dscale;
            v->all_valid&=!errors[i] && !values[i].isnull;
        }
    }
}
static int projection_execute_impl(struct projection_batch *b,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,struct qs_vector_input input)
{
    unsigned count=b->count;
    if(!b->nfilters) return qs_vector_execute_input(&b->plan,chunk,selection,first,count,b->needed,b->vectors,input)?-5:0;
    uint8_t dependencies[QS_EXPRS];
    for(unsigned i=0;i<b->plan.nexprs;++i) dependencies[i]=b->needed[i]&1;
    if(qs_vector_execute_input(&b->plan,chunk,selection,first,count,dependencies,b->vectors,input)) return -5;
    input.plan=&b->plan; input.vectors=b->vectors; input.count=count;
    uint16_t rows[QS_VECTOR_ROWS]; for(unsigned i=0;i<count;++i) rows[i]=i;
    unsigned kept=count,first_error=count;
    for(unsigned f=0;f<b->nfilters && kept;++f) {
        int index=b->filters[f].expression;
        if(qs_vector_evaluate(&input,index,rows,kept)) return -5;
        unsigned next=0;
        for(unsigned i=0;i<kept;++i) {
            unsigned row=rows[i];
            if(b->vectors[index].errors[row]) {
                if(row<first_error) { first_error=row; b->error=-5; b->error_node=b->filters[f].node; }
            } else if(row<first_error && !qs_vector_null(&b->vectors[index],row) && qs_vector_value(&b->vectors[index],row))
                rows[next++]=row;
        }
        kept=next;
    }
    for(unsigned i=0;i<b->plan.nexprs;++i) if((b->needed[i]&2) && kept && qs_vector_evaluate(&input,i,rows,kept)) return -5;
    if(kept!=count) projection_select(b,rows,kept);
    b->count=kept; return 0;
}
static int projection_execute(struct projection_batch *b,struct qs_chunk *chunk,
    const uint32_t *selection,unsigned first,struct qs_vector_input input)
{
    struct execution *s=input.set_state;
    if(s && s->paged) { input.text=s->text; input.text_io=s->io; input.resume=b->retry; }
    b->retry_input=input; b->retry_chunk=chunk; b->retry_selection=selection;
    b->retry_first=first; b->retry_count=b->count;
    int status=projection_execute_impl(b,chunk,selection,first,input);
    if(status && input.text) text_status(s,qs_dictionary_text_request(input.text));
    b->retry=status && s && s->expression_wait;
    return status;
}
static int64_t projection_join_fill(struct execution *s,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[b->scan]; struct operator_state *o=&s->nodes[b->scan];
    unsigned ln=s->plan->nodes[n->left].expr.nout;
    if(b->error) { s->control->error=b->error; s->control->error_node=b->error_node; return b->error; }
    if(n->kind==QP_INDEX_JOIN) {
        b->borrowed_cells=NULL; b->borrowed_selection=NULL;
        int previous=s->control->error; unsigned previous_node=s->control->error_node;
        int64_t status=index_next(s,b->scan,NULL,b);
        if(s->expression_wait) return status;
        if(status<0 && b->input_count) {
            b->error=status; b->error_node=s->control->error?s->control->error_node:b->scan;
            s->control->error=previous; s->control->error_node=previous_node;
        } else if(status!=2) return status;
        goto evaluate_batch;
    }
    while(b->input_count<QS_VECTOR_ROWS) {
        int previous=s->control->error; unsigned previous_node=s->control->error_node;
        int merge=!n->hash || o->hash_fallback==1;
        struct qs_cell values[QS_COLS];
        int64_t status=b->joined_row?2:merge?merge_match_next(s,b->scan,values):hash_match_next(s,b->scan,b->keep_window && b->input_count);
        /* Hash construction can choose the existing Merge fallback before
         * producing rows. Keep this DAG and begin collecting Merge pairs. */
        if(status==3 && !merge && o->hash_fallback==1) continue;
        if(status==QP_BATCH_END) break;
        if(status!=2) {
            if(s->expression_wait) return status;
            /* The shared page request must be serviced before another
             * operator can issue I/O. Keep this partial column batch across
             * the wait instead of exposing it and requesting the page twice. */
            if(status==QP_WAIT || status>2) return status;
            if(status<0) {
                b->error=status; b->error_node=s->control->error?s->control->error_node:b->scan;
                s->control->error=previous; s->control->error_node=previous_node;
                if(!b->input_count) { s->control->error=status; s->control->error_node=b->error_node; return status; }
            }
            if(!b->input_count) return status;
            break;
        }
        if(merge) { projection_append(b,values); continue; }
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
    struct qs_vector_input input={.cells=b->borrowed_cells?b->borrowed_cells:b->cells,.dictionary=s->dictionary,
        .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    /* COUNT(*) can have no payload columns; it still consumes cardinality. */
    if(b->plan.nexprs && projection_execute(b,NULL,b->borrowed_cells?b->borrowed_selection:NULL,0,input)) return -5;
    return 2;
}
/* A Sort Source already produces evaluated cells in complete batches. Keep
 * its buffer alive until the dependent expression vectors have been consumed. */
static int64_t projection_sort_fill(struct execution *s,struct projection_batch *b)
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
    unsigned consumed=b->count;
    if(projection_execute(b,NULL,NULL,0,input)) return -5;
    sort->position+=consumed;
    return 2;
}
static void projection_clear(struct execution *s,struct projection_batch **slot)
{
    struct projection_batch *b=*slot; if(!b) return;
    cs_work_free(s->context,b->cells); cs_work_free(s->context,b->vectors);
    cs_work_free(s->context,b->group_input); cs_work_free(s->context,b); *slot=NULL;
}
/* Return 3 only for an internal scalar fallback, before consuming input. */
static int64_t projection_fill(struct execution *s,unsigned id,int source)
{
    struct projection_batch **slot=source?&s->nodes[id].source_projection:&s->nodes[id].projection;
    struct projection_batch *b=*slot;
    if(b->materializing) return materialize_outputs(s,b)?-3:2;
    if(b->retry) {
        b->count=b->retry_count;
        if(projection_execute(b,b->retry_chunk,b->retry_selection,b->retry_first,b->retry_input)) return -5;
        if(b->sorted) s->nodes[b->scan].left_sort.position+=b->retry_count;
        else if(!b->group_source && !b->joined) s->nodes[b->scan].scan_position+=b->retry_count;
        b->position=0;
        if(!b->lazy_text && materialize_outputs(s,b)) return -3;
    }
    while(b->position==b->count) {
        if(b->error) { s->control->error=b->error; s->control->error_node=b->error_node; return b->error; }
        int64_t status=b->group_source?projection_group_fill(s,b):b->sorted?projection_sort_fill(s,b):
            b->joined?projection_join_fill(s,b):scan_chunk_next(s,b->scan);
        if(status!=2) {
            if(!status || (b->joined && status==3)) {
                projection_clear(s,slot);
            }
            return status;
        }
        /* Filter may have fallen back after exhausting its scratch budget. */
        if(b->filtered_scan && !s->nodes[b->scan].scan_filter) {
            projection_clear(s,slot); return 3;
        }
        if(!b->group_source && !b->joined && !b->sorted) {
            struct operator_state *scan=&s->nodes[b->scan];
            struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,&s->plan->nodes[b->scan]));
            b->count=chunk->selected-scan->scan_position; if(b->count>QS_VECTOR_ROWS) b->count=QS_VECTOR_ROWS;
            const struct qp_node *source=&s->plan->nodes[b->scan];
            if(s->paged || (scan->defer_strings && source->expr.predicate<0))
                for(unsigned col=0;col<b->plan.nattrs;++col) if(b->plan.cols[col].type==Q_STRING)
                    b->plan.cols[col].deferred=scan->scan_filter?scan->scan_filter->cols[col].deferred:QS_EAGER;
            const void *raw=scan->scan_raw_bytes?s->arena+input_offset(s,source)+scan->scan_raw:NULL;
            struct qs_vector_input input={.raw=raw,.raw_bytes=scan->scan_raw_bytes,.dictionary=s->dictionary,
                .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
            unsigned consumed=b->count;
            if(projection_execute(b,chunk,qs_selection(chunk),scan->scan_position,input)) return -5;
            scan->scan_position+=consumed; b->position=0;
        }
        /* A retained output must outlive this vector's byte views. Predicates
         * do not intern their temporary slices; projected text does. */
        if(!b->lazy_text && materialize_outputs(s,b)) return -3;
    }
    return 2;
}
/* Feed one producer's evaluated columns straight into the existing run
 * storage. Keep the batch and cursor fixed until its spill request completes. */
static int64_t sort_sink_columns(struct execution *s,struct sorted_rows *sort,unsigned id)
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
static int64_t projection_next(struct execution *s,unsigned id,struct qs_cell *row,int source)
{
    int64_t status=projection_fill(s,id,source);
    if(status==3) return next_row_impl(s,id,row);
    if(status!=2) return status;
    struct projection_batch *b=source?s->nodes[id].source_projection:s->nodes[id].projection;
    for(unsigned i=0;i<b->plan.nout;++i) {
        unsigned index=b->plan.outputs[i].index;
        if(b->vectors[index].errors[b->position]) return -5;
        row[i]=qs_vector_cell(&b->plan,b->vectors,index,b->position);
    }
    ++b->position; ++s->metrics.matched; return 2;
}
/* Reference the producer's expression columns, hash only its keys, then
 * scatter directly into the compact build rows. No intermediate AFDM rows.
 * A full table leaves the vector cursor for the existing growth/spill path. */
static int hash_sink_vectors(struct execution *s,unsigned id)
{
    struct projection_batch *b=s->plan->nodes[id].kind==QP_AGGREGATE?
        s->nodes[id].source_projection:s->nodes[id].projection;
    const struct qp_node *n=&s->plan->nodes[s->hash_sink-1];
    struct operator_state *o=&s->owner->nodes[s->hash_sink-1];
    const struct qs_plan *right=&s->plan->nodes[n->right].expr;
    if(!o->hash_data || o->hash_fallback) return 0;
    if(s->paged && materialize_outputs(s,b)) return -3;
    unsigned at,count=hash_reserve_rows(o,b->count-b->position,&at);
    if(!count) return 0;
    uint64_t hashes[QS_VECTOR_ROWS];
    for(unsigned r=0;r<count;++r) hashes[r]=UINT64_C(0x9e3779b97f4a7c15);
    for(unsigned k=0;k<n->nkeys;++k) {
        int index=b->plan.outputs[n->right_keys[k]].index;
        for(unsigned r=0;r<count;++r) {
            unsigned row=b->position+r;
            if(b->vectors[index].errors[row]) return -5;
            hashes[r]=hash_key(hashes[r],qs_vector_cell(&b->plan,b->vectors,index,row));
        }
    }
    unsigned stride=hash_row_bytes(right),offset=0;
    for(unsigned col=0;col<right->nout;++col) {
        int index=b->plan.outputs[col].index;
        unsigned bytes=hash_value_bytes(right,col);
        for(unsigned r=0;r<count;++r) {
            unsigned row=b->position+r;
            if(b->vectors[index].errors[row]) return -5;
            struct qs_cell value=qs_vector_cell(&b->plan,b->vectors,index,row);
            hash_store_value(o->hash_data+(size_t)(at+r)*stride+offset,&value,bytes);
        }
        offset+=bytes+(bytes<16?1:3);
    }
    uint32_t *next=(void *)(o->hash_data+(size_t)o->hash_capacity*stride);
    uint32_t *buckets=next+o->hash_capacity;
    for(unsigned r=0;r<count;++r) {
        unsigned bucket=hashes[r]&(o->hash_buckets-1);
        next[at+r]=__atomic_exchange_n(&buckets[bucket],at+r,__ATOMIC_RELAXED);
    }
    b->position+=count; s->metrics.matched+=count; s->metrics.build_rows+=count;
    return 0;
}
/* Probe the bucket array as a batch, keeping column payloads borrowed until
 * a candidate needs them. Match-chain state remains in the join operator, so
 * one input can produce arbitrarily many output batches without rereading it. */
static int64_t hash_probe_input(struct execution *s,unsigned id,const uint32_t *buckets)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    o->hash_payload=0;
    const struct qp_node *left=&s->plan->nodes[n->left];
    /* Aggregate Source columns are separate from its Sink input vectors. */
    int source=left->kind==QP_AGGREGATE;
    int eligible=(left->kind==QP_SCAN || left->kind==QP_PROJECT || left->kind==QP_JOIN || left->kind==QP_INDEX_JOIN || source) &&
        !o->external && s->source_node<0 && !(!s->worker && s->plan->workers>1 && left->parallel);
    if(eligible) {
        int status=projection_init(s,n->left,source); if(status) return status;
        struct projection_batch *b=source?s->nodes[n->left].source_projection:s->nodes[n->left].projection;
        if(b && !o->hash_probe) {
            o->hash_probe=cs_work_calloc(s->context,1,sizeof(*o->hash_probe));
            if(o->hash_probe) {
                b->lazy_text=1;
                const struct qp_node *source=&s->plan->nodes[b->scan];
                if(source->kind==QP_SCAN && !source->ordered_index && source->expr.predicate<0)
                    for(unsigned col=0;col<source->expr.nattrs;++col)
                        if(source->expr.cols[col].slot>=0 && source->expr.cols[col].type==Q_STRING)
                            s->nodes[b->scan].defer_strings=1;
            }
        }
        if(b && o->hash_probe) {
            int64_t ready=projection_fill(s,n->left,source);
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
                    for(unsigned r=0;r<b->count;++r) {
                        uint64_t hash=UINT64_C(0x9e3779b97f4a7c15); unsigned null=0,error=0;
                        for(unsigned col=0;col<b->plan.nout;++col)
                            error|=b->vectors[b->plan.outputs[col].index].errors[r];
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
                    probe->ready=1;
                }
                unsigned r=b->position++;
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
    if(status==2) {
        unsigned at=keys_null(n,o->left)?UINT_MAX:buckets[row_hash(n,o->left,0)&(o->hash_buckets-1)];
        o->hash_match=at==UINT_MAX?0:at+1;
    }
    return status;
}
/* A payload text view becomes a canonical ID only for a matching row.
 * Cache it with the other retained cells for duplicate build-side matches. */
static int hash_left_cell(struct execution *s,unsigned id,unsigned col,struct qs_cell *value)
{
    struct operator_state *o=&s->nodes[id];
    if(o->hash_payload&(1U<<col)) {
        unsigned left=s->plan->nodes[id].left;
        struct projection_batch *b=s->plan->nodes[left].kind==QP_AGGREGATE?
            s->nodes[left].source_projection:s->nodes[left].projection;
        unsigned r=b->position-1; int index=b->plan.outputs[col].index;
        const struct qs_vector *v=&b->vectors[index];
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
static int hash_left_payload(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    for(unsigned columns=o->hash_payload;columns;columns&=columns-1) {
        unsigned col=__builtin_ctz(columns); struct qs_cell value;
        int status=hash_left_cell(s,id,col,&value); if(status) return status;
    }
    return 0;
}
static struct aggregate_input *aggregate_input(struct execution *s,struct operator_state *o)
{
    if(!o->aggregate_input) o->aggregate_input=cs_work_calloc(s->context,1,sizeof(*o->aggregate_input));
    return o->aggregate_input;
}
/* active: 1 initializes group keys, 2 compares this row with the group,
 * 3 evaluates/accumulates arguments, 4 evaluates hash-aggregate arguments.
 * Only one of these can wait per row. */
static int initialize_group_keys(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    struct qs_eval flat,*e=&flat; struct aggregate_input *input=NULL;
    unsigned first=0;
    if(s->text) {
        input=aggregate_input(s,o); if(!input) return -4;
        if(!input->active) { input->active=1; input->argument=0; init_eval(&input->evaluation,&n->expr,o->left,NULL); }
        if(input->active!=1) return -5;
        e=&input->evaluation; first=input->argument;
    } else init_eval(e,&n->expr,o->left,NULL);
    for(unsigned k=first;k<n->expr.nkeys;++k) {
        if(evaluate(s,e,n->expr.keys[k],&o->keys[k])) return -5;
        if(input) input->argument=k+1;
    }
    if(input) input->active=0;
    return 0;
}
static int group_same(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    struct qs_eval flat,*e=&flat; struct aggregate_input *input=NULL;
    unsigned first=0;
    if(s->text) {
        input=aggregate_input(s,o); if(!input) return -4;
        if(!input->active) { input->active=2; input->argument=0; init_eval(&input->evaluation,&n->expr,o->left,NULL); }
        if(input->active!=2) return -5;
        e=&input->evaluation; first=input->argument;
    } else init_eval(e,&n->expr,o->left,NULL);
    for(unsigned k=first;k<n->expr.nkeys;++k) {
        struct qs_cell key; if(evaluate(s,e,n->expr.keys[k],&key)) return -1;
        int equal=equal_cell(s,key,o->keys[k],n->expr.exprs[n->expr.keys[k]].type);
        if(equal<=0) { if(input) input->active=0; return equal; }
        if(input) input->argument=k+1;
    }
    if(input) input->active=0;
    return 1;
}
static inline int accumulate_value(struct execution *s,const struct qp_node *n,
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
static int accumulate_paged(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    struct aggregate_input *input=aggregate_input(s,o); if(!input) return -4;
    if(!input->active) {
        input->active=3; input->ready=0; input->argument=input->aggregate=0;
        init_eval(&input->evaluation,&n->expr,o->left,NULL);
    }
    if(input->active!=3) return -5;
    for(;!input->ready && input->argument<n->expr.naggs;++input->argument) {
        unsigned a=input->argument; const struct qs_agg *agg=&n->expr.aggs[a];
        input->values[a]=(struct qs_cell){.value=1};
        if(agg->kind!=Q_COUNT_STAR && evaluate(s,&input->evaluation,agg->expr,&input->values[a])) return -5;
    }
    input->ready=1;
    for(;input->aggregate<n->expr.naggs;++input->aggregate) {
        unsigned i=input->aggregate; const struct qs_agg *agg=&n->expr.aggs[i];
        struct accumulator *a=&o->accum[i]; struct qs_cell v=input->values[i];
        if(v.isnull) continue;
        if((n->distinct_aggs&(1U<<i)) && a->has_last) {
            int equal=equal_cell(s,a->last,v,n->expr.exprs[agg->expr].type);
            if(equal<0) return -5;
            if(equal) continue;
        }
        int status=accumulate_value(s,n,agg,a,v); if(status) return status;
        /* Publish DISTINCT's last value only after MIN/MAX comparison succeeds. */
        if(n->distinct_aggs&(1U<<i)) { a->last=v; a->has_last=1; }
    }
    input->active=input->ready=0; return 0;
}
static int accumulate(struct execution *s,const struct qp_node *n,struct operator_state *o)
{
    if(s->text) return accumulate_paged(s,n,o);
    struct qs_eval e; init_eval(&e,&n->expr,o->left,NULL);
    struct qs_cell values[QS_AGGS];
    for(unsigned i=0;i<n->expr.naggs;++i) {
        values[i]=(struct qs_cell){.value=1};
        if(n->expr.aggs[i].kind!=Q_COUNT_STAR && evaluate(s,&e,n->expr.aggs[i].expr,&values[i])) return -5;
    }
    for(unsigned i=0;i<n->expr.naggs;++i) {
        const struct qs_agg *agg=&n->expr.aggs[i]; struct accumulator *a=&o->accum[i]; struct qs_cell v=values[i];
        if(v.isnull) continue;
        if(n->distinct_aggs&(1U<<i)) {
            int cmp=1;
            if(a->has_last) cmp=compare_cell(s,a->last,v,n->expr.exprs[agg->expr].type);
            if(cmp==INT_MIN) return -5;
            if(!cmp) continue;
            a->last=v; a->has_last=1;
        }
        int status=accumulate_value(s,n,agg,a,v); if(status) return status;
    }
    return 0;
}
static int64_t aggregate_next(struct execution *,unsigned,struct qs_cell *);
static int64_t aggregate_next_impl(struct execution *,unsigned,struct qs_cell *,int);
static int group_add(struct group_value *g,struct qs_cell v,uint64_t count,unsigned kind)
{
    if(count && (kind==Q_SUM || kind==Q_AVG)) {
        if(!g->count) g->sum=v;
        else {
            unsigned scale=v.scale>g->sum.scale?v.scale:g->sum.scale; __int128 left,right;
            if(qs_scale(g->sum.value,scale-g->sum.scale,&left) || qs_scale(v.value,scale-v.scale,&right) ||
               __builtin_add_overflow(left,right,&g->sum.value)) return -5;
            g->sum.scale=scale; if(v.dscale>g->sum.dscale) g->sum.dscale=v.dscale;
        }
    }
    return __builtin_add_overflow(g->count,count,&g->count)?-5:0;
}
static int group_update(struct execution *s,const struct qp_node *n,unsigned aggregate,
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
static struct compact_group *group_at(const struct operator_state *o,unsigned slot)
{ return (void *)((unsigned char *)o->groups+(size_t)slot*o->group_stride); }
static void group_keys(const struct qp_node *n,const struct operator_state *o,const struct compact_group *g,struct qs_cell *keys)
{
    int wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
    const void *stored=g->values+n->expr.naggs;
    if(wide && o->group_integer) {
        const int64_t *values=stored;
        for(unsigned k=0;k<n->nkeys;++k) keys[k]=(struct qs_cell){.value=values[k],.isnull=!!(g->key_flags&(2U<<k))};
    } else if(wide) memcpy(keys,stored,n->nkeys*sizeof(*keys));
    else keys[0]=(struct qs_cell){.value=g->key,.isnull=g->key_flags==2};
}
static uint64_t group_hash(const struct qp_node *n,const struct qs_cell *keys)
{
    uint64_t hash=UINT64_C(0x9e3779b97f4a7c15);
    for(unsigned k=0;k<n->nkeys;++k) {
        struct qs_cell v=keys[k];
        if(v.isnull) v=(struct qs_cell){0};
        while(v.scale && !(v.value%10)) { v.value/=10; --v.scale; }
        hash=mix_hash(hash^(uint64_t)v.value^(uint64_t)((unsigned __int128)v.value>>64)^((uint64_t)v.scale<<56));
    }
    return hash;
}
static int group_equal(struct execution *s,const struct qp_node *n,const struct qs_cell *a,const struct qs_cell *b)
{
    for(unsigned k=0;k<n->nkeys;++k) {
        int equal=equal_cell(s,a[k],b[k],n->expr.exprs[n->expr.keys[k]].type);
        if(equal<0) return -5;
        if(!equal) return 0;
    }
    return 1;
}
static int group_final_value(const struct qs_agg *agg,const struct group_value *g,struct qs_cell *value)
{
    *value=(struct qs_cell){.isnull=!g->count};
    if(agg->kind==Q_COUNT || agg->kind==Q_COUNT_STAR) *value=(struct qs_cell){.value=g->count};
    else if(g->count) {
        *value=g->sum;
        if(agg->kind==Q_AVG && qs_average(*value,g->count,value)) return -5;
    }
    return 0;
}
static int group_result(struct execution *s,const struct qp_node *n,const struct group_value *groups,
    const struct qs_cell *keys,struct qs_cell *row,int raw)
{
    struct qs_cell input[QS_COLS],values[QS_AGGS];
    for(unsigned i=0;i<QS_COLS;++i) input[i]=(struct qs_cell){.isnull=1};
    for(unsigned k=0;k<n->nkeys;++k) input[n->left_keys[k]]=keys[k];
    for(unsigned a=0;a<n->expr.naggs;++a) {
        int status=group_final_value(&n->expr.aggs[a],&groups[a],&values[a]); if(status) return status;
    }
    if(raw) {
        memcpy(row,keys,n->nkeys*sizeof(*row));
        memcpy(row+n->nkeys,values,n->expr.naggs*sizeof(*row)); return 2;
    }
    int status=project(s,n,input,values,row);
    return status<0?status:status?2:0;
}
static const struct compact_group *group_source_next(struct execution *s,unsigned id)
{
    const struct operator_state *source=&s->owner->nodes[id];
    if(s->source_at==s->source_end) {
        s->source_at=atomic_fetch_add_explicit(&s->owner->next_offset,64,memory_order_relaxed);
        unsigned rows=atomic_load_explicit(source->group_count,memory_order_relaxed);
        if(s->source_at>=rows) { s->source_end=s->source_at; return NULL; }
        s->source_end=s->source_at+64;
        if(s->source_end>rows) s->source_end=rows;
    }
    return group_at(source,s->source_at++);
}
/* Separate Source columns from the aggregate's input vectors. Read-only CU
 * Sources claim records; serial/stream/spilled Sources advance their original
 * group iterator. Retain partial batches while its I/O request is serviced. */
static int64_t projection_group_fill(struct execution *s,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[b->scan];
    if(b->error) { s->control->error=b->error; s->control->error_node=b->error_node; return b->error; }
    while(b->input_count<QS_VECTOR_ROWS) {
        struct qs_cell values[QS_COLS]; int64_t status;
        int previous=s->control->error; unsigned previous_node=s->control->error_node;
        if(s->worker && s->source_node==(int)b->scan) {
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
/* Published keys stay immutable throughout a CU wave. The bounded shared
 * table also accepts rows that do not fit a CU-local table. Only the owner
 * grows/flushes it between CU waves; full tables retain the original rows. */
#define GROUP_LOCK (1U<<31)
#define GROUP_TAG_MASK 0x7fff0000U
enum { GROUP_LOCAL, GROUP_SHARED, GROUP_PARTITION };
/* DuckDB's hash salts reject collisions in the compact lookup array before
 * fetching a much larger aggregate record. Low bits retain NULL/key flags. */
static unsigned group_entry_state(unsigned flags,uint64_t hash)
{ return flags|(((unsigned)(hash>>48)&0x7fffU)<<16); }
static uint64_t group_values_hash(const struct qp_node *n,const struct qs_cell *keys)
{
    return n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER?group_hash(n,keys):
        mix_hash(keys[0].isnull?0:(uint64_t)keys[0].value);
}
static int group_text_extreme(const struct execution *s,const struct qp_node *n)
{
    if(!s->text) return 0;
    for(unsigned a=0;a<n->expr.naggs;++a) {
        const struct qs_agg *agg=&n->expr.aggs[a];
        if((agg->kind==Q_MIN || agg->kind==Q_MAX) && n->expr.exprs[agg->expr].type==Q_STRING) return 1;
    }
    return 0;
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
static int group_update_continue(struct execution *s,unsigned id)
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
        for(unsigned k=0;k<n->nkeys;++k) {
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
            for(unsigned k=0;equal && k<n->nkeys;++k)
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
                for(unsigned k=0;k<n->nkeys;++k) key_values[k]=keys[k].isnull?0:(int64_t)keys[k].value;
            } else if(wide) memcpy(g->values+n->expr.naggs,keys,n->nkeys*sizeof(*keys));
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
static int group_sink_values(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *keys,const struct group_value *values,int shared)
{ return group_sink_hashed_values(s,id,o,keys,values,shared,group_values_hash(&s->plan->nodes[id],keys)); }
static struct operator_state *group_worker_local(struct execution *s,unsigned id)
{
    struct operator_state *local=s->worker?s->nodes[id].group_local:NULL;
    if(local && !local->phase) {
        memset(local->group_entries,0,(size_t)local->group_capacity*sizeof(*local->group_entries)); local->phase=1;
    }
    return local;
}
static int group_worker_values(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *keys,const struct group_value *values,int shared,uint64_t hash)
{
    if(s->group_update && s->group_update->active) return group_update_continue(s,id);
    struct operator_state *local=shared?group_worker_local(s,id):NULL;
    if(local) {
        int added=group_sink_hashed_values(s,id,local,keys,values,GROUP_LOCAL,hash); if(added) return added;
    }
    return group_sink_hashed_values(s,id,o,keys,values,shared,hash);
}
static int group_sink_row_paged(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *row,int shared)
{
    const struct qp_node *n=&s->plan->nodes[id];
    struct aggregate_input *input=aggregate_input(s,&s->nodes[id]); if(!input) return -4;
    if(s->group_update && s->group_update->active) {
        int status=group_update_continue(s,id); if(status>=0) input->active=0;
        return status;
    }
    if(!input->active) {
        input->active=4; input->argument=0;
        init_eval(&input->evaluation,&n->expr,row,NULL);
    }
    if(input->active!=4) return -5;
    for(;input->argument<n->expr.naggs;++input->argument) {
        unsigned a=input->argument; input->values[a]=(struct qs_cell){.value=1};
        if(n->expr.aggs[a].kind!=Q_COUNT_STAR &&
           evaluate(s,&input->evaluation,n->expr.aggs[a].expr,&input->values[a])) return -5;
    }
    struct qs_cell keys[QS_KEYS]; struct group_value values[QS_AGGS];
    for(unsigned k=0;k<n->nkeys;++k) keys[k]=row[n->left_keys[k]];
    for(unsigned a=0;a<n->expr.naggs;++a)
        values[a]=(struct group_value){.sum=input->values[a],.count=!input->values[a].isnull};
    int status=group_worker_values(s,id,o,keys,values,shared,group_values_hash(n,keys));
    if(status>=0) input->active=0;
    return status;
}
static int group_sink_row(struct execution *s,unsigned id,struct operator_state *o,
    const struct qs_cell *row,int shared)
{
    if(s->text) return group_sink_row_paged(s,id,o,row,shared);
    const struct qp_node *n=&s->plan->nodes[id];
    struct qs_cell keys[QS_KEYS]; struct group_value values[QS_AGGS];
    for(unsigned k=0;k<n->nkeys;++k) keys[k]=row[n->left_keys[k]];
    struct qs_eval eval; int initialized=0;
    for(unsigned a=0;a<n->expr.naggs;++a) {
        struct qs_cell value={.value=1};
        if(n->expr.aggs[a].kind!=Q_COUNT_STAR) {
            int index=n->expr.aggs[a].expr;
            if(n->expr.exprs[index].op==Q_COLUMN) value=row[n->expr.exprs[index].column];
            else {
                if(!initialized) { init_eval(&eval,&n->expr,row,NULL); initialized=1; }
                if(evaluate(s,&eval,index,&value)) return -5;
            }
        }
        values[a]=(struct group_value){.sum=value,.count=!value.isnull};
    }
    return group_worker_values(s,id,o,keys,values,shared,group_values_hash(n,keys));
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
/* Keep keys and aggregate arguments in vectors. Only rows rejected by a full
 * shared table enter the existing AFDM adapter, so its owner can grow/spill
 * between waves without losing the remainder of this vector. */
static int group_sink_vectors(struct execution *s,unsigned id,struct qs_result *out,unsigned capacity)
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
static int group_sink_batch(struct execution *s,struct qs_result *out)
{
    unsigned id=s->group_sink-1,columns=s->plan->nodes[s->root].expr.nout;
    struct operator_state *o=&s->owner->nodes[id],*local=&s->nodes[id];
    for(;out->begin<out->rows;++out->begin) {
        struct qs_cell *row=(struct qs_cell *)out->data+(size_t)out->begin*columns;
        int status=group_sink_row(s,id,o,row,1); if(status<0) return status;
        if(!status) {
            memmove((struct qs_cell *)out->data+(size_t)local->group_batch_kept++*columns,row,columns*sizeof(*row));
        }
    }
    out->rows=local->group_batch_kept; out->begin=local->group_batch_kept=0; return 0;
}
static void group_drop_local(struct execution *s,struct operator_state *local)
{
    if(!local) return;
    cs_work_free(s->context,local->groups); cs_work_free(s->context,local->group_entries);
    cs_work_free(s->context,local);
}
/* Called only after the producing CUs have finished. Keep the current partial
 * state until its merge succeeds, including across table growth and spill. */
static struct compact_group *group_local_next(struct execution *s,struct operator_state *o)
{
    while(o->group_local_slot<o->group_local_count) {
        struct operator_state *local=o->group_locals[o->group_local_slot];
        unsigned rows=atomic_load_explicit(local->group_count,memory_order_relaxed);
        if(o->group_local_row<local->group_flush) o->group_local_row=local->group_flush;
        if(rows && !o->group_local_row && !atomic_load(o->group_count) &&
           local->group_capacity==o->group_capacity) {
            /* Transfer the first table instead of copying all its groups. */
            cs_work_free(s->context,o->groups); cs_work_free(s->context,o->group_entries);
            o->groups=local->groups; o->group_entries=local->group_entries;
            local->groups=NULL; local->group_entries=NULL; atomic_store(o->group_count,rows);
            o->group_local_row=rows;
        }
        if(o->group_local_row<rows) return group_at(local,o->group_local_row);
        group_drop_local(s,local); o->group_locals[o->group_local_slot++]=NULL; o->group_local_row=0;
    }
    cs_work_free(s->context,o->group_locals); o->group_locals=NULL;
    o->group_local_count=o->group_local_slot=0; return NULL;
}
/* Retain the compact integer-key path. Other keys add only their actual cell
 * count and grow from a small table; spill value/count states, never raw rows. */
static size_t group_reserve(const struct execution *s,const struct qp_node *n)
{
    size_t reserve=2*s->sort_limit;
    /* Hash groups are allocated before their scan launches. Reserve the actual
     * worker state too, or a small query budget can lose
     * all spill space as soon as the workers start. Active workers are already
     * charged to the parent and must not be counted a second time. */
    if(s->plan->workers>1 && !s->parallel_started && s->plan->nodes[n->left].parallel && !s->nodes[n->left].finished) {
        int leaf=qp_probe_scan(s->plan,n->left);
        size_t index=leaf>=0 && s->plan->nodes[leaf].ordered_index?QS_INDEX_WORK_BYTES+sizeof(struct qs_plan):0;
        reserve+=s->plan->workers*(sizeof(*s)+s->plan->count*sizeof(*s->nodes)+sizeof(struct qs_plan)+index+256);
    }
    return reserve;
}
/* A parent must not reserve a large estimated group table while a blocking
 * child still needs to build/materialize its input. Start small there and grow
 * from real input after those dependencies run; all bytes share one context. */
static int group_input_pending(const struct execution *s,int id)
{
    if(id<0) return 0;
    const struct qp_node *n=&s->plan->nodes[id]; const struct operator_state *o=&s->nodes[id];
    if(n->kind==QP_AGGREGATE && n->nkeys) return !o->finished;
    if(n->kind==QP_SORT && !o->left_sort.drained) return 1;
    if(n->kind==QP_MATERIALIZED) return 1;
    return group_input_pending(s,n->left) || group_input_pending(s,n->right);
}
static int64_t hash_aggregate_next_impl(struct execution *s,unsigned id,struct qs_cell *row,int prepare,int raw)
{
    struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    struct sorted_rows *sort=&o->left_sort;
    if(!qp_hash_aggregate_supported(n)) { n->hash=0; return prepare?3:aggregate_next_impl(s,id,row,raw); }
    int wide=n->nkeys!=1 || n->expr.exprs[n->expr.keys[0]].type!=Q_INTEGER;
    if(!o->group_capacity) {
        /* Keep the insertion counter away from read-mostly capacity/stride
         * fields. The root context owns this small aligned allocation until
         * query close; table resize does not move it or change its count. */
        if(!o->group_count) {
            void *storage=cs_work_alloc(s->context,128);
            if(!storage) { n->hash=0; return prepare?3:aggregate_next_impl(s,id,row,raw); }
            o->group_count=(void *)(((uintptr_t)storage+63)&~(uintptr_t)63);
            atomic_init(o->group_count,0);
        }
        struct cs_work_context_stats memory;
        if(cs_work_get_context_stats(s->context,&memory)) return -4;
        size_t available=memory.limit_bytes-memory.used_bytes,reserve=group_reserve(s,n);
        available=available>reserve?available-reserve:available/2;
        o->group_integer=1;
        /* Runtime strings are canonical dictionary IDs, also 64-bit keys. */
        for(unsigned k=0;k<n->nkeys;++k) {
            unsigned type=n->expr.exprs[n->expr.keys[k]].type;
            if(type!=Q_INTEGER && type!=Q_STRING) o->group_integer=0;
        }
        o->group_stride=sizeof(*o->groups)+n->expr.naggs*sizeof(struct group_value)+
            (wide?n->nkeys*(o->group_integer?sizeof(int64_t):sizeof(struct qs_cell)):0);
        o->group_stride=(o->group_stride+15)&~15U;
        unsigned capacity=128;
        /* PG's pre-HAVING group estimate is distinct from output cardinality.
         * Use it for wide tables too, within the real remaining budget and
         * spill/worker reserve, instead of serializing waves from 128 buckets.
         * Legacy plans without that estimate retain the conservative path. */
        int key_only=n->expr.aggs[0].kind==Q_COUNT_STAR && n->expr.nout==n->nkeys;
        for(unsigned i=0;key_only && i<n->expr.nout;++i)
            if(n->expr.exprs[n->expr.outputs[i].index].op!=Q_COLUMN) key_only=0;
        double groups=n->estimated_groups>0?n->estimated_groups:n->estimated_rows;
        size_t bytes_per_slot=o->group_stride-o->group_stride/8+sizeof(struct group_entry);
        while(!group_input_pending(s,n->left) &&
              (!wide || n->estimated_groups>0 || s->plan->nodes[n->left].kind==QP_SCAN ||
              (key_only && s->plan->nodes[n->left].parallel)) && capacity<groups*8/7 &&
              (size_t)capacity*2*bytes_per_slot<=available && capacity<UINT_MAX/2)
            capacity*=2;
        for(;;) {
            /* Only the compact lookup array needs clearing. New dense
             * records are initialized by the CU that publishes each key.
             * The 7/8 load limit also bounds the number of stored records. */
            o->groups=cs_work_alloc(s->context,(size_t)(capacity-capacity/8)*o->group_stride);
            o->group_entries=o->groups?cs_work_calloc(s->context,capacity,sizeof(*o->group_entries)):NULL;
            if(o->group_entries) break;
            cs_work_free(s->context,o->groups); o->groups=NULL;
            if(capacity==128) break;
            capacity/=2;
        }
        if(!o->groups) { n->hash=0; return prepare?3:aggregate_next_impl(s,id,row,raw); }
        o->group_capacity=capacity;
    }
    for(;;) {
        if(o->group_phase==0) {
            if(!o->have_left) {
                if(o->group_locals && !s->parallel_started) {
                    if(!group_local_next(s,o)) continue;
                    o->have_left=o->group_partial=1;
                } else {
                    int64_t status=next_row(s,n->left,o->left);
                    if(status==QP_WAIT || status<0 || status>2) return status;
                    o->have_left=status==2;
                    if(!status) {
                        if(o->group_locals) continue;
                        o->input_done=1; o->group_phase=1; continue;
                    }
                }
            }
            int added;
            if(o->group_partial) {
                struct operator_state *local=o->group_locals[o->group_local_slot];
                struct compact_group *g=group_at(local,o->group_local_row); struct qs_cell keys[QS_KEYS];
                group_keys(n,local,g,keys); added=group_sink_values(s,id,o,keys,g->values,0);
            } else added=group_sink_row(s,id,o,o->left,0);
            if(added<0) return added;
            if(!added) {
                unsigned capacity=o->group_capacity*2;
                struct cs_work_context_stats m;
                if(cs_work_get_context_stats(s->context,&m)) return -4;
                size_t available=m.limit_bytes-m.used_bytes,reserve=group_reserve(s,n);
                /* Keep spill/worker space after replacing the old buffers.
                 * Realloc may grow in place; the allocator, rather than a
                 * pessimistic quota, enforces the actual transient peak. */
                size_t bytes_per_slot=o->group_stride-o->group_stride/8+sizeof(struct group_entry);
                size_t released=(size_t)o->group_capacity*bytes_per_slot;
                available+=released;
                available=available>reserve?available-reserve:0;
                struct group_entry *entries=capacity>o->group_capacity &&
                    (size_t)capacity*bytes_per_slot<=available?
                    cs_work_calloc(s->context,capacity,sizeof(*entries)):NULL;
                void *larger=entries?cs_work_realloc(s->context,o->groups,(size_t)(capacity-capacity/8)*o->group_stride):NULL;
                if(larger) {
                    struct group_entry *old=o->group_entries;
                    o->groups=larger; o->group_capacity=capacity;
                    o->group_entries=entries;
                    unsigned rows=atomic_load_explicit(o->group_count,memory_order_relaxed);
                    for(unsigned i=0;i<rows;++i) {
                        const struct compact_group *v=group_at(o,i);
                        uint64_t hash=wide?(uint64_t)v->key:mix_hash(v->key_flags==2?0:(uint64_t)v->key);
                        unsigned slot=hash&(capacity-1);
                        while(atomic_load_explicit(&entries[slot].state,memory_order_relaxed)) slot=(slot+1)&(capacity-1);
                        entries[slot].row=i;
                        atomic_store_explicit(&entries[slot].state,group_entry_state(v->key_flags,hash),memory_order_relaxed);
                    }
                    cs_work_free(s->context,old); continue;
                }
                cs_work_free(s->context,entries);
                o->group_phase=1; continue;
            }
            if(o->group_partial) { ++o->group_local_row; o->group_partial=0; }
            o->have_left=0; continue;
        }
        if(o->group_phase==1) {
            if(o->input_done && !o->hash_fallback) { o->group_phase=3; continue; }
            o->hash_fallback=1;
            struct qs_plan partial={.nout=n->nkeys+2*n->expr.naggs}; int columns[QS_KEYS];
            for(unsigned k=0;k<n->nkeys;++k) {
                columns[k]=k; partial.outputs[k]=s->plan->nodes[n->left].expr.outputs[n->left_keys[k]];
            }
            for(unsigned a=0;a<n->expr.naggs;++a) {
                /* Internal values, including canonical text IDs, stay cells;
                 * final output alone converts string IDs to exported text. */
                partial.outputs[n->nkeys+2*a].type_oid=1700;
                partial.outputs[n->nkeys+2*a+1].type_oid=20;
            }
            int64_t status=sort_init(s,sort,&partial,n->nkeys,columns,NULL); if(status) return status;
            if(sort->input_pending) {
                status=sort_call(s,sort,0); if(status) return status;
                sort->input_pending=0; sort->input->rows=0;
            }
            while(o->group_flush<atomic_load_explicit(o->group_count,memory_order_relaxed) && sort->input->rows<QS_RESULT_ROWS) {
                unsigned slot=o->group_flush++;
                struct compact_group *g=group_at(o,slot);
                struct qs_cell *out=sort->input->data[sort->input->rows++].cells;
                group_keys(n,o,g,out);
                for(unsigned a=0;a<n->expr.naggs;++a) {
                    out[n->nkeys+2*a]=g->values[a].sum;
                    out[n->nkeys+2*a+1]=(struct qs_cell){.value=g->values[a].count};
                }
            }
            if(sort->input->rows) { sort->input_pending=1; continue; }
            if(o->input_done) {
                cs_work_free(s->context,o->groups); o->groups=NULL;
                cs_work_free(s->context,o->group_entries); o->group_entries=NULL;
                sort->complete=1; sort->group.count=1; sort->group.states[0]=sort->task.state;
                cs_work_free(s->context,sort->input); sort->input=NULL;
                o->group_phase=2;
            } else {
                memset(o->group_entries,0,(size_t)o->group_capacity*sizeof(*o->group_entries));
                atomic_store_explicit(o->group_count,0,memory_order_relaxed);
                o->group_flush=0; o->group_phase=0;
            }
            continue;
        }
        if(o->group_phase==3) {
            if(prepare) return 3;
            while(o->group_flush<atomic_load_explicit(o->group_count,memory_order_relaxed)) {
                unsigned slot=o->group_flush++;
                struct compact_group *g=group_at(o,slot);
                struct qs_cell keys[QS_KEYS]; group_keys(n,o,g,keys);
                int64_t result=group_result(s,n,g->values,keys,row,raw); if(result) return result;
            }
            cs_work_free(s->context,o->groups); o->groups=NULL;
            cs_work_free(s->context,o->group_entries); o->group_entries=NULL; o->finished=1; return 0;
        }
        if(prepare) return 3;
        /* Merge partial value/count pairs; AVG is divided only for final output. */
        if(!o->have_right && !o->group_ready) {
            if((s->pending_sort==sort || sort->position==sort->output->rows) && !sort->drained) {
                int64_t status=sort_call(s,sort,1); if(status) return status;
                sort->position=0; sort->drained=sort->output->finished;
            }
            if(sort->position<sort->output->rows) {
                memcpy(o->right,sort->output->data[sort->position++].cells,(n->nkeys+2*n->expr.naggs)*sizeof(*o->right)); o->have_right=1;
            } else o->group_ready=1;
        }
        if(!o->phase) {
            if(!o->have_right) { cs_work_free(s->context,sort->output); sort->output=NULL; o->finished=1; return 0; }
            memcpy(o->keys,o->right,n->nkeys*sizeof(*o->keys));
            memset(o->accum,0,sizeof(o->accum));
            for(unsigned a=0;a<n->expr.naggs;++a) o->accum[a].sum.isnull=1;
            o->phase=1;
        }
        struct group_value groups[QS_AGGS];
        for(unsigned a=0;a<n->expr.naggs;++a)
            groups[a]=(struct group_value){.sum=o->accum[a].sum,.count=o->accum[a].count};
        int equal=o->have_right?group_equal(s,n,o->right,o->keys):0; if(equal<0) return equal;
        if(equal) {
            for(;o->aggregate_position<n->expr.naggs;++o->aggregate_position) {
                unsigned a=o->aggregate_position;
                int status=group_update(s,n,a,&groups[a],o->right[n->nkeys+2*a],o->right[n->nkeys+2*a+1].value);
                if(status) return status;
                o->accum[a].sum=groups[a].sum; o->accum[a].count=groups[a].count;
            }
            o->aggregate_position=0; o->have_right=0; continue;
        }
        o->phase=0;
        int64_t result=group_result(s,n,groups,o->keys,row,raw); if(result) return result;
    }
}
static int64_t hash_aggregate_next(struct execution *s,unsigned id,struct qs_cell *row,int raw)
{
    /* Pure in-memory output is one call per group: leave it untimed. */
    if(!s->profiling || s->nodes[id].group_phase==3) return hash_aggregate_next_impl(s,id,row,0,raw);
    unsigned op=s->nodes[id].group_phase==2?QS_AGG_SOURCE:QS_AGG_SINK;
    struct blocking_timer timer=blocking_begin(s);
    int64_t status=hash_aggregate_next_impl(s,id,row,0,raw); blocking_end(s,op,timer); return status;
}
/* Consume a contiguous key range from expression columns. The next group's
 * first row stays in the batch; the current accumulators survive I/O and
 * vector boundaries. Return 2 for a completed group, 3 for scalar fallback. */
static int64_t stream_group_vectors(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    for(;;) {
        int64_t status=projection_fill(s,id,0);
        if(status!=2) {
            if(!status) { o->input_done=1; return o->phase?2:0; }
            return status;
        }
        struct projection_batch *b=o->projection;
        unsigned first=b->position,end=first;
        while(end<b->count) {
            struct qs_cell keys[QS_KEYS];
            for(unsigned k=0;k<n->nkeys;++k) {
                int index=b->group_keys[k]; if(b->vectors[index].errors[end]) return -5;
                keys[k]=qs_vector_cell(&b->plan,b->vectors,index,end);
            }
            if(!o->phase) {
                for(unsigned col=0;col<b->plan.nout;++col) {
                    int index=b->plan.outputs[col].index; if(b->vectors[index].errors[end]) return -5;
                    o->first[col]=qs_vector_cell(&b->plan,b->vectors,index,end);
                }
                memcpy(o->keys,keys,n->nkeys*sizeof(*keys));
                memset(o->accum,0,sizeof(o->accum)); o->phase=1;
            } else {
                int equal=group_equal(s,n,keys,o->keys); if(equal<0) return equal;
                if(!equal) break;
            }
            ++end;
        }
        if(end==first) return 2;
        for(unsigned a=b->aggregate_position;a<n->expr.naggs;++a,b->aggregate_position=a) {
            const struct qs_agg *agg=&n->expr.aggs[a];
            if(agg->kind==Q_COUNT_STAR) { o->accum[a].count+=end-first; continue; }
            int index=b->group_values[a];
            if(agg->kind==Q_COUNT && b->vectors[index].all_valid) {
                o->accum[a].count+=end-first; continue;
            }
            for(unsigned r=b->aggregate_row?b->aggregate_row:first;r<end;++r,b->aggregate_row=r) {
                if(b->vectors[index].errors[r]) return -5;
                struct qs_cell value=qs_vector_cell(&b->plan,b->vectors,index,r);
                if(!value.isnull) { int added=accumulate_value(s,n,agg,&o->accum[a],value); if(added) return added; }
            }
            b->aggregate_row=0;
        }
        b->aggregate_position=0;
        b->position=end; s->metrics.matched+=end-first;
        if(end<b->count) return 2;
    }
}
static int64_t aggregate_next_impl(struct execution *s,unsigned id,struct qs_cell *row,int raw)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    if(o->finished) return 0;
    if(n->hash) return hash_aggregate_next(s,id,row,raw);
    if(n->nkeys && n->left>=0 && !n->sorted_input && !n->distinct && !o->phase && !o->emitted &&
       input_ordered(s->plan,n->left,n->nkeys,n->left_keys,NULL)) s->plan->nodes[id].sorted_input=1;
    if(n->nkeys && n->sorted_input && !n->distinct && !n->distinct_aggs && !o->input_done && !o->have_left &&
       n->left>=0 && !(!s->worker && s->plan->workers>1 && s->plan->nodes[n->left].parallel)) {
        int status=projection_init(s,id,0); if(status) return status;
    }
    /* Initialize before starting the child pipeline: parallel workers combine
     * their local states here before the first (and only) output row. */
    if(!n->nkeys && !o->phase && !o->emitted) {
        memset(o->accum,0,sizeof(o->accum)); o->phase=1;
    }
    if(!n->nkeys && !n->distinct && !n->distinct_aggs && !o->input_done && !o->emitted && n->left>=0 &&
       !(!s->worker && s->plan->workers>1 && s->plan->nodes[n->left].parallel)) {
        int64_t status=projection_init(s,id,0); if(status) return status;
        while(o->projection) {
            status=projection_fill(s,id,0);
            if(!status) { o->input_done=1; break; }
            if(status==3) break; /* Existing scalar fallback retains its cursor. */
            if(status!=2) return status;
            struct blocking_timer timer={0}; if(s->profiling) timer=blocking_begin(s);
            status=aggregate_sink_vectors(s,id);
            if(s->profiling) blocking_end(s,QS_AGG_SINK,timer);
            if(status) return status;
        }
    }
    for(;;) {
        int ready=0;
        if(n->nkeys && o->projection) {
            /* One call per output group: use CSF/enclosing sink timings,
             * rather than adding clock reads for every small group. */
            int64_t status=stream_group_vectors(s,id);
            if(status!=2 && status!=3) return status;
            ready=status==2;
        }
        if(!ready && !o->have_left && !o->input_done) {
            int64_t status;
            if(n->nkeys && (!n->sorted_input || n->distinct)) {
                int keys[QS_KEYS+1]; memcpy(keys,n->left_keys,n->nkeys*sizeof(*keys)); unsigned count=n->nkeys;
                if(n->distinct) keys[count++]=n->distinct-1;
                status=sorted_input(s,&o->left_sort,n->left,count,keys,NULL,o->left);
            } else status=next_row(s,n->left,o->left);
            if(status==QP_WAIT || status<0 || status>2) return status;
            o->have_left=status==2; o->input_done=!status;
        }
        if(!o->phase) {
            if(!o->have_left && (n->nkeys || o->emitted)) return 0;
            memset(o->accum,0,sizeof(o->accum));
            if(o->have_left) {
                memcpy(o->first,o->left,sizeof(o->first));
                int status=initialize_group_keys(s,n,o); if(status) return status;
            }
            o->phase=1;
        }
        if(o->have_left) {
            int same=o->aggregate_input && o->aggregate_input->active==3?1:group_same(s,n,o); if(same<0) return -5;
            if(same) {
                int status=accumulate(s,n,o); if(status) return status;
                o->have_left=0; continue;
            }
        }
        struct qs_cell values[QS_AGGS];
        for(unsigned i=0;i<n->expr.naggs;++i) {
            const struct qs_agg *agg=&n->expr.aggs[i]; const struct accumulator *a=&o->accum[i];
            struct qs_cell v={.isnull=!a->count};
            if(agg->kind==Q_COUNT || agg->kind==Q_COUNT_STAR) v=(struct qs_cell){.value=a->count};
            else if(a->count) {
                v=agg->kind==Q_MIN || agg->kind==Q_MAX?a->extreme:a->sum;
                if(agg->kind==Q_AVG && qs_average(v,a->count,&v)) return -5;
            }
            values[i]=v;
        }
        o->phase=0; ++o->emitted;
        if(raw) {
            memcpy(row,o->keys,n->nkeys*sizeof(*row));
            memcpy(row+n->nkeys,values,n->expr.naggs*sizeof(*row)); return 2;
        }
        int count=project(s,n,o->first,values,row); if(count<0) return count;
        if(count) return 2;
    }
}
static int64_t aggregate_next(struct execution *s,unsigned id,struct qs_cell *row)
{ return aggregate_next_impl(s,id,row,0); }

/* Import the indexed side's expressions once, with columns referencing the
 * decoded match chunk (its first two slots are lookup key and source row). */
static void index_projection_init(struct execution *s,unsigned id)
{
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    int useful=right->expr.predicate>=0;
    for(unsigned i=0;i<right->expr.nout;++i)
        useful|=right->expr.exprs[right->expr.outputs[i].index].op!=Q_COLUMN;
    if(!useful) return;
    struct projection_batch *b=cs_work_calloc(s->context,1,sizeof(*b)); if(!b) return;
    b->plan=right->expr; b->plan.nexprs=0; b->plan.predicate=-1; b->plan.ncols+=2;
    for(unsigned i=0;i<b->plan.nattrs;++i) if(b->plan.cols[i].slot>=0) b->plan.cols[i].slot+=2;
    struct projection_import import={.execution=s,.batch=b};
    for(unsigned i=0;i<b->plan.nout;++i) {
        int index=projection_import(&import,n->right,right->expr.outputs[i].index);
        if(index<0 || qs_vector_require(&b->plan,index,b->needed)) goto fallback;
        b->plan.outputs[i].index=index;
    }
    /* Initialize output dependencies but evaluate them only for survivors. */
    for(unsigned i=0;i<b->plan.nexprs;++i) b->needed[i]&=1;
    if(right->expr.predicate>=0) {
        int index=projection_import(&import,n->right,right->expr.predicate);
        if(index<0 || qs_vector_require(&b->plan,index,b->needed)) goto fallback;
        b->plan.predicate=index;
    }
    b->vectors=cs_work_alloc(s->context,b->plan.nexprs*sizeof(*b->vectors));
    if(!b->vectors) goto fallback;
    s->nodes[id].index_projection=b; return;
fallback:
    cs_work_free(s->context,b);
}
static int index_project_batch(struct execution *s,struct projection_batch *b,struct qs_chunk *chunk)
{
    if(b->materializing) return materialize_outputs(s,b);
    b->count=chunk->selected;
    struct qs_vector_input input={.dictionary=s->dictionary,.parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    if(s->paged) { input.text=s->text; input.text_io=s->io; input.resume=b->retry; }
    int status=0;
    if(qs_vector_execute_input(&b->plan,chunk,qs_selection(chunk),0,chunk->selected,b->needed,b->vectors,input)) { status=-5; goto finished; }
    uint16_t active[QS_VECTOR_ROWS]; unsigned count=0;
    for(unsigned r=0;r<chunk->selected;++r) {
        int index=b->plan.predicate;
        if(index>=0) {
            if(b->vectors[index].errors[r]) continue;
            struct qs_cell keep=qs_vector_cell(&b->plan,b->vectors,index,r);
            if(keep.isnull || !keep.value) continue;
        }
        active[count++]=r;
    }
    input.plan=&b->plan; input.vectors=b->vectors; input.count=chunk->selected;
    for(unsigned i=0;i<b->plan.nout;++i)
        if(qs_vector_evaluate(&input,b->plan.outputs[i].index,active,count)) { status=-5; break; }
finished:
    if(status && input.text) text_status(s,qs_dictionary_text_request(input.text));
    b->retry=status && s->expression_wait;
    if(status) return status;
    return s->paged?materialize_outputs(s,b):0;
}
/* Decode one candidate in the original consumption order. A zero result is
 * rejected by the indexed side; negative results remain deferred per row. */
static int index_join_values(struct execution *s,unsigned id,struct qs_chunk *chunk,unsigned r,
    const struct projection_batch *projected,struct qs_cell *values)
{
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    struct operator_state *o=&s->nodes[id]; unsigned ln=s->plan->nodes[n->left].expr.nout;
    unsigned at=qs_selection(chunk)[r]; if(at>=chunk->rows) return -1;
    unsigned source=qs_values(chunk)[chunk->capacity+at]; if(source>=o->index_count) return -1;
    memcpy(values,o->index_left+(size_t)source*ln,ln*sizeof(*values));
    if(projected) {
        int predicate=projected->plan.predicate;
        if(predicate>=0) {
            if(projected->vectors[predicate].errors[r]) return -5;
            struct qs_cell keep=qs_vector_cell(&projected->plan,projected->vectors,predicate,r);
            if(keep.isnull || !keep.value) return 0;
        }
        for(unsigned i=0;i<right->expr.nout;++i) {
            unsigned index=projected->plan.outputs[i].index;
            const struct qs_vector *v=&projected->vectors[index];
            if(v->errors[r]) return -5;
            values[ln+i]=qs_vector_cell(&projected->plan,projected->vectors,index,r);
            if(v->kind==QS_TEXT && !values[ln+i].isnull) {
                unsigned text; if(intern_view(s,v->strings[r],&text)) return -3;
                values[ln+i].value=text;
            }
        }
    } else {
        struct qs_cell raw[QS_COLS];
        for(unsigned col=0;col<right->expr.nattrs;++col) if(right->expr.cols[col].slot>=0) {
            unsigned slot=2+right->expr.cols[col].slot;
            raw[col]=(struct qs_cell){.value=qs_values(chunk)[(size_t)slot*chunk->capacity+at],
                .scale=right->expr.cols[col].scale,.dscale=qs_dscales(chunk)[(size_t)slot*chunk->capacity+at],
                .isnull=(qs_nulls(chunk)[(size_t)slot*((chunk->capacity+7)/8)+at/8]>>(at%8))&1};
        }
        return project(s,right,raw,NULL,values+ln);
    }
    return 1;
}
static int index_qual_prepare(struct execution *s,unsigned id,struct qs_chunk *chunk,
    const struct projection_batch *projected,const struct projection_batch *output)
{
    if(chunk->selected>QS_VECTOR_ROWS) return -1;
    struct operator_state *o=&s->nodes[id]; struct index_qual_batch *q=o->index_qual;
    if(!q) {
        if(o->index_qual_checked) return 0;
        o->index_qual_checked=1;
        q=cs_work_calloc(s->context,1,sizeof(*q)); if(!q) return 0;
        if(join_qual_init(s,id,&q->expr,output)) { cs_work_free(s->context,q); return 0; }
        for(unsigned i=0;i<q->expr.plan.nexprs;++i) q->expr.needed[i]&=1;
        o->index_qual=q;
    }
    struct projection_batch *b=&q->expr;
    uint16_t active[QS_VECTOR_ROWS]; unsigned count=0;
    if(!q->preparing) { q->preparing=1; q->prepared=0; memset(q->status,0,chunk->selected*sizeof(*q->status)); }
    for(;q->prepared<chunk->selected;++q->prepared) {
        unsigned r=q->prepared;
        struct qs_cell values[QS_COLS];
        int status=index_join_values(s,id,chunk,r,projected,values);
        if(s->expression_wait) return status;
        q->status[r]=status;
        for(unsigned col=0;col<b->plan.nattrs;++col) {
            int slot=b->plan.cols[col].slot;
            if(slot>=0) b->cells[(size_t)slot*QS_VECTOR_ROWS+r]=status>0?values[col]:(struct qs_cell){.isnull=1};
        }
        if(status<0) {
            for(unsigned col=0;col<b->plan.ncols;++col) for(unsigned rest=r+1;rest<chunk->selected;++rest)
                b->cells[(size_t)col*QS_VECTOR_ROWS+rest]=(struct qs_cell){.isnull=1};
            q->prepared=chunk->selected; break;
        }
    }
    for(unsigned r=0;r<chunk->selected;++r) {
        if(q->status[r]<0) break;
        if(q->status[r]) active[count++]=r;
    }
    struct qs_vector_input input={.cells=b->cells,.dictionary=s->dictionary,
        .parameters=s->params,.nparameters=QP_PARAMS,.inset=inset_vectors,.set_state=s};
    if(s->paged) { input.text=s->text; input.text_io=s->io; input.resume=b->retry; }
    int status=qs_vector_execute_input(&b->plan,NULL,NULL,0,chunk->selected,b->needed,b->vectors,input);
    input.plan=&b->plan; input.vectors=b->vectors; input.count=chunk->selected;
    if(!status) status=qs_vector_evaluate(&input,b->plan.predicate,active,count);
    if(status && input.text) text_status(s,qs_dictionary_text_request(input.text));
    b->retry=status && s->expression_wait;
    return status?-5:1;
}
/* index_scan.c calls this consumer with a batch of decoded heap matches.
 * Only the lookup key is narrowed to int64; left payloads keep 128-bit cells. */
int64_t qs_join_consume(unsigned stage,struct qs_chunk *chunk,const struct qs_plan *unused,
    struct qs_task *task,struct qs_join_control *control,struct femu_csd_args *args,int resume)
{
    (void)stage; (void)unused; (void)task; (void)control; (void)args; (void)resume;
    struct execution *s=index_execution; unsigned id=index_node;
    const struct qp_node *n=&s->plan->nodes[id];
    struct operator_state *o=&s->nodes[id];
    struct projection_batch *b=index_columns;
    if(o->index_flush) { o->index_flush=0; return 0; }
    if(!o->index_active) {
        o->index_active=1; o->index_consume_position=0; o->index_projected=0;
        if(!b) { o->index_output->rows=0; o->index_position=0; }
    }
    struct projection_batch *projected=chunk->selected>1?o->index_projection:NULL;
    if(projected && !o->index_projected) {
        int status=index_project_batch(s,projected,chunk); if(status) return status;
        o->index_projected=1;
    }
    struct index_qual_batch *qualified=NULL;
    if(b && n->join_qual>=0 && chunk->selected>1) {
        int status=index_qual_prepare(s,id,chunk,projected,b); if(status<0) return status;
        if(status) qualified=o->index_qual;
    }
    /* The Index cursor retains this batch until the output is consumed.
     * SEMI gathers across rounds, so it keeps its existing owned buffer. */
    int borrow=qualified && !b->input_count && (n->join_type==QP_INNER || n->join_type==QP_LEFT);
    for(;o->index_consume_position<chunk->selected;++o->index_consume_position) {
        unsigned r=o->index_consume_position;
        unsigned at=qs_selection(chunk)[r]; struct qs_cell values[QS_COLS];
        unsigned source=qs_values(chunk)[chunk->capacity+at];
        if(source>=o->index_count) return -1;
        struct qs_cell match={.value=1};
        if(qualified) {
            int status=qualified->status[r]; if(status<0) return status; if(!status) continue;
            const struct projection_batch *q=&qualified->expr; unsigned index=q->plan.predicate;
            if(q->vectors[index].errors[r]) return -5;
            match=qs_vector_cell(&q->plan,q->vectors,index,r);
            if(match.isnull || !match.value) continue;
            for(unsigned col=0;!borrow && col<q->plan.nattrs;++col) {
                int slot=q->plan.cols[col].slot;
                if(slot>=0) values[col]=q->cells[(size_t)slot*QS_VECTOR_ROWS+r];
            }
        } else {
            if(o->project_retry) memcpy(values,o->project_retry->input,n->expr.nattrs*sizeof(*values));
            else {
                if(o->join_condition && o->join_condition->active)
                    memcpy(values,o->join_condition->input,n->expr.nattrs*sizeof(*values));
                else {
                    int status=index_join_values(s,id,chunk,r,projected,values); if(status<0) return status; if(!status) continue;
                }
                if(n->join_qual>=0 && join_condition(s,id,values,&match)) return -5;
            }
        }
        if(match.isnull || !match.value) continue;
        uint64_t bit=UINT64_C(1)<<(source%64);
        uint64_t *matches=&o->index_matches[source/64];
        if(n->join_type==QP_SEMI && (*matches&bit)) continue;
        if(n->join_type==QP_ANTI) { *matches|=bit; ++s->metrics.join_rows; continue; }
        if(b) {
            /* Keep only columns needed by the fused expression DAG. The
             * index cursor retains the decoded chunk until this batch drains. */
            if(borrow) {
                b->borrowed_cells=qualified->expr.cells;
                b->borrowed_selection=qualified->selection;
                qualified->selection[b->input_count++]=r;
            } else projection_append(b,values);
        } else {
            int status=project(s,n,values,NULL,o->index_output->data[o->index_output->rows].cells);
            if(status<0) return status;
            if(status) ++o->index_output->rows;
        }
        *matches|=bit; ++s->metrics.join_rows;
    }
    o->index_active=0; o->index_consume_position=0;
    if(o->index_qual) o->index_qual->preparing=o->index_qual->prepared=0;
    if(n->join_type==QP_SEMI || n->join_type==QP_ANTI) qs_index_skip(&o->index_control,0,o->index_matches);
    /* SEMI candidates arrive in rounds over unresolved lookup keys. Gather
     * accepted left rows until the lookup batch finishes, preserving its
     * early match skip without running the output DAG per candidate. */
    unsigned rows=b?b->input_count:o->index_output->rows;
    if(rows && (!b || n->join_type!=QP_SEMI)) {
        o->index_flush=1; return QP_INDEX_YIELD;
    }
    return 0;
}
static int64_t index_next(struct execution *s,unsigned id,struct qs_cell *row,struct projection_batch *b)
{
    const struct qp_node *n=&s->plan->nodes[id],*right=&s->plan->nodes[n->right];
    struct operator_state *o=&s->nodes[id]; unsigned ln=s->plan->nodes[n->left].expr.nout;
    if(!o->index_plan) {
        struct qs_plan *p=cs_work_calloc(s->context,1,sizeof(*p));
        if(!b) o->index_output=cs_work_calloc(s->context,1,sizeof(*o->index_output));
        o->index_left=cs_work_alloc(s->context,QS_INDEX_KEYS*ln*sizeof(*o->index_left));
        if(!p || (!b && !o->index_output) || !o->index_left) return -4;
        o->index_plan=p; p->nattrs=right->expr.nattrs+2; p->ncols=right->expr.ncols+2; p->njoins=1;
        p->work_mem_parent=(uintptr_t)s->context;
        p->join=right->expr.join; p->join.relid=right->expr.relid; p->join.relfilenode=right->expr.relfilenode;
        p->join.nattrs=right->expr.nattrs;
        p->join.semi=n->join_type==QP_SEMI || n->join_type==QP_ANTI?(b?-2:-1):0;
        p->join.probe_key=0; p->join.predicate=-1; p->join.work_mem_bytes=QS_INDEX_WORK_BYTES;
        ++p->join.build_key;
        p->cols[0]=(struct qs_column){.slot=0,.type=Q_INTEGER,.length=8,.align=8};
        p->cols[1]=p->cols[0]; p->cols[1].slot=1;
        for(unsigned i=0;i<right->expr.nattrs;++i) {
            p->cols[i+2]=right->expr.cols[i];
            if(p->cols[i+2].slot>=0) p->cols[i+2].slot+=2;
        }
        int status=qs_index_init(p,&o->index_control); if(status) return status;
        if(s->paged) { int bound=qs_index_paged_dictionary(&o->index_control,s->paged); if(bound) return bound; }
        else qs_index_dictionary(&o->index_control,s->dictionary);
        index_projection_init(s,id);
        struct qs_chunk *input=(void *)(s->arena+vector_offset(s,n)),*output=(void *)(s->arena+vector_offset(s,n)+qs_index_input_bytes());
        *input=(struct qs_chunk){.capacity=QS_INDEX_KEYS,.ncols=2};
        *output=(struct qs_chunk){.capacity=256,.ncols=p->ncols};
    }
    for(;;) {
        if(!b && !o->index_active) {
            if(o->index_position<o->index_output->rows) {
                memcpy(row,o->index_output->data[o->index_position++].cells,n->expr.nout*sizeof(*row)); return 2;
            }
            o->index_output->rows=0; o->index_position=0;
        }
        if(o->phase==2) {
            while(o->index_unmatched<o->index_count) {
                unsigned at=o->index_unmatched++;
                if((n->join_type!=QP_LEFT && n->join_type!=QP_ANTI) ||
                   (o->index_matches[at/64]&(UINT64_C(1)<<(at%64)))) continue;
                struct qs_cell values[QS_COLS]; memcpy(values,o->index_left+(size_t)at*ln,ln*sizeof(*values));
                for(unsigned i=0;i<right->expr.nout;++i) values[ln+i]=(struct qs_cell){.isnull=1};
                if(b) {
                    projection_append(b,values);
                } else {
                    int count=project(s,n,values,NULL,row); if(count<0) return count; if(count) return 2;
                }
            }
            o->index_count=0; o->phase=0;
            if(b && b->input_count) return 2;
        }
        if(!o->phase) {
            unsigned batch=n->sorted_input?1:QS_INDEX_KEYS;
            while(o->index_count<batch && !o->input_done) {
                int64_t status=next_row(s,n->left,o->index_left+(size_t)o->index_count*ln);
                if(status==QP_WAIT || status<0 || status>2) return status;
                if(!status) o->input_done=1; else ++o->index_count;
            }
            if(!o->index_count) {
                if(o->index_qual) {
                    cs_work_free(s->context,o->index_qual->expr.cells);
                    cs_work_free(s->context,o->index_qual->expr.vectors);
                    cs_work_free(s->context,o->index_qual); o->index_qual=NULL;
                }
                o->finished=1; return 0;
            }
            memset(o->index_matches,0,sizeof(o->index_matches));
            o->index_resume=0; o->index_flush=0; o->index_unmatched=0; o->phase=1;
            struct qs_chunk *input=(void *)(s->arena+vector_offset(s,n));
            input->rows=input->selected=o->index_count; memset(qs_nulls(input),0,2*((QS_INDEX_KEYS+7)/8));
            for(unsigned i=0;i<o->index_count;++i) {
                struct qs_cell key=o->index_left[(size_t)i*ln+n->left_keys[0]];
                if(key.value>INT64_MAX || key.value<INT64_MIN) return -5;
                qs_values(input)[i]=key.value; qs_values(input)[QS_INDEX_KEYS+i]=i;
                if(key.isnull) qs_nulls(input)[i/8]|=1U<<(i%8);
                qs_selection(input)[i]=i;
            }
        }
        if(s->control->ready && s->control->node==id && s->control->index_nreads) {
            s->control->ready=s->control->index_nreads=0;
        }
        struct qs_plan *p=o->index_plan;
        void *mr[]={s->arena+vector_offset(s,n),p,&o->index_task,&o->index_control,
            s->arena+vector_offset(s,n)+qs_index_input_bytes(),s->arena+input_offset(s,n)};
        long long len[]={qs_chunk_bytes(QS_INDEX_KEYS,2),sizeof(*p),sizeof(o->index_task),sizeof(o->index_control),
            qs_chunk_bytes(256,p->ncols),QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE};
        struct femu_csd_args a={.numr=6,.mr_addr=mr,.mr_len=len,.cparam2=s->args->cparam2};
        if(s->paged) qs_index_spill_io(&o->index_control,0,s->io);
        /* A self-join can reuse the input pages already retained by its scan.
         * This is the current morsel only; advancing the scan updates the window. */
        int scan=qp_probe_scan(s->plan,n->left);
        if(scan>=0 && right->expr.relid && s->plan->nodes[scan].expr.relid==right->expr.relid &&
           s->plan->nodes[scan].expr.relfilenode==right->expr.relfilenode && s->nodes[scan].position) {
            uint64_t end=s->nodes[scan].position,begin=(end-1)/s->plan->scan_bytes*s->plan->scan_bytes;
            qs_index_heap_window(&o->index_control,0,s->arena+input_offset(s,&s->plan->nodes[scan]),begin/QS_PAGE,(end-begin)/QS_PAGE);
        }
        index_execution=s; index_node=id; index_columns=b;
        int64_t status=qs_index_probe(0,mr[0],p,&o->index_task,&o->index_control,&a,o->index_resume);
        index_columns=NULL; o->index_resume=1;
        s->dictionary_wait=s->paged && qs_index_dictionary_pending(&o->index_control,0)?status:0;
        if(status==QP_INDEX_YIELD) { if(b) return 2; continue; }
        if(status==QS_INDEX_WAIT && o->index_task.index_pending) {
            s->control->node=id; s->control->index_nreads=o->index_task.index_nreads;
            memcpy(s->control->index_reads,o->index_task.index_reads,s->control->index_nreads*sizeof(s->control->index_reads[0]));
            return QP_WAIT;
        }
        if(status) return status;
        o->phase=2;
    }
}
static int64_t next_row_impl(struct execution *s,unsigned id,struct qs_cell *row)
{
    if(id>=s->plan->count) return -1;
    struct qp_node *n=&s->plan->nodes[id]; struct operator_state *o=&s->nodes[id];
    if(s->worker && (int)id==s->source_node) {
        if(n->kind==QP_AGGREGATE && s->owner->partition_join<0) {
            const struct operator_state *source=&s->owner->nodes[id];
            int64_t status=projection_init(s,id,1); if(status) return status;
            if(o->source_projection) return projection_next(s,id,row,1);
            for(;;) {
                const struct compact_group *g=group_source_next(s,id); if(!g) return 0;
                struct qs_cell keys[QS_KEYS]; group_keys(n,source,g,keys);
                status=group_result(s,n,g->values,keys,row,0); if(status) return status;
            }
        }
        unsigned bytes=hash_row_bytes(&n->expr),block_rows=QS_SORT_IO_BYTES/bytes;
        if(s->source_at==s->source_end) {
            s->source_at=atomic_fetch_add(&s->owner->next_offset,block_rows);
            if(s->source_at>=s->source.rows) return 0;
            s->source_end=s->source_at+block_rows;
            if(s->source_end>s->source.rows) s->source_end=s->source.rows;
        }
        unsigned char packed[QS_OUT*19];
        int64_t status=spool_row_bytes(s,&s->source,s->source_at,bytes,packed); if(status) return status;
        hash_load(row,packed,&n->expr); ++s->source_at; return 2;
    }
    if(o->finished && !projection_pending(o->projection) && !projection_pending(o->source_projection)) {
        projection_clear(s,&o->projection); projection_clear(s,&o->source_projection); return 0;
    }
    if(!s->worker && s->plan->workers>1 && n->parallel) return parallel_next(s,id,row);
    if(id==s->plan->root && s->raw_projection) return scan_next(s,id,row);
    if(n->kind==QP_SCAN || n->kind==QP_SORT || (n->kind==QP_PROJECT && n->left>=0) || n->kind==QP_JOIN || n->kind==QP_INDEX_JOIN) {
        int status=projection_init(s,id,0); if(status) return status;
        if(o->projection) return projection_next(s,id,row,0);
    }
    if(n->kind==QP_SCAN) return scan_next(s,id,row);
    if(n->kind==QP_JOIN) return n->hash?hash_next(s,id,row):join_next(s,id,row);
    if(n->kind==QP_INDEX_JOIN) return index_next(s,id,row,NULL);
    if(n->kind==QP_AGGREGATE) {
        int status=projection_init(s,id,1); if(status) return status;
        if(o->source_projection) return projection_next(s,id,row,1);
        return aggregate_next(s,id,row);
    }
    if(n->kind==QP_PROJECT && n->left<0) {
        if(o->emitted++) return 0;
        int count=project(s,n,NULL,NULL,row);
        return count<0?count:count?2:0;
    }
    if(n->kind==QP_MATERIALIZED) {
        int64_t status=materialized_row(s,id,o->position,row); if(status==2) ++o->position; return status;
    }
    for(;;) {
        int64_t status;
        if(n->kind==QP_SORT) status=sorted_input(s,&o->left_sort,n->left,n->expr.norder,NULL,n->expr.order,o->left);
        else if(n->kind==QP_CTE_SCAN) {
            status=materialized_row(s,n->left,o->position,o->left); if(status==2) ++o->position;
        } else {
            if(n->kind==QP_LIMIT && n->expr.limit>=0 && o->emitted>=(uint64_t)n->expr.limit) return 0;
            status=next_row(s,n->left,o->left);
        }
        if(status!=2) return status;
        if(n->kind==QP_LIMIT && o->skipped<(uint64_t)n->expr.offset) { ++o->skipped; continue; }
        int count=project(s,n,o->left,NULL,row); if(count<0) return count;
        if(count) { ++o->emitted; return 2; }
    }
}
/* DuckDB-style local ungrouped states, combined after the worker finishes.
 * AVG transfers sum/count; only the coordinator performs the final division. */
static int aggregate_combine(struct execution *s,unsigned id,const struct operator_state *from)
{
    const struct qp_node *n=&s->plan->nodes[id]; struct operator_state *to=&s->nodes[id];
    for(unsigned i=to->aggregate_position;i<n->expr.naggs;++i,to->aggregate_position=i) {
        const struct accumulator *a=&from->accum[i]; struct accumulator *b=&to->accum[i];
        if(!a->count) continue;
        unsigned kind=n->expr.aggs[i].kind;
        if(kind==Q_MIN || kind==Q_MAX) {
            int cmp=b->count?compare_cell(s,a->extreme,b->extreme,n->expr.exprs[n->expr.aggs[i].expr].type):0;
            if(cmp==INT_MIN) return -5;
            if(!b->count || (kind==Q_MIN?cmp<0:cmp>0)) b->extreme=a->extreme;
        }
        struct group_value partial={.sum=b->sum,.count=b->count};
        int status=group_add(&partial,a->sum,a->count,kind); if(status) return status;
        b->sum=partial.sum; b->count=partial.count;
    }
    to->aggregate_position=0; return 0;
}
/* A scan/projection-only input can feed the local aggregate directly from
 * expression vectors. Row batches remain the adapter for joins and other
 * operators that have not yet acquired a column-batch interface. */
static int aggregate_sink_vectors(struct execution *s,unsigned id)
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
static int aggregate_sink_batch(struct execution *s,struct qs_result *out)
{
    unsigned id=s->aggregate_sink-1,columns=s->plan->nodes[s->root].expr.nout;
    struct operator_state *o=&s->nodes[id];
    for(;out->begin<out->rows;++out->begin) {
        memcpy(o->left,(struct qs_cell *)out->data+(size_t)out->begin*columns,columns*sizeof(*o->left));
        int status=accumulate(s,&s->plan->nodes[id],o); if(status) return status;
    }
    out->rows=out->begin=0; return 0;
}
/* Build the read-only side once, then give each CU its own probe cursor,
 * index cache and AFDM output. The host schedules these ordinary CSF calls. */
static void close_workers(struct execution *s)
{
    for(unsigned w=0;w<s->plan->workers;++w) if(s->workers[w]) {
        struct execution *e=s->workers[w];
        for(unsigned i=0;i<s->plan->count;++i) if(e->nodes[i].index_control.state) qs_index_close(&e->nodes[i].index_control);
        e->control->state=0; cs_work_end(e->context); s->workers[w]=NULL;
    }
}
/* Local Hash Aggregate tables use the same layout/update functions as the
 * shared table. Admission uses the query's actual free memory, retaining the
 * existing spill reserve; no per-CU fraction is taken from the query limit. */
static void parallel_group_begin(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id]; unsigned workers=s->plan->workers;
    if(s->partition_join>=0) return;
    if(!o->group_locals) {
        struct cs_work_context_stats m;
        if(cs_work_get_context_stats(s->context,&m)) return;
        size_t data=(size_t)(o->group_capacity-o->group_capacity/8)*o->group_stride;
        size_t entries=(size_t)o->group_capacity*sizeof(*o->group_entries);
        size_t bytes=workers*(data+entries+sizeof(*o)+3*128+sizeof(o)); /* allocation/alignment headroom */
        size_t available=m.limit_bytes-m.used_bytes,reserve=2*s->sort_limit;
        if(available<reserve || bytes>available-reserve) return;
        o->group_locals=cs_work_calloc(s->context,workers,sizeof(*o->group_locals));
        if(!o->group_locals) return;
        o->group_local_count=workers;
        for(unsigned w=0;w<workers;++w) {
            struct operator_state *local=cs_work_calloc(s->context,1,sizeof(*local));
            o->group_locals[w]=local;
            if(!local) goto fallback;
            local->group_capacity=o->group_capacity; local->group_stride=o->group_stride; local->group_integer=o->group_integer;
            local->group_count=&local->hash_rows; atomic_init(local->group_count,0);
            local->groups=cs_work_alloc(s->context,data);
            local->group_entries=cs_work_alloc(s->context,entries);
            if(!local->groups || !local->group_entries) goto fallback;
        }
    }
    if(o->group_local_count==workers)
        for(unsigned w=0;w<workers;++w) s->workers[w]->nodes[id].group_local=o->group_locals[w];
    return;
fallback:
    for(unsigned w=0;w<o->group_local_count;++w) group_drop_local(s,o->group_locals[w]);
    cs_work_free(s->context,o->group_locals); o->group_locals=NULL; o->group_local_count=0;
}
static int64_t group_combine_local(struct execution *s)
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
static void parallel_group_combine(struct execution *s,unsigned id)
{
    struct operator_state *o=&s->nodes[id];
    /* With an empty target, each hash partition has one owning CU. Lookups
     * share publication, but matching aggregate states need no update lock.
     * Keep inputs immutable so a full target can restart through normal spill. */
    s->group_shard=!atomic_load(o->group_count);
    s->group_combine=id+1; s->parallel_done=0;
    memset(s->parallel_position,0,sizeof(s->parallel_position));
    for(unsigned w=0;w<s->plan->workers;++w) {
        struct execution *e=s->workers[w];
        e->group_shard=s->group_shard; e->group_merge_full=false;
        e->group_combine=id+1; e->host_scan=false;
        e->nodes[id].group_local_slot=e->nodes[id].group_local_row=0; e->metrics=(struct qs_metrics){0};
        e->profile=(struct qs_profile){0}; e->local_profile_cpu=e->sink_ns=e->sink_rows=0;
        for(unsigned i=0;i<s->plan->count;++i) e->nodes[i].index_task.index_pages=e->nodes[i].index_task.index_tuples=0;
        *e->control=(struct qp_control){.state=(uintptr_t)e};
        memset(&s->outputs[w+1],0,sizeof(s->outputs[w+1]));
    }
    s->control->parallel=2; s->control->scan_node=0; s->control->available=s->control->drained=0;
    ++s->control->epoch;
    if(s->profiling) { s->pipeline_start=now_ns(CLOCK_MONOTONIC); s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID); }
}
/* Like DuckDB's LocalSortState: local sinks own runs, while the coordinator
 * retains their handles for one merge Source. Allocate the first runs before
 * launching CUs, so every started sink can evict one of its own full runs. */
static void parallel_sort_begin(struct execution *s,unsigned root)
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
static int64_t parallel_next(struct execution *s,unsigned root,struct qs_cell *row)
{
    struct qp_plan *p=s->plan;
restart:
    if(!s->parallel_started || s->parallel_node!=root) {
        int source=qp_pipeline_source(p,root);
        if(source<0) { p->nodes[root].parallel=0; return row?next_row(s,root,row):3; }
        int builds[QP_NODES]; unsigned count=0;
        for(int id=root;id>=0 && id!=source;id=p->nodes[id].left)
            if(p->nodes[id].kind==QP_JOIN) builds[count++]=id;
        /* Finish deeper build dependencies first. If a parent falls back to
         * Merge Join, its left input must not start another parallel phase
         * while the parent's unfinished right input still owns the buffers. */
        for(unsigned i=count;i;--i) {
            unsigned id=builds[i-1];
            int64_t status=hash_build(s,id); if(status) return status;
            if(s->nodes[id].external && s->nodes[id].finished) {
                for(unsigned j=0;j<count;++j) {
                    struct operator_state *o=&s->nodes[builds[j]];
                    if(o->hash_data) cs_work_free(s->context,o->hash_data);
                    o->hash_data=NULL;
                }
                s->control->parallel=0; s->nodes[root].finished=1; return 0;
            }
            if(s->nodes[id].hash_fallback==1) {
                p->nodes[root].parallel=0;
                return row?next_row(s,root,row):3;
            }
        }
        s->partition_join=-1;
        for(unsigned i=0;i<count;++i)
            if(s->nodes[builds[i]].external && !s->nodes[builds[i]].external->direct) { s->partition_join=builds[i]; break; }
        if(p->nodes[source].kind==QP_AGGREGATE && s->partition_join<0) {
            struct blocking_timer timer={0}; if(s->profiling) timer=blocking_begin(s);
            int64_t status=hash_aggregate_next_impl(s,source,NULL,1,0);
            if(s->profiling) blocking_end(s,QS_AGG_SINK,timer);
            if(status!=3) return status;
            if(s->nodes[source].group_phase!=3 || !s->nodes[source].groups) {
                p->nodes[root].parallel=0; return row?next_row(s,root,row):3;
            }
            /* A direct Source has no expensive probe chain to distribute.
             * Base its task count on real groups, not a large PG estimate. */
            if(source==(int)root && atomic_load(s->nodes[source].group_count)<QP_VECTOR_ROWS) {
                p->nodes[root].parallel=0; return row?next_row(s,root,row):3;
            }
        }
        unsigned hash_sink=0,group_sink=0,aggregate_sink=0;
        for(unsigned i=0;i<p->count;++i) {
            const struct qp_node *parent=&p->nodes[i];
            if(parent->kind==QP_AGGREGATE && parent->left==(int)root &&
               !parent->nkeys && !parent->distinct && !parent->distinct_aggs)
                aggregate_sink=i+1;
        }
        for(unsigned i=0;i<p->count;++i)
            if(p->nodes[i].kind==QP_AGGREGATE && p->nodes[i].left==(int)root &&
               p->nodes[i].hash && s->nodes[i].groups && !s->nodes[i].group_phase)
                group_sink=i+1;
        for(unsigned i=0;i<p->count;++i)
            if(p->nodes[i].kind==QP_JOIN && p->nodes[i].right==(int)root &&
               s->nodes[i].hash_data && !s->nodes[i].hash_phase && !s->nodes[i].hash_fallback)
                hash_sink=i+1;
        /* Build dependencies are now ready. Size the empty parent from PG's
         * output estimate, or the exact group count when preserved, instead
         * of stopping CU waves to grow the original 128-row table. DuckDB
         * sizes its pointer table from Count() after collecting build rows;
         * our streaming sink retains growth/spill for an underestimated size. */
        int counted=source;
        for(int id=root;id!=source;id=p->nodes[id].left)
            if(p->nodes[id].kind!=QP_PROJECT || p->nodes[id].expr.predicate>=0) { counted=-1; break; }
        if(hash_sink && s->partition_join<0) {
            struct operator_state *o=&s->nodes[hash_sink-1];
            struct cs_work_context_stats m;
            if(cs_work_get_context_stats(s->context,&m)) return -4;
            size_t reserve=2*s->sort_limit+p->workers*(sizeof(*s)+p->count*sizeof(*s->nodes)+sizeof(struct qs_plan)+256);
            size_t available=m.limit_bytes-m.used_bytes;
            available=available>reserve?available-reserve:0;
            size_t stride=hash_row_bytes(&p->nodes[root].expr)+3*sizeof(uint32_t);
            uint64_t rows=p->nodes[root].estimated_rows;
            if(counted>=0 && p->nodes[source].kind==QP_AGGREGATE && p->nodes[source].expr.predicate<0)
                rows=atomic_load(s->nodes[source].group_count);
            unsigned capacity=o->hash_capacity;
            while(capacity<=rows && capacity<UINT_MAX/4 && (size_t)capacity*2*stride<=available) capacity*=2;
            if(!o->hash_rows && capacity>o->hash_capacity) {
                void *data=hash_memory(s,hash_sink-1,NULL,(size_t)capacity*stride);
                if(data) {
                    cs_work_free(s->context,o->hash_data); o->hash_data=data;
                    o->hash_capacity=capacity; hash_reindex(s,hash_sink-1);
                }
            }
        }
        for(unsigned w=0;w<p->workers;++w) {
            struct cs_work_context_stats stats;
            if(cs_work_get_context_stats(s->context,&stats)) return -4;
            const struct qp_node *leaf=&p->nodes[source];
            size_t private_bytes=sizeof(*s)+p->count*sizeof(*s->nodes)+sizeof(struct qs_plan)+256;
            if(leaf->ordered_index) private_bytes+=QS_INDEX_WORK_BYTES+sizeof(struct qs_plan);
            size_t spill_reserve=0;
            for(unsigned i=0;i<p->count;++i)
                if(p->nodes[i].kind==QP_AGGREGATE && p->nodes[i].left==(int)root && s->nodes[i].group_capacity)
                    spill_reserve=2*s->sort_limit;
            /* Cursor/filter state is allocated lazily by the worker. Keep
             * that space and the existing spill reserve before launching;
             * otherwise a successful launch can fail on its first page. */
            if(stats.limit_bytes-stats.used_bytes<(p->workers-w)*private_bytes+spill_reserve) {
                close_workers(s); p->workers=1;
                return row?next_row(s,root,row):3;
            }
            cs_work_context *context=cs_work_begin_child(s->context,stats.limit_bytes);
            struct execution *e=context?cs_work_calloc(context,1,sizeof(*e)):NULL;
            if(e) e->nodes=cs_work_calloc(context,p->count,sizeof(*e->nodes));
            if(!e || !e->nodes) {
                if(context) cs_work_end(context);
                close_workers(s); p->nodes[root].parallel=0;
                return row?next_row(s,root,row):3;
            }
            e->context=context; e->plan=p; e->owner=s; e->raw_projection=s->raw_projection; e->worker=w+1; e->root=root;
            e->hash_sink=hash_sink; e->group_sink=group_sink; e->aggregate_sink=aggregate_sink;
            e->control=s->control+w+1; e->dictionary=s->dictionary; e->arena=s->arena;
            e->paged=s->paged; e->literal_ids=s->literal_ids; e->dictionary_ready=1;
            if(e->paged) {
                e->text=qs_dictionary_text_create(e->paged);
                if(!e->text) { cs_work_end(context); close_workers(s); p->nodes[root].parallel=0; return row?next_row(s,root,row):3; }
            }
            e->io_bytes=QS_SORT_IO_BYTES;
            e->sort_limit=s->sort_limit; e->profiling=s->profiling; e->initializer=p->ninit;
            e->source_node=-1;
            if(p->nodes[source].kind==QP_AGGREGATE) e->source_node=source;
            if(s->partition_join>=0) {
                const struct external_hash *h=s->nodes[s->partition_join].external;
                e->source_node=p->nodes[s->partition_join].left;
                e->source=h->probe[h->partition]; e->source.cache=NULL; e->source.loaded=UINT_MAX;
            }
            memcpy(e->params,s->params,sizeof(e->params));
            for(unsigned i=0;i<p->count;++i) {
                const struct operator_state *built=&s->nodes[i]; struct operator_state *local=&e->nodes[i];
                if(built->hash_phase) {
                    local->hash_data=built->hash_data; local->hash_rows=built->hash_rows;
                    local->hash_capacity=built->hash_capacity; local->hash_phase=1; local->hash_shared=1;
                    local->hash_buckets=built->hash_buckets;
                }
                if(built->set_ready) {
                    local->set=built->set; local->set_capacity=built->set_capacity;
                    local->set_count=built->set_count; local->set_null=built->set_null; local->set_ready=1;
                    local->set_pages=built->set_pages; local->set_external=built->set_external;
                }
            }
            *e->control=(struct qp_control){.state=(uintptr_t)e}; s->workers[w]=e;
            if(leaf->kind==QP_SCAN) *(struct qs_chunk *)(s->arena+vector_offset(e,leaf))=
                (struct qs_chunk){.capacity=leaf->ordered_index?256:qp_scan_capacity(p,leaf),.ncols=leaf->expr.ncols};
        }
        if(group_sink) parallel_group_begin(s,group_sink-1);
        if(!hash_sink && !group_sink && !aggregate_sink) parallel_sort_begin(s,root);
        s->parallel_node=root; s->parallel_done=0;
        memset(s->parallel_position,0,sizeof(s->parallel_position)); atomic_store(&s->next_offset,0);
        s->control->parallel=hash_sink || group_sink?2:1; ++s->control->epoch; s->control->available=s->control->drained=0;
        s->control->scan_node=0;
        int scan=qp_probe_scan(p,root);
        bool scheduled=s->partition_join<0 && scan>=0 && !p->nodes[scan].ordered_index;
        /* Index probes retain their keys/payloads across namespace waits.
         * They can consume the same independently scheduled scan morsels as
         * hash probes; only the retained Heap window ends at a morsel edge. */
        if(scheduled) s->control->scan_node=scan+1;
        s->parallel_started=1; s->used_workers=p->workers;
        if(s->profiling) { s->pipeline_start=now_ns(CLOCK_MONOTONIC); s->pipeline_tick=now_ns(CLOCK_THREAD_CPUTIME_ID); }
    }
    for(unsigned w=0;w<p->workers;++w) if(s->control->available&(UINT64_C(1)<<w)) {
        struct qs_result *out=&s->outputs[w+1];
        /* Direct HOST delivery has already consumed the CU's final packet.
         * The coordinator only collects EOF/metrics, never its typed rows. */
        if(p->packed_result==QP_DIRECT_RESULT) s->parallel_position[w]=out->rows;
        if(s->parallel_position[w]<out->rows) {
            if(!row) return 2; /* Borrow this batch without advancing it. */
            unsigned cols=p->nodes[root].expr.nout;
            memcpy(row,(struct qs_cell *)out->data+(size_t)s->parallel_position[w]++*cols,cols*sizeof(*row));
            return 2;
        }
        if(out->finished) s->parallel_done|=UINT64_C(1)<<w;
        s->parallel_position[w]=0; s->control->available&=~(UINT64_C(1)<<w); s->control->drained|=UINT64_C(1)<<w;
    }
    if(s->parallel_done==qp_worker_mask(p->workers)) {
        struct qp_pipeline_profile *phase=&p->pipelines[s->group_combine?s->group_combine-1:root];
        if(!s->parallel_combining && s->profiling) {
            phase->wall_ns+=now_ns(CLOCK_MONOTONIC)-s->pipeline_start;
            phase->coordinator_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-s->pipeline_tick; ++phase->rounds;
        }
        s->parallel_combining=1;
        for(unsigned w=s->parallel_combine_worker;w<p->workers;++w,s->parallel_combine_worker=w) {
            const struct qs_result *v=&s->outputs[w+1];
            if(s->workers[w]->aggregate_sink) {
                unsigned id=s->workers[w]->aggregate_sink-1;
                int status=aggregate_combine(s,id,&s->workers[w]->nodes[id]); if(status) return status;
            }
            if(s->profiling) {
                phase->worker_ns+=v->profile.operators[QS_JOIN_PROBE].cpu_ns;
                phase->sink_ns+=s->workers[w]->sink_ns; phase->sink_rows+=s->workers[w]->sink_rows;
            }
            s->metrics.build_rows+=v->metrics.build_rows;
            s->metrics.scanned+=v->metrics.scanned; s->metrics.matched+=v->metrics.matched;
            s->metrics.input_bytes+=v->metrics.input_bytes-v->metrics.index_pages*QS_PAGE;
            s->metrics.index_pages+=v->metrics.index_pages; s->metrics.index_tuples+=v->metrics.index_tuples;
            s->metrics.spill_read_bytes+=v->metrics.spill_read_bytes;
            s->metrics.spill_write_bytes+=v->metrics.spill_write_bytes;
            for(unsigned op=0;op<QS_OPERATORS;++op) {
                struct qs_timing *t=&s->profile.operators[op]; const struct qs_timing *x=&v->profile.operators[op];
                t->calls+=x->calls; t->wall_ns+=x->wall_ns; t->cpu_ns+=x->cpu_ns;
            }
        }
        s->parallel_combine_worker=0; s->parallel_combining=0;
        unsigned group=s->workers[0]->group_sink;
        if(!s->group_combine && group && s->nodes[group-1].group_locals) {
            parallel_group_combine(s,group-1); return QP_WAIT;
        }
        if(s->group_combine && s->group_shard) {
            struct operator_state *o=&s->nodes[s->group_combine-1]; bool full=false;
            for(unsigned w=0;w<p->workers;++w) full|=s->workers[w]->group_merge_full;
            if(full) {
                memset(o->group_entries,0,(size_t)o->group_capacity*sizeof(*o->group_entries));
                atomic_store(o->group_count,0);
            } else for(unsigned w=0;w<o->group_local_count;++w)
                o->group_locals[w]->group_flush=atomic_load(o->group_locals[w]->group_count);
        }
        s->group_combine=0; s->group_shard=false; close_workers(s);
        int source=qp_pipeline_source(p,root);
        if(source>=0 && p->nodes[source].kind==QP_AGGREGATE && s->partition_join<0) {
            cs_work_free(s->context,s->nodes[source].groups); s->nodes[source].groups=NULL;
            cs_work_free(s->context,s->nodes[source].group_entries); s->nodes[source].group_entries=NULL;
            s->nodes[source].finished=1;
        }
        s->parallel_started=0; s->control->parallel=0;
        if(s->partition_join>=0) {
            external_advance(s,s->partition_join); s->partition_join=-1;
            goto restart;
        }
        for(int id=root;id>=0 && id!=source;id=p->nodes[id].left) if(p->nodes[id].kind==QP_JOIN) {
            cs_work_free(s->context,s->nodes[id].hash_data); s->nodes[id].hash_data=NULL;
        }
        s->nodes[root].finished=1; return 0;
    }
    return QP_WAIT;
}
static unsigned sort_depth(const struct qp_plan *plan,unsigned id)
{
    const struct qp_node *n=&plan->nodes[id]; unsigned left=n->left<0?0:sort_depth(plan,n->left),right=n->right<0?0:sort_depth(plan,n->right);
    if(n->kind==QP_JOIN) return (left>right?left:right)+2;
    return left+(n->kind==QP_SORT || (n->kind==QP_AGGREGATE && n->nkeys && (!n->sorted_input || n->distinct)));
}
static void drop_sort(struct sorted_rows *sort)
{
    if(!sort->initialized || !sort->task.state || sort->drained) return;
    struct qs_group group=sort->group;
    if(!group.count) { group.count=1; group.states[0]=sort->task.state; }
    void *mr[]={&group}; long long len[]={sizeof(group)};
    struct femu_csd_args a={.numr=1,.mr_addr=mr,.mr_len=len};
    pg_sina_sort_source_impl(&a); sort->task.state=0;
}
static void close_operators(struct execution *s)
{
    if(s->operators_closed) return;
    s->operators_closed=1;
    for(unsigned i=0;i<s->plan->count;++i) { drop_sort(&s->nodes[i].left_sort); drop_sort(&s->nodes[i].right_sort); }
    for(unsigned i=0;i<s->plan->count;++i) if(s->nodes[i].index_control.state) qs_index_close(&s->nodes[i].index_control);
    close_workers(s);
}
static void close_execution(struct execution *s)
{
    close_operators(s);
    if(s->paged && !s->owner) qs_dictionary_abort(s->paged);
    cs_work_context *context=s->context; s->control->state=0; cs_work_end(context);
}
/* BUSY completes this graph call without publishing a partial result.
 * Keep its row count private until the same batch can continue. */
static int64_t dictionary_request(struct execution *s,struct qs_result *out,int64_t status)
{
    if(status!=QS_DICTIONARY_BUSY) return status;
    s->buffered_rows=out->rows; s->buffered_finished=out->finished; s->buffered=1;
    out->rows=out->finished=0;
    s->control->output_rows=s->control->finished=0;
    return s->worker?QP_RESOURCE_WAIT:0;
}
static int64_t finish_dictionary(struct execution *s,struct femu_csd_args *a,struct qs_result *out)
{
    close_operators(s);
    int64_t status=qs_dictionary_close(s->paged,s->initial_access,s->io,s->closing>1?csReentryValue(a):0);
    if(status) {
        s->closing=status==QS_DICTIONARY_BUSY?1:2;
        out->finished=0; s->control->output_rows=s->control->finished=0;
        return dictionary_request(s,out,status);
    }
    s->paged=NULL; out->finished=s->control->finished=1; s->control->output_rows=out->rows;
    close_execution(s); return 0;
}
/* Resource discovery uses a normal zero-argument CSF and its 64-bit completion
 * value. SNIA CSE properties report CUs, but do not describe this allocator. */
int64_t pg_sina_workmem_capacity(struct femu_csd_args *a)
{
    struct cs_work_stats stats;
    if(!a || a->numr || cs_work_get_stats(&stats) || stats.capacity_bytes>INT64_MAX) return -1;
    return stats.capacity_bytes;
}
/* plan, lifecycle/page requests, result, input/dictionary arena, spill I/O.
 * A successful call either returns result rows or asks PG to LOAD pages.
 * NEED_STORE/LOAD is handled entirely by the existing SNIA batch scheduler. */
static int64_t initialize_dictionary(struct execution *s,struct femu_csd_args *a)
{
    if(s->dictionary_ready) return 0;
    const struct qs_dict *config=(const void *)s->arena;
    uint64_t completion=s->initial_wait>QS_DICTIONARY_BUSY?csReentryValue(a):0;
    for(;s->imported<=config->count;++s->imported) {
        const char *bytes; unsigned length; uint32_t id=0;
        if(qs_dict_get(config,s->imported,&bytes,&length)) return -3;
        int64_t status=qs_dictionary_insert(s->paged,s->initial_access,bytes,length,&id,s->io,completion);
        s->initial_wait=status; if(status) return status;
        completion=0; if(id!=s->imported) return -3;
    }
    for(;s->literal_node<s->plan->count;++s->literal_node,s->literal_string=0) {
        const struct qs_plan *p=&s->plan->nodes[s->literal_node].expr;
        if(p->nstrings>QS_STRINGS) return -1;
        for(;s->literal_string<p->nstrings;++s->literal_string) {
            const struct qs_string *v=&p->strings[s->literal_string]; uint32_t id=0;
            if(v->length>sizeof(v->bytes)) return -1;
            int64_t status=qs_dictionary_insert(s->paged,s->initial_access,v->bytes,v->length,&id,s->io,completion);
            s->initial_wait=status; if(status) return status;
            completion=0; s->literal_ids[s->literal_node*QS_STRINGS+s->literal_string]=id;
        }
    }
    s->dictionary_ready=1; return 0;
}
/* Translate this SQL program's immutable file mapping into generic device
 * reads. Neither FEMU nor SNIA sees the file/extent interpretation. */
static int namespace_ranges(struct execution *s,const struct qp_file_mapping *map,
    uint64_t at,uint64_t bytes,uint64_t target,unsigned *count)
{
    if(!map->count) return 0;
    if(!map->nsid || !map->lba_bytes || map->reserved || map->offset<sizeof(*s->plan) ||
       map->offset>(uint64_t)s->args->mr_len[0] ||
       map->count>((uint64_t)s->args->mr_len[0]-map->offset)/sizeof(struct qs_extent) ||
       bytes>UINT64_MAX-at) return -1;
    const struct qs_extent *extents=(void *)((unsigned char *)s->plan+map->offset);
    struct femu_graph_read *reads=s->io;
    unsigned lo=0,hi=map->count;
    while(lo<hi) {
        unsigned mid=lo+(hi-lo)/2;
        if(extents[mid].logical+extents[mid].bytes<=at) lo=mid+1; else hi=mid;
    }
    uint64_t end=at+bytes;
    for(unsigned i=lo;i<map->count && at<end;++i) {
        const struct qs_extent *e=&extents[i];
        if(e->logical>at || e->bytes>UINT64_MAX-e->logical) return -1;
        uint64_t n=end<e->logical+e->bytes?end-at:e->logical+e->bytes-at;
        if(!n || (at-e->logical)%map->lba_bytes || n%map->lba_bytes || n/map->lba_bytes>UINT32_MAX) return -1;
        uint64_t lba=e->lba+(at-e->logical)/map->lba_bytes;
        struct femu_graph_read *last=*count?&reads[*count-1]:NULL;
        if(last && le32toh(last->nsid)==map->nsid &&
            le64toh(last->lba)+le32toh(last->blocks)==lba &&
            le64toh(last->offset)+(uint64_t)le32toh(last->blocks)*map->lba_bytes==target &&
            n/map->lba_bytes<=UINT32_MAX-le32toh(last->blocks))
            last->blocks=htole32(le32toh(last->blocks)+n/map->lba_bytes);
        else {
            if(*count==FEMU_CSD_GRAPH_MAX_NODES) return 0;
            reads[(*count)++]=(struct femu_graph_read){.nsid=htole32(map->nsid),.blocks=htole32(n/map->lba_bytes),
                .afdm_index=htole32(3),.lba=htole64(lba),.offset=htole64(target)};
        }
        at+=n; target+=n;
    }
    return at==end?1:-1;
}
static int64_t namespace_wait(struct execution *s)
{
    struct qp_control *c=s->control;
    if(c->ready || (!c->bytes && !c->index_nreads)) return 0;
    if(c->node>=s->plan->count) return -1;
    const struct qp_node *n=&s->plan->nodes[c->node];
    unsigned count=0; int status;
    if(c->index_nreads) {
        if(c->index_nreads>QS_INDEX_READS || (n->kind!=QP_INDEX_JOIN && !n->ordered_index)) return -1;
        unsigned leaf=n->ordered_index?c->node:(unsigned)n->right;
        if(leaf>=s->plan->count) return -1;
        for(unsigned i=0;i<c->index_nreads;++i) {
            const struct qs_index_read *r=&c->index_reads[i];
            if(r->heap>1 || r->slot>=QS_INDEX_CACHE) return -1;
            const struct qp_file_mapping *map=r->heap?&s->plan->files[leaf]:&s->plan->indexes[leaf];
            status=namespace_ranges(s,map,(uint64_t)r->block*QS_PAGE,QS_PAGE,
                input_offset(s,n)+QS_SORT_IO_BYTES+(size_t)r->slot*QS_PAGE,&count);
            if(status<=0) return status;
        }
    } else {
        status=namespace_ranges(s,&s->plan->files[c->node],c->offset,c->bytes,input_offset(s,n),&count);
        if(status<=0) return status;
    }
    s->pending_nvm=count;
    return csNeedNvmRead(count);
}

/* A partially exported row remains in the existing result AFDM. Only
 * completed columns contain text; the remaining ones still contain IDs. */
static int export_row(struct execution *s,const struct qs_plan *root,struct qs_cell *row,struct qs_result *out)
{
    for(unsigned col=s->output_column;col<root->nout;++col) {
        if(!row[col].isnull && !qs_cell_string(&row[col]) && !qs_cell_is_string_ref(&row[col]) &&
           root->exprs[root->outputs[col].index].type==Q_STRING) {
            const char *bytes; unsigned length;
            if(text_get(s,(unsigned)row[col].value,&bytes,&length)) {
                s->output_column=col; s->output_pending=1; return -3;
            }
            if(length>255 || out->str_bytes+length>QS_RESULT_STRING_BYTES) { text_release(s); return -3; }
            if(length<=QS_STRING_MAX) qs_cell_set_string(&row[col],bytes,length);
            else {
                memcpy(out->str_data+out->str_bytes,bytes,length);
                qs_cell_set_string_ref(&row[col],out->str_bytes,length); out->str_bytes+=length;
            }
            if(text_release(s)) return -3;
        }
    }
    s->output_column=0; s->output_pending=0; return 0;
}

static int output_string_order(const void *a,const void *b)
{
    const struct output_string *x=a,*y=b;
    return x->id<y->id?-1:x->id>y->id;
}
/* Consume Sort's run-owned payload before its next Source batch can replace
 * the buffer. Expressions continue to use canonical IDs; only final output
 * cells become inline strings or references into the result AFDM. */
static void export_sort_text(struct execution *s,const struct qs_plan *root,
    struct qs_cell *row,struct qs_result *out)
{
    for(unsigned col=0;col<root->nout;++col) {
        struct qs_cell *v=&row[col];
        if(v->isnull || root->exprs[root->outputs[col].index].type!=Q_STRING || qs_cell_string(v) || qs_cell_is_string_ref(v)) continue;
        unsigned n; const char *bytes=sort_text_get(s,(uint32_t)v->value,&n);
        if(!bytes || n>255 || out->str_bytes+n>QS_RESULT_STRING_BYTES) continue;
        if(n && n<=QS_STRING_MAX) qs_cell_set_string(v,bytes,n);
        else {
            memcpy(out->str_data+out->str_bytes,bytes,n);
            qs_cell_set_string_ref(v,out->str_bytes,n); out->str_bytes+=n;
        }
    }
}
/* IDs and body locations are appended together. Resolve this fixed-size
 * batch in ID order, then write each value back to its original row/column.
 * This groups directory/body reads without changing the SQL result order. */
static int export_batch(struct execution *s,const struct qs_plan *root,struct qs_result *out)
{
    if(!s->output_active) {
        s->output_count=s->output_position=0;
        for(unsigned row=s->output_exported;row<out->rows;++row)
            for(unsigned col=0;col<root->nout;++col) {
                const struct qs_cell *v=qp_result_row(out,row,root->nout,s->plan->packed_result)+col;
                if(!v->isnull && !qs_cell_string(v) && !qs_cell_is_string_ref(v) && root->exprs[root->outputs[col].index].type==Q_STRING)
                    s->output_order[s->output_count++]=(struct output_string){(uint32_t)v->value,row,col};
            }
        qsort(s->output_order,s->output_count,sizeof(*s->output_order),output_string_order);
        s->output_active=1;
    }
    for(;s->output_position<s->output_count;++s->output_position) {
        const struct output_string *at=&s->output_order[s->output_position];
        const char *bytes; unsigned length;
        if(text_get(s,at->id,&bytes,&length)) return -3;
        if(length>255 || out->str_bytes+length>QS_RESULT_STRING_BYTES) { text_release(s); return -3; }
        struct qs_cell *v=qp_result_row(out,at->row,root->nout,s->plan->packed_result)+at->column;
        if(length<=QS_STRING_MAX) qs_cell_set_string(v,bytes,length);
        else {
            memcpy(out->str_data+out->str_bytes,bytes,length);
            qs_cell_set_string_ref(v,out->str_bytes,length); out->str_bytes+=length;
        }
        if(text_release(s)) return -3;
    }
    s->output_exported=out->rows; s->output_active=0; return 0;
}

/* Encode one boundary row before its input or Sort source buffer is reused.
 * A dictionary wait retains this row and completed columns. If the variable
 * arena is full, publish the current packet and retry this already encoded
 * row against the next empty packet. */
static int host_append(struct execution *s,const struct qs_plan *p,struct qs_result *out)
{
    for(;s->host_column<p->nout;++s->host_column) {
        unsigned col=s->host_column,oid=p->outputs[col].type_oid;
        const struct qs_cell *v=&s->host_row[col]; s->host_values[col]=0;
        if(v->isnull) continue;
        if(!qp_host_variable(oid)) {
            if(oid==20 && (v->value>INT64_MAX || v->value<INT64_MIN)) return -3;
            if(oid==701) { double value=(double)v->value; memcpy(&s->host_values[col],&value,8); }
            else s->host_values[col]=(uint64_t)v->value;
            continue;
        }
        unsigned offset=(s->host_var_bytes+3)&~3U,bytes=0;
        unsigned char *to=s->host_var+offset;
        if(oid==1700) {
            if(s->raw_projection) {
                unsigned body=(unsigned)((unsigned __int128)v->value>>64);
                if(body>sizeof(s->host_var)-offset-4) return -3;
                bytes=body+4; uint32_t header=bytes<<2; memcpy(to,&header,4);
                memcpy(to+4,(const void *)(uintptr_t)v->value,body);
            } else bytes=qp_host_numeric(to,*v);
        } else {
            const char *text; unsigned length,kind=p->exprs[p->outputs[col].index].type;
            char single=(char)v->value;
            if(kind==Q_CHAR) { text=&single; length=1; }
            else if(s->raw_projection) { text=(const void *)(uintptr_t)v->value; length=(unsigned)((unsigned __int128)v->value>>64); }
            else if(qs_cell_string(v)) { text=qs_cell_string_bytes(v); length=qs_cell_string(v); }
            else if(text_get(s,(unsigned)v->value,&text,&length)) return -3;
            unsigned padded=oid==1042 && p->outputs[col].typmod>4?(unsigned)p->outputs[col].typmod-4:length;
            if(padded<length) padded=length;
            if(padded>255 || padded+4>sizeof(s->host_var)-offset) { text_release(s); return -3; }
            bytes=padded+4; uint32_t header=bytes<<2; memcpy(to,&header,4);
            memcpy(to+4,text,length); memset(to+4+length,' ',padded-length);
            if(text_release(s)) return -3;
        }
        s->host_values[col]=offset; s->host_var_bytes=offset+bytes;
    }
    unsigned base=(out->str_bytes+3)&~3U;
    if(s->host_var_bytes && s->host_var_bytes>sizeof(out->str_data)-base) return 1;
    if(s->host_var_bytes) memcpy(out->str_data+base,s->host_var,s->host_var_bytes);
    for(unsigned col=0;col<p->nout;++col) {
        unsigned width=s->host_widths[col];
        uint64_t value=s->host_values[col];
        if(qp_host_variable(p->outputs[col].type_oid)) value+=base;
        memcpy(s->host_columns[col]+(size_t)out->rows*width,&value,width);
        if(s->host_row[col].isnull) s->host_nulls[col][out->rows/8]|=1U<<(out->rows%8);
    }
    out->str_bytes=base+s->host_var_bytes; ++out->rows;
    s->host_pending=s->host_column=s->host_var_bytes=0;
    return 0;
}

int64_t pg_sina_plan(struct femu_csd_args *a)
{
    if(!a || a->numr!=5 || a->mr_len[0]<(long long)sizeof(struct qp_plan) ||
        a->mr_len[1]<(long long)sizeof(struct qp_control) || a->mr_len[2]<(long long)sizeof(struct qs_result) ||
        a->mr_len[4]<QS_SORT_IO_BYTES) return -1;
    struct qp_plan *plan=a->mr_addr[0]; unsigned worker=((uint64_t)a->cparam1>>1)&127;
    if(plan->workers>QS_WORKERS || worker>plan->workers || a->mr_len[1]<(long long)((worker+1)*sizeof(struct qp_control)) ||
       a->mr_len[2]<(long long)((worker+1)*sizeof(struct qs_result))) return -1;
    struct qp_control *control=(struct qp_control *)a->mr_addr[1]+worker;
    struct qs_result *out=(struct qs_result *)a->mr_addr[2]+worker;
    struct execution *s=(void *)(uintptr_t)control->state;
    if(control->close) { if(s) close_execution(s); return 0; }
    if(!s) {
        if(worker) return -1;
        if(!plan->count || plan->count>QP_NODES || plan->root>=plan->count) return -1;
        struct cs_work_stats stats; if(cs_work_get_stats(&stats)) return -4;
        size_t budget=plan->nodes[plan->root].expr.work_mem_bytes;
        if(!budget || budget>stats.capacity_bytes) budget=stats.capacity_bytes;
        cs_work_context *context=cs_work_begin_limit(budget); if(!context) return -4;
        s=cs_work_calloc(context,1,sizeof(*s)); if(!s) { cs_work_end(context); return -4; }
        s->context=context; s->plan=plan; s->control=control; s->raw_projection=qp_host_raw_projection(plan);
        s->partition_join=s->source_node=-1;
        s->arena=a->mr_addr[3]; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
        s->nodes=cs_work_calloc(context,plan->count,sizeof(*s->nodes));
        if(!s->nodes) { cs_work_end(context); return -4; }
        unsigned depth=sort_depth(plan,plan->root);
        s->sort_limit=budget/(depth+2); if(s->sort_limit<512*1024) { cs_work_end(context); return -4; }
        s->literal_ids=cs_work_calloc(context,plan->count*QS_STRINGS,sizeof(*s->literal_ids));
        unsigned text_columns=0;
        const struct qs_plan *output=&plan->nodes[plan->root].expr;
        for(unsigned col=0;col<output->nout;++col)
            text_columns+=output->exprs[output->outputs[col].index].type==Q_STRING;
        if(text_columns && plan->packed_result<QP_TYPED_RESULT) {
            s->output_order=cs_work_calloc(context,(size_t)qp_result_capacity(output,plan->packed_result)*text_columns,sizeof(*s->output_order));
            if(!s->output_order) { cs_work_end(context); return -4; }
        }
        s->paged=qs_dictionary_create(context);
        if(s->paged) { s->text=qs_dictionary_text_create(s->paged); s->initial_access=qs_dictionary_access_create(s->paged); }
        if(!s->literal_ids || !s->paged || !s->text || !s->initial_access) {
            if(s->paged) qs_dictionary_abort(s->paged);
            cs_work_end(context); return -4;
        }
        s->imported=1;
        s->profiling=a->cparam1&1; s->outputs=a->mr_addr[2]; control->state=(uintptr_t)s;
        out->rows=out->begin=out->finished=out->str_bytes=0; out->status=0;
    }
    s->args=a; s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
    /* A new submission must not acknowledge an earlier worker batch again,
     * including early returns while dictionary initialization/resume is BUSY.
     * Device-internal I/O reentry still accumulates acknowledgements until
     * the host observes this submission's single completion. */
    if(!csIsReentry(a)) control->drained=0;
    if(s->buffered) { out->rows=s->buffered_rows; out->finished=s->buffered_finished; s->buffered=0; }
    if(s->closing) return finish_dictionary(s,a,out);
    int typed=plan->packed_result==QP_DIRECT_RESULT || (!worker && plan->packed_result==QP_TYPED_RESULT);
    int continuing=s->text_wait || s->dictionary_wait || s->initial_wait || s->pending_sort || (typed && s->host_building);
    int64_t imported=initialize_dictionary(s,a);
    if(imported) return dictionary_request(s,out,imported);
    if(s->text_wait) {
        int64_t resumed=qs_dictionary_text_resume(s->text,s->io,s->text_wait>QS_DICTIONARY_BUSY?csReentryValue(a):0);
        s->text_wait=resumed; if(resumed) return dictionary_request(s,out,resumed);
        s->expression_wait=0;
    }
    if(!continuing && !csIsReentry(a) && ((uint64_t)a->cparam1&QP_PAGES_INPUT) && !((uint64_t)a->cparam1&QP_SCAN_INPUT))
        control->ready=1;
    if(!continuing && ((uint64_t)a->cparam1&QP_SCAN_INPUT) && !csIsReentry(a)) {
        int id=qp_probe_scan(plan,s->root);
        uint64_t page=(uint64_t)a->cparam1>>QP_SCAN_PAGE_SHIFT;
        if(!worker || id<0 || plan->nodes[id].ordered_index || page>UINT64_MAX/QS_PAGE) return -1;
        struct qp_node *n=&plan->nodes[id]; struct operator_state *o=&s->nodes[id];
        uint64_t offset=page*QS_PAGE;
        if(offset>n->bytes || (s->host_scan && !s->scan_drained)) return -1;
        s->host_scan=true; s->scan_drained=false;
        o->position=offset; o->scan_position=o->scan_input=o->scan_bytes=0;
        struct qs_chunk *chunk=(void *)(s->arena+vector_offset(s,n)); chunk->rows=chunk->selected=0;
        control->node=id; control->offset=offset;
        control->bytes=n->bytes-offset<plan->scan_bytes?n->bytes-offset:plan->scan_bytes;
        control->ready=control->bytes!=0;
    }
    /* The last AFDM is private to this CU, also for SNIA NEED_LOAD/STORE. */
    s->io=a->mr_addr[4]; s->io_bytes=a->mr_len[4];
    if(s->pending_nvm) {
        if(!csIsReentry(a) || csReentryValue(a)!=s->pending_nvm) return -1;
        s->pending_nvm=0; control->ready=1;
    }
    if(s->pending_spool) {
        if(!csIsReentry(a)) return -1;
        struct spool *p=s->pending_spool;
        if(s->pending_store) { p->tokens[p->blocks++]=csReentryValue(a); p->used=0; s->metrics.spill_write_bytes+=QS_SORT_IO_BYTES; }
        else { memcpy(p->cache,s->io,QS_SORT_IO_BYTES); p->loaded=s->pending_block; s->metrics.spill_read_bytes+=QS_SORT_IO_BYTES; }
        s->pending_spool=NULL;
    }
    if(!csIsReentry(a) && !continuing) {
        out->rows=out->begin=out->finished=out->str_bytes=0; out->status=0; s->output_exported=0;
    }
    uint64_t start=s->profiling?now_ns(CLOCK_MONOTONIC):0,cpu=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
    if(s->profiling && !worker) s->pipeline_tick=cpu;
    struct execution *owner=worker?s->owner:s;
    if(s->profiling) {
        unsigned active=atomic_fetch_add(&owner->active_cus,1)+1,peak=atomic_load(&owner->peak_cus);
        while(active>peak && !atomic_compare_exchange_weak(&owner->peak_cus,&peak,active)) { }
    }
    unsigned columns=plan->nodes[worker?(int)s->root:(int)plan->root].expr.nout;
    /* Internal buffers pack only the pipeline's projected columns. A two-column
     * pipeline fits 2048 rows in the same AFDM as 256 final sixteen-cell rows. */
    unsigned capacity=worker && !typed?QS_RESULT_ROWS*QS_OUT/(columns?columns:1):
        qp_result_capacity(&plan->nodes[plan->root].expr,plan->packed_result);
    int64_t status=0; int host_full=0,host_publish=0;
    if(typed && !s->host_building) {
        const struct qs_plan *p=&plan->nodes[plan->root].expr;
        for(unsigned col=0;col<p->nout;++col) {
            s->host_widths[col]=qp_host_width(p->outputs[col].type_oid);
            s->host_columns[col]=qp_host_values(out,p,capacity,col);
            s->host_nulls[col]=s->host_columns[col]+(size_t)capacity*s->host_widths[col];
            memset(s->host_nulls[col],0,(capacity+7)/8);
        }
    }
    if(s->output_active) { status=s->output_status; goto export_results; }
    while(s->initializer<plan->ninit) {
        unsigned index=s->initializer,node=plan->init_nodes[index]; int param=plan->init_params[index];
        if(param<0) {
            status=build_set(s,node); if(status) break;
            ++s->initializer; continue;
        }
        struct qs_cell value[QS_OUT];
        status=next_row(s,node,value);
        if(status==QP_WAIT || status<0 || status>2) break;
        if(!s->initializer_phase) {
            if(!status) { s->params[param]=(struct qs_cell){.isnull=1}; ++s->initializer; }
            else { s->params[param]=value[0]; s->initializer_phase=1; }
        } else {
            if(status==2) { status=-8; break; }
            s->initializer_phase=0; ++s->initializer;
        }
    }
produce:
    if(worker && s->group_combine) {
        uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
        status=group_combine_local(s);
        if(s->profiling) s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick;
        if(!status) out->finished=control->finished=1;
        goto combined;
    }
    /* STORE reentry must finish consuming the retained output before the
     * producer overwrites it with another batch. */
    if(worker && s->sort_sink && s->pending_sort==s->sort_sink && !s->sort_sink->columns) goto produced;
    /* A spilled-partition Source retains its original row cursor. Resident
     * groups can now expose finalized columns without reopening their input. */
    if(worker && s->hash_sink && (s->source_node!=(int)s->root || plan->nodes[s->root].kind==QP_AGGREGATE)) {
        unsigned id=s->root; int source=plan->nodes[id].kind==QP_AGGREGATE;
        status=projection_init(s,id,source); if(status<0) goto produced;
        while(source?s->nodes[id].source_projection:s->nodes[id].projection) {
            status=projection_fill(s,id,source);
            if(status!=2) {
                if(!status) out->finished=control->finished=1;
                if(status!=3) goto produced;
                break;
            }
            struct projection_batch *b=source?s->nodes[id].source_projection:s->nodes[id].projection;
            unsigned before=b->position;
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            status=hash_sink_vectors(s,id);
            if(s->profiling) { s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick; s->sink_rows+=b->position-before; }
            if(status<0) goto produced;
            /* The remaining rows must reach the coordinator before another
             * input overwrites the vectors; next_row retains their cursor. */
            if(b->position<b->count) break;
        }
        status=2;
    }
    if(worker && s->group_sink) {
        unsigned id=s->group_sink-1;
        status=projection_init(s,id,0); if(status<0) goto produced;
        while(s->nodes[id].projection) {
            status=projection_fill(s,id,0);
            if(status!=2) {
                if(!status) out->finished=control->finished=1;
                if(status!=3) goto produced;
                break;
            }
            struct projection_batch *b=s->nodes[id].projection;
            unsigned before=b->position,rows=out->rows;
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            status=group_sink_vectors(s,id,out,capacity);
            if(s->profiling) {
                s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick;
                s->sink_rows+=b->position-before-(out->rows-rows);
            }
            if(status<0) goto produced;
            status=2; if(out->rows) goto produced;
        }
        status=2;
    }
    if(worker && s->aggregate_sink) {
        unsigned id=s->aggregate_sink-1;
        status=projection_init(s,id,0); if(status<0) goto produced;
        while(s->nodes[id].projection) {
            status=projection_fill(s,id,0);
            if(status!=2) {
                if(!status) out->finished=control->finished=1;
                if(status!=3) goto produced;
                break;
            }
            uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
            unsigned rows=s->nodes[id].projection->count;
            status=aggregate_sink_vectors(s,id);
            if(s->profiling) { s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick; s->sink_rows+=rows; }
            if(status<0) goto produced;
        }
        status=2;
    }
    if(worker && s->sort_sink) {
        status=sort_sink_columns(s,s->sort_sink,s->root);
        if(status!=3) {
            if(!status) out->finished=control->finished=1;
            goto produced;
        }
        status=2;
    }
    while(status>=0 && status!=QP_WAIT && status<=2 && out->rows<capacity) {
        struct qs_cell *row=typed?s->host_row:worker?(struct qs_cell *)out->data+(size_t)out->rows*columns:
            qp_result_row(out,out->rows,columns,plan->packed_result);
        if(typed && !worker && !s->host_pending && plan->workers>1 && plan->nodes[plan->root].kind==QP_SCAN && plan->nodes[plan->root].parallel) {
            /* Workers already evaluated this Scan's projection. Borrow a
             * whole canonical batch, rather than running the generic row
             * dispatcher and searching the ready CU mask for every tuple.
             * Acknowledge it only on the next borrow, after all row payloads
             * (including deferred Numeric bodies) have been encoded. */
            if(!s->host_source_rows) {
                status=parallel_next(s,plan->root,NULL);
                if(status!=2) {
                    if(!status) out->finished=control->finished=1;
                    goto produced;
                }
                for(unsigned w=0;w<plan->workers;++w) if((s->control->available&(UINT64_C(1)<<w)) &&
                    s->parallel_position[w]<s->outputs[w+1].rows) {
                    s->host_source_worker=w;
                    s->host_source_rows=s->outputs[w+1].rows-s->parallel_position[w];
                    s->host_source=(const struct qs_cell *)s->outputs[w+1].data+(size_t)s->parallel_position[w]*columns;
                    break;
                }
            }
            memcpy(row,s->host_source,columns*sizeof(*row));
            s->host_source+=columns; --s->host_source_rows; ++s->parallel_position[s->host_source_worker];
            status=2;
        } else status=(typed?s->host_pending!=0:s->output_pending!=0)?2:next_row(s,worker?(int)s->root:(int)plan->root,row);
        if(status==QP_WAIT || status<0 || status>2) break;
        if(!status) { out->finished=control->finished=1; break; }
        const struct qs_plan *root=&plan->nodes[plan->root].expr;
        if(typed) {
            s->host_pending=1;
            int appended=host_append(s,root,out);
            if(appended<0) { status=appended; break; }
            if(appended) { host_full=1; break; }
            continue;
        }
        if(!worker) export_sort_text(s,root,row,out);
        if(!worker && !s->output_order && export_row(s,root,row,out)) { status=-3; break; }
        ++out->rows;
    }
produced:
    if(worker && s->sort_sink && !s->sort_sink->columns && status>=0 && status<=2 &&
       (out->rows || out->finished || s->pending_sort==s->sort_sink)) {
        struct sorted_rows *local=s->sort_sink;
        if(s->pending_sort!=local) local->producer_status=status;
        local->batch=out; out->begin=0;
        unsigned rows=out->rows;
        uint64_t tick=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
        status=sort_call(s,local,0);
        if(!status) {
            if(out->finished && !local->sink_finished) {
                local->finishing_sink=1;
                status=sort_call(s,local,0);
            }
        }
        if(!status) {
            local->batch=NULL; out->rows=0;
            status=out->finished?0:local->producer_status;
            if(s->profiling) s->sink_rows+=rows;
        }
        if(s->profiling) s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-tick;
        if(s->host_scan && !out->rows && status==2) goto produce;
    }
    if(worker && (s->hash_sink || s->group_sink || s->aggregate_sink) && status>=0 && status<=2) {
        uint64_t cpu=s->profiling?now_ns(CLOCK_THREAD_CPUTIME_ID):0;
        unsigned rows=out->rows;
        if(s->hash_sink) hash_sink_batch(s,out);
        if(s->group_sink) { int added=group_sink_batch(s,out); if(added<0) status=added; }
        if(s->aggregate_sink) { int added=aggregate_sink_batch(s,out); if(added<0) status=added; }
        if(s->profiling) { s->sink_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu; s->sink_rows+=rows-out->rows; }
        /* A direct sink can consume another output batch from the same input
         * without asking PG to submit another compute request. */
        if(s->host_scan && !out->rows && status==2) goto produce;
    }
export_results:
    if(!typed && !worker && s->output_order && out->rows && status>=0 && status<=2 &&
       !s->text_wait && !s->dictionary_wait) {
        if(!s->output_active) s->output_status=status;
        if(export_batch(s,&plan->nodes[plan->root].expr,out)) status=-3;
        else status=s->output_status;
    }
combined:
    if(typed) {
        host_publish=!s->text_wait && !s->dictionary_wait && status>=0 &&
            (out->finished || host_full || out->rows==capacity || (worker && s->host_scan && s->scan_drained));
        if(host_publish) { qp_host_compact(out,&plan->nodes[plan->root].expr,capacity); s->host_building=0; }
        else s->host_building=out->rows!=0;
    }
    if(s->text_wait) status=s->text_wait;
    if(s->dictionary_wait) status=s->dictionary_wait;
    if(s->expression_wait && s->pending_spool) {
        s->expression_wait=0;
        status=csNeedLoad(s->pending_spool->tokens[s->pending_block]);
    }
    if(status==QP_WAIT) {
        int64_t request=namespace_wait(s); if(request) status=request;
    }
    if(s->profiling) {
        struct qs_timing *t=&s->profile.operators[QS_JOIN_PROBE];
        if(!worker && s->parallel_started)
            plan->pipelines[s->group_combine?s->group_combine-1:s->parallel_node].coordinator_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-s->pipeline_tick;
        ++t->calls; t->wall_ns+=now_ns(CLOCK_MONOTONIC)-start; t->cpu_ns+=now_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
        atomic_fetch_sub(&owner->active_cus,1); control->peak_cus=atomic_load(&owner->peak_cus);
    }
    control->output_rows=(s->text_wait || s->dictionary_wait || (typed && !host_publish))?0:out->rows;
    control->output_strings=out->str_bytes;
    out->metrics=s->metrics; out->metrics.workers=s->used_workers?s->used_workers:1; out->profile=s->profile;
    struct qs_dictionary_stats dictionary;
    qs_dictionary_get_stats(s->paged,&dictionary);
    out->metrics.dictionary_entries=dictionary.strings; out->metrics.dictionary_bytes=dictionary.bytes;
    if(!worker) {
        out->metrics.spill_read_bytes+=dictionary.loads*QS_DICTIONARY_PAGE_BYTES;
        out->metrics.spill_write_bytes+=dictionary.stores*QS_DICTIONARY_PAGE_BYTES;
    }
    for(unsigned i=0;i<plan->count;++i) if(plan->nodes[i].kind==QP_INDEX_JOIN || plan->nodes[i].ordered_index) {
        out->metrics.index_pages+=s->nodes[i].index_task.index_pages;
        out->metrics.index_tuples+=s->nodes[i].index_task.index_tuples;
    }
    out->metrics.input_bytes+=out->metrics.index_pages*QS_PAGE;
    struct cs_work_context_stats memory;
    if(!cs_work_get_context_stats(s->context,&memory)) out->metrics.work_peak=memory.peak_used_bytes;
    if(status<0) return status;
    if(s->text_wait==QS_DICTIONARY_BUSY || s->dictionary_wait==QS_DICTIONARY_BUSY ||
       (s->pending_sort && status==QS_DICTIONARY_BUSY)) return dictionary_request(s,out,QS_DICTIONARY_BUSY);
    if(worker && status<=2) {
        uint64_t metadata=typed?QP_TYPED_OUTPUT|((uint64_t)control->output_rows<<5)|((uint64_t)control->output_strings<<22):0;
        if(typed && host_publish) qp_host_pack(out,&plan->nodes[plan->root].expr,control->output_strings);
        return metadata | (control->output_rows?QP_OUTPUT_READY:0) | (s->host_scan && s->scan_drained?QP_INPUT_DRAINED:0) |
            (out->finished?QP_WORKER_FINISHED:0) |
            (!control->ready && (control->bytes || control->index_nreads)?QP_CONTROL_REQUIRED:0);
    }
    if(out->finished && !worker) { s->closing=1; return finish_dictionary(s,a,out); }
    return status==QP_WAIT?0:status>2?status:0;
}
