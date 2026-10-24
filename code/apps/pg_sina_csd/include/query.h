/* Query data exchanged by the PG backend and device CSFs.
 * These are application records, not extra SNIA arguments or a device runtime. */
#ifndef PG_SINA_QUERY_H
#define PG_SINA_QUERY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define QS_JOINS 7
#define QS_COLS 64
#define QS_EXPRS 128
#define QS_AGGS 16
#define QS_KEYS 8
#define QS_OUT 16
#define QS_GROUPS 256
#define QS_RESULT_ROWS 256
#define QS_WORKERS 64 /* FEMU supports 1..64 CUs; active slots follow device resources. */
#define QS_PAGE 8192U
#define QS_CHUNK (4U*1024*1024)
#define QS_SORT_IO_BYTES (64U*1024)
#define QS_STRINGS 32
#define QS_STRING_MAX 15 /* Inline final-result bytes; longer values use the result arena. */
#define QS_DICT_SMALL_ENTRIES 512
#define QS_DICT_SMALL_DATA (64U*1024U)
#define QS_DICT_BIG_ENTRIES (1024U*1024U)
#define QS_DICT_BIG_DATA (20U*1024U*1024U)
#define QS_RESULT_STRING_BYTES (64U*1024U)
#define QS_INDEX_READS 64
#define QS_INDEX_CACHE 128
#define QS_INDEX_WORK_BYTES (128U*1024) /* Base cursor/tag state; pages stay in AFDM. */
static inline size_t qs_index_work_bytes(unsigned workers)
{ return QS_INDEX_WORK_BYTES+(size_t)(workers?workers-1:0)*4096; }
#define QS_INDEX_WAIT 1 /* Private join continuation; never a SNIA status. */
enum qs_type { Q_DECIMAL, Q_INTEGER, Q_DATE, Q_CHAR, Q_BOOLEAN, Q_STRING, Q_PREFIX, Q_PATTERN, Q_DICTIONARY };
enum qs_op { Q_COLUMN, Q_CONST, Q_ADD, Q_SUB, Q_MUL, Q_EQ, Q_NE, Q_LT, Q_LE, Q_GT, Q_GE,
             Q_AND, Q_OR, Q_NOT, Q_IS_NULL, Q_IS_NOT_NULL, Q_CASE, Q_YEAR, Q_AGGREF, Q_DIV,
             Q_PARAMETER, Q_LIKE, Q_SUBSTRING, Q_COALESCE, Q_INSET };
enum qs_agg_kind { Q_SUM, Q_AVG, Q_COUNT, Q_COUNT_STAR, Q_MIN, Q_MAX };
/* Filter-only fields keep their Heap references after selection; outputs
 * use QS_DEFERRED so their values survive reuse of the input block. */
enum qs_deferred { QS_EAGER, QS_DEFERRED, QS_FILTER_ONLY };
struct qs_column { int16_t length, slot; uint8_t align, type, scale, deferred, prefix_len, trim_spaces, restricted, dict_big; char prefix[32]; };
/* SQL literal dictionary: IDs 1..nstrings; 0 means another non-NULL string.
 * Only a proven equality/IN restriction permits returning/grouping a string. */
struct qs_string { uint8_t column, length; char bytes[128]; };
struct qs_expr { int16_t left, right, column, otherwise; uint8_t op, type, scale, dscale, isnull; int64_t constant; };
struct qs_agg { int16_t expr; uint8_t kind; };
struct qs_output { int16_t index; uint8_t aggregate; uint32_t type_oid; int32_t typmod; };
struct qs_order { uint8_t output, descending, nulls_first; };
struct qs_join {
    uint32_t relid, relfilenode, rti;
    unsigned nattrs;
    int build_key, probe_key, predicate;
    int build_key2, probe_key2; /* Optional second integer equality key. */
    int semi; /* 1: evaluate EXISTS here; -1: the native-Plan consumer confirms a match. */
    int left_pad; /* Emit one NULL-build row per probe row (left join count). */
    int residual; /* Joined-row predicate index + 1; 0 means no residual. */
    uint64_t rows, work_mem_bytes;
    uint32_t indexid, indexnode, indexroot;
    uint8_t index_key_bytes; /* PG12 ascending integer B-tree leading key. */
};
struct qs_plan {
    uint32_t relid, relfilenode;
    unsigned nattrs, ncols, nexprs, naggs, nkeys, nout, norder, nstrings;
    unsigned runtime_dict; /* At least one Q_STRING column uses the runtime dictionary. */
    unsigned dict_big; /* At least one Q_STRING column comes from a large relation. */
    int predicate;
    unsigned ordered_rows; /* 1: SQL row sort; 2: internal full-row input. */
    unsigned external_aggregate;
    unsigned nested; /* First-level groups are re-aggregated by nested_key_agg. */
    int nested_key_agg;
    int having; /* Post-aggregate boolean expression. */
    struct qs_join join; /* First build; its work_mem_bytes is the total build reservation. */
    unsigned njoins;
    struct qs_join extra_joins[QS_JOINS-1];
    uint64_t work_mem_bytes; /* Per execution slot, set at execution; 0 uses device capacity. */
    uint64_t work_mem_parent; /* Device-only allocator context; host plans leave this zero. */
    int64_t limit, offset;
    struct qs_column cols[QS_COLS];
    struct qs_string strings[QS_STRINGS];
    struct qs_expr exprs[QS_EXPRS];
    struct qs_agg aggs[QS_AGGS];
    int keys[QS_KEYS];
    struct qs_output outputs[QS_OUT];
    struct qs_order order[QS_OUT];
};
static inline unsigned qs_join_count(const struct qs_plan *p)
{ return p->njoins?p->njoins:p->join.relid?1:0; }
static inline const struct qs_join *qs_join_at(const struct qs_plan *p,unsigned i)
{ return i?&p->extra_joins[i-1]:&p->join; }
static inline unsigned qs_probe_attrs(const struct qs_plan *p)
{
    unsigned n=p->nattrs;
    for(unsigned i=0;i<qs_join_count(p);++i) n-=qs_join_at(p,i)->nattrs;
    return n;
}
struct qs_extent { uint64_t logical, lba, bytes; };
struct qs_request { struct qs_plan plan; char device[128]; uint32_t nsid, lba_bytes, nextents; uint64_t bytes; };
struct qs_index_read { uint32_t block; uint16_t slot, heap; };
struct qs_task {
    uint64_t state;
    uint32_t input_bytes, workers, slot; /* slot is a PG-assigned input index. */
    uint64_t scanned, matched, joined;
    uint64_t index_pages, index_tuples;
    uint32_t index_pending, index_nreads;
    struct qs_index_read index_reads[QS_INDEX_READS];
};
struct qs_join_control {
    uint64_t state, rows, spill_write_bytes, spill_read_bytes;
    uint32_t batches, current, indexed;
};
struct qs_group { uint64_t states[QS_WORKERS]; unsigned count; };
/* For a deferred numeric column, Scan stores this raw AFDM payload location
 * in its 8-byte value slot. Filter decodes only selected rows before Aggregate.
 * No device pointer escapes to the PG backend. */
struct qs_heap_ref { uint32_t offset, bytes; };
/* Column values, NULL bitmaps and numeric display scales, then selection. */
struct qs_chunk { uint32_t capacity, ncols, rows, selected; };
static inline size_t qs_chunk_bytes(unsigned capacity, unsigned ncols)
{ return sizeof(struct qs_chunk)+(size_t)capacity*ncols*8+((capacity+7)/8)*(size_t)ncols+(size_t)capacity*ncols+7+(size_t)capacity*4; }
static inline size_t qs_join_chunk_stride(const struct qs_plan *p)
{ return (qs_chunk_bytes(256,p->ncols)+7)&~(size_t)7; }
static inline int64_t *qs_values(struct qs_chunk *c) { return (int64_t *)(c+1); }
static inline unsigned char *qs_nulls(struct qs_chunk *c) { return (unsigned char *)(qs_values(c)+(size_t)c->capacity*c->ncols); }
static inline unsigned char *qs_dscales(struct qs_chunk *c) { return qs_nulls(c)+((c->capacity+7)/8)*(size_t)c->ncols; }
static inline uint32_t *qs_selection(struct qs_chunk *c)
{ return (uint32_t *)(((uintptr_t)(qs_dscales(c)+(size_t)c->capacity*c->ncols)+7)&~(uintptr_t)7); }
struct qs_cell { __int128 value; uint8_t scale, dscale, isnull; };
/* A final string result is packed into the existing 16-byte cell: value holds
 * the bytes and scale holds the length. Values longer than QS_STRING_MAX use
 * qs_result.str_data and store an offset/length reference in the cell. */
static inline void qs_cell_set_string(struct qs_cell *cell,const char *bytes,unsigned length)
{
    cell->value=0; cell->scale=(uint8_t)length; cell->dscale=0; cell->isnull=0;
    if(length) memcpy(&cell->value,bytes,length);
}
static inline void qs_cell_set_string_ref(struct qs_cell *cell,uint32_t offset,unsigned length)
{
    cell->value=offset; cell->scale=(uint8_t)length; cell->dscale=1; cell->isnull=0;
}
static inline void qs_cell_set_string_sort(struct qs_cell *cell,const char *bytes,unsigned length)
{
    cell->value=0; cell->scale=(uint8_t)length; cell->dscale=UINT8_MAX; cell->isnull=0;
    if(length) memcpy(&cell->value,bytes,length);
}
static inline unsigned qs_cell_string(const struct qs_cell *cell)
{ return cell->scale; }
static inline const char *qs_cell_string_bytes(const struct qs_cell *cell)
{ return (const char *)&cell->value; }
static inline int qs_cell_is_string_ref(const struct qs_cell *cell)
{ return cell->dscale==1; }
/* Shared by query CUs. Flat dictionaries keep entries/buckets/bytes after
 * this header. Native-tree execution supplies a workmem allocator and grows
 * flat snapshots lazily; old snapshots stay alive until query cleanup so
 * readers and borrowed text views remain valid across growth. */
struct qs_dict_entry { uint32_t offset; uint16_t length; };
struct qs_dict {
    uint32_t lock, count, bytes, entries, capacity, data;
    struct qs_dict *current;
    void *context;
    void *(*allocate)(void *,size_t);
};
static inline size_t qs_dict_bytes(unsigned entries,unsigned data)
{
    return sizeof(struct qs_dict)+(size_t)entries*sizeof(struct qs_dict_entry)+
        (size_t)entries*sizeof(uint32_t)+data;
}
static inline struct qs_dict_entry *qs_dict_entries(struct qs_dict *dict)
{ return (void *)(dict+1); }
static inline uint32_t *qs_dict_buckets(struct qs_dict *dict)
{ return (void *)(qs_dict_entries(dict)+dict->entries); }
static inline char *qs_dict_data(struct qs_dict *dict)
{ return (char *)(qs_dict_buckets(dict)+dict->capacity); }
static inline uint32_t qs_dict_hash(const char *bytes,unsigned length)
{
    uint32_t hash=2166136261U;
    for(unsigned i=0;i<length;++i) { hash^=(unsigned char)bytes[i]; hash*=16777619U; }
    return hash;
}
static inline struct qs_dict *qs_dict_storage(const struct qs_dict *dict)
{
    struct qs_dict *d=__atomic_load_n(&dict->current,__ATOMIC_ACQUIRE);
    return d?d:dict->allocate?NULL:(struct qs_dict *)dict;
}
/* Called only under the insertion lock. A published snapshot's layout never
 * changes. Keeping old allocations in the query context avoids a separate
 * reader lifetime protocol and charges their real cost to workmem. */
static inline struct qs_dict *qs_dict_grow(struct qs_dict *dict,unsigned count,unsigned bytes)
{
    if(!dict->allocate || count>dict->entries || bytes>dict->data) return NULL;
    struct qs_dict *old=qs_dict_storage(dict);
    unsigned entries=old?old->entries:dict->entries<512?dict->entries:512;
    unsigned data=old?old->data:dict->data<65536?dict->data:65536;
    while(entries<count || (count>entries*3/4 && entries<dict->entries))
        entries=entries<=dict->entries/2?entries*2:dict->entries;
    while(data<bytes) data=data<=dict->data/2?data*2:dict->data;
    struct qs_dict *d=dict->allocate(dict->context,qs_dict_bytes(entries,data));
    if(!d) return NULL;
    *d=(struct qs_dict){.entries=entries,.capacity=entries,.data=data};
    memset(qs_dict_buckets(d),0,(size_t)entries*sizeof(uint32_t));
    if(old) {
        memcpy(qs_dict_entries(d),qs_dict_entries(old),(size_t)dict->count*sizeof(struct qs_dict_entry));
        memcpy(qs_dict_data(d),qs_dict_data(old),dict->bytes);
        for(unsigned i=0;i<dict->count;++i) {
            const struct qs_dict_entry *e=&qs_dict_entries(d)[i];
            uint32_t at=qs_dict_hash(qs_dict_data(d)+e->offset,e->length)&(entries-1);
            while(qs_dict_buckets(d)[at]) at=(at+1)&(entries-1);
            qs_dict_buckets(d)[at]=i+1;
        }
    }
    __atomic_store_n(&dict->current,d,__ATOMIC_RELEASE);
    return d;
}
/* Existing strings can be found without the insertion lock. A reader on an
 * older snapshot can miss a new value; insertion rechecks under the lock. */
static inline int qs_dict_find(struct qs_dict *dict,const char *bytes,unsigned length)
{
    struct qs_dict *d=qs_dict_storage(dict); if(!d) return 0;
    struct qs_dict_entry *entries=qs_dict_entries(d);
    uint32_t *buckets=qs_dict_buckets(d),mask=d->capacity-1;
    uint32_t at=qs_dict_hash(bytes,length)&mask;
    for(unsigned probes=0;probes<d->capacity;++probes) {
        uint32_t id=__atomic_load_n(&buckets[at],__ATOMIC_ACQUIRE);
        if(!id) return 0;
        struct qs_dict_entry *e=&entries[id-1];
        if(e->length==length && (!length || !memcmp(qs_dict_data(d)+e->offset,bytes,length))) return (int)id;
        at=(at+1)&mask;
    }
    return 0;
}
static inline int qs_dict_insert(struct qs_dict *dict,const char *bytes,unsigned length)
{
    int id=qs_dict_find(dict,bytes,length);
    if(id) return id;
    while(__sync_lock_test_and_set(&dict->lock,1U)) { }
    id=qs_dict_find(dict,bytes,length);
    if(!id) {
        unsigned count=dict->count,used=dict->bytes;
        struct qs_dict *d=qs_dict_storage(dict);
        if(count>=dict->entries || length>65535U || length>dict->data-used) id=-1;
        else {
            if(!d || count>=d->entries || length>d->data-used ||
               (dict->allocate && count+1>d->entries*3/4 && d->entries<dict->entries))
                d=qs_dict_grow(dict,count+1,used+length);
            if(!d) id=-1;
            else {
                qs_dict_entries(d)[count]=(struct qs_dict_entry){used,(uint16_t)length};
                memcpy(qs_dict_data(d)+used,bytes,length);
                __atomic_store_n(&dict->bytes,used+length,__ATOMIC_RELAXED);
                uint32_t *buckets=qs_dict_buckets(d),mask=d->capacity-1;
                uint32_t at=qs_dict_hash(bytes,length)&mask;
                while(buckets[at]) at=(at+1)&mask;
                __atomic_store_n(&dict->count,count+1,__ATOMIC_RELEASE);
                __atomic_store_n(&buckets[at],count+1,__ATOMIC_RELEASE);
                id=(int)count+1;
            }
        }
    }
    __sync_lock_release(&dict->lock);
    return id;
}
static inline int qs_dict_get(const struct qs_dict *dict,unsigned id,const char **bytes,unsigned *length)
{
    if(!dict || !id || id>__atomic_load_n(&dict->count,__ATOMIC_ACQUIRE)) return -1;
    const struct qs_dict *d=qs_dict_storage(dict); if(!d || id>d->entries) return -1;
    const struct qs_dict_entry *e=&((const struct qs_dict_entry *)(d+1))[id-1];
    const char *base=(const char *)(((const struct qs_dict_entry *)(d+1))+d->entries)+
        (size_t)d->capacity*sizeof(uint32_t);
    *bytes=base+e->offset; *length=e->length; return 0;
}
struct qs_row { struct qs_cell cells[QS_OUT]; };
struct qs_metrics {
    uint64_t scanned, matched, input_bytes, chunks, work_peak, fdm_bytes;
    uint64_t dictionary_entries,dictionary_bytes;
    uint64_t spill_write_bytes, spill_read_bytes, runs, spilled_runs;
    uint64_t build_rows, join_rows, build_batches;
    uint64_t index_pages, index_tuples;
    unsigned workers;
};
/* Optional per-query measurements. Each CU owns its own AFDM counters. */
enum qs_operator { QS_SCAN, QS_FILTER, QS_AGG_SINK, QS_AGG_SOURCE, QS_SORT, QS_LIMIT,
                   QS_SORT_SINK, QS_SORT_SOURCE, QS_JOIN_BUILD, QS_JOIN_LOAD, QS_JOIN_PROBE, QS_OPERATORS };
struct qs_timing { uint64_t calls, wall_ns, cpu_ns, max_wall_ns; };
struct qs_profile {
    struct qs_timing operators[QS_OPERATORS];
    uint64_t setup_ns, build_ns, pipeline_ns[2], cleanup_ns, total_ns;
};
struct qs_result { int status; unsigned rows, begin, finished; char error[160]; struct qs_metrics metrics; struct qs_profile profile; struct qs_row data[QS_RESULT_ROWS]; uint32_t str_bytes; char str_data[QS_RESULT_STRING_BYTES]; };
#endif
