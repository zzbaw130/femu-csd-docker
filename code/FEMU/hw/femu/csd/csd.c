#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/rcu.h"
#include <gmodule.h>

#include "csd.h"
#include "csd-cache.h"
#include "../bbssd/ftl.h"

#ifdef CONFIG_FEMU_CSD_UBPF
#include <ubpf.h>
#endif

typedef int64_t (*FemuCsdSharedLibFn)(FemuCsdArgs *args);

#define CSD_EXEC_DATA_MAX (1U << 20)

/*
 * A program is held whole in host memory while it loads, and the load command
 * carries its size in a 32-bit field, so an unbounded one lets the guest ask
 * for four gigabytes per program index and there are 65535 of those. Cap what
 * one program may be and what all of them together may be.
 */
#define CSD_PROGRAM_MAX (16U << 20)
#define CSD_PROGRAM_TOTAL_MAX (64U << 20)

typedef struct FemuCsdState FemuCsdState;
typedef struct FemuCsdGraph FemuCsdGraph;
typedef struct FemuCacheItem FemuCacheItem;
typedef struct FemuCacheRing {
    struct femu_cache_rx rx;
    QemuMutex slots_lock; /* Guest credit probes never hold the CU/FTL lock. */
    bool busy[FEMU_CACHE_RX_SLOTS], stopping;
    unsigned active, next;
    uint8_t *sent; /* One bit per namespace page, scoped to this receiver. */
    uint64_t npages, offered_bytes, duplicate_bytes, dma_bytes;
} FemuCacheRing;
struct FemuCacheItem {
    struct {
        uint64_t token,cookie,offset,lba,bytes,load_ns;
        uint32_t afdm,nsid;
    } event;
    FemuCacheRing *ring;
    uint8_t *data;
    int slot;
    FemuCsdGraph *graph;
    unsigned session;
    unsigned node;
    bool internal_read;
    uint64_t dma_start_ns, dma_ns, copy_end_ns, link_end_ns, cu_ns, consumers, dma_bytes;
    uint64_t dma_lock_ns,update_lock_ns,publish_start_ns,publish_end_ns,segments,copy_cpu_ns;
    unsigned dma_active; /* pins the retained pages during worker DMA */
    bool published, dropped, data_done;
};

/*
 * Device memory and programs are counted references: the table holds one, and
 * each Execute in flight holds one per range or program it uses, so freeing
 * one does not pull it out from under a program still running on it. The
 * quota they count against is given back only when the last one goes.
 */
typedef struct FemuCsdAfdm {
    uint32_t id;
    uint32_t refs;
    uint64_t size;
    uint8_t *data;
    FemuCsdState *csd;
} FemuCsdAfdm;

typedef struct FemuCsdProgram {
    uint32_t id;
    uint32_t refs;
    FemuCsdState *csd;
    QSLIST_ENTRY(FemuCsdProgram) dead;
    uint8_t type;
    bool active;
    bool indirect;
    bool loading;
    uint32_t runtime;
    uint16_t runtime_scale;
    uint64_t size;
    uint64_t load_size;
    uint64_t pid;
    uint8_t *data;
    GModule *module;
    FemuCsdSharedLibFn shared_lib_fn;
#ifdef CONFIG_FEMU_CSD_UBPF
    struct ubpf_vm *ubpf_vm;
    ubpf_jit_fn ubpf_jit_fn;
#endif
} FemuCsdProgram;

typedef struct FemuCsdGroup {
    uint32_t id;
    int8_t prio;
    uint8_t qos_flags;
    uint32_t bandwidth;
    uint32_t deadline;
} FemuCsdGroup;

typedef struct FemuCsdMrs {
    uint16_t rsid;
    uint32_t numr;
    NvmeCsdMemoryRange *ranges;
} FemuCsdMrs;

/* an Execute handed from the poller to a compute unit thread */
typedef struct FemuCsdJob {
    NvmeRequest *req;           /* NULL once its submission queue is gone */
    int poller;                 /* whose completion ring it goes back on */
    int64_t arrival;
    uint64_t runtime;
    int64_t reserved_end;       /* compute unit hold taken at submission */
    FemuCsdProgram *program;
    FemuCsdAfdm **afdms;
    FemuCsdArgs args;
    FemuCsdGraph *graph;
    unsigned graph_node;
    uint8_t *data;
    QTAILQ_ENTRY(FemuCsdJob) entry;
} FemuCsdJob;

typedef struct FemuCsdSession {
    uint32_t id;
    unsigned graphs;
    FemuCacheRing *cache_ring;
    NvmeNamespace *ns;
    /* Enabled only by the existing SNIA_FEMU_PROFILE switch on graph requests. */
    bool profile;
    uint64_t completed_graphs, graph_ns, events, io_calls, io_ns, io_work_ns;
    uint64_t cu_calls, cu_ns, cu_queue_ns, late_ns, dispatch_ns;
    GHashTable *cancelled; /* cancellation racing graph acceptance */
} FemuCsdSession;

typedef struct FemuCsdSpill {
    uint64_t id, page, pages, bytes, creator;
    FemuCsdSession *session;
    unsigned busy;
} FemuCsdSpill;

enum { GRAPH_READY, GRAPH_COMPUTING, GRAPH_COMPUTED, GRAPH_IO, GRAPH_CACHE_WAIT,
       GRAPH_IO_DONE, GRAPH_DONE };
typedef struct FemuCsdReadCompletion {
    FemuCsdAfdm *afdm;
    uint64_t lba, offset, bytes;
} FemuCsdReadCompletion;
typedef struct FemuCsdGraphNode {
    struct femu_graph_node wire;
    FemuCsdJob *job;
    FemuCsdAfdm *afdm;
    unsigned state, range;
    uint64_t deadline, result, transferred, range_blocks;
    uint64_t cache_lba, cache_bytes, cache_offset;
    FemuCsdAfdm *cache_afdm;
    uint64_t cache_wait_begin;
    bool cache_candidate;
    uint64_t io_action, io_token, io_bytes;
    FemuCsdSpill *spill;
    struct femu_graph_read *reads;
    FemuCsdReadCompletion *read_completions;
    unsigned read_completed;
    bool serial_reads;
    uint16_t status;
    FemuCsdGraph *graph;
    uint64_t dependents;
    unsigned remaining;
    size_t pos;
} FemuCsdGraphNode;

struct FemuCsdGraph {
    NvmeRequest *req;
    int poller;
    FemuCsdSession *session;
    uint8_t *data;
    uint32_t bytes, count, flags;
    uint64_t cookie, done, result, ready;
    unsigned active;
    bool queued;
    unsigned cache_pending;
    uint64_t cu_done_ns;
    uint16_t status;
    FemuCsdGraphNode nodes[FEMU_CSD_GRAPH_MAX_NODES];
    QTAILQ_ENTRY(FemuCsdGraph) entry, ready_entry;
};

struct FemuCsdState {
    FemuCtrl *n;
    CsdCtrlParams params;
    uint64_t fdm_capacity;
    uint64_t fdm_used;
    uint64_t prog_used;         /* bytes held by loaded programs */
    uint32_t next_afdm_id;
    uint32_t next_group_id;
    uint32_t next_rsid;
    GHashTable *afdms;
    GHashTable *programs;
    GHashTable *groups;
    GHashTable *mrs;
    QemuMutex lock;
    QemuMutex cache_lock; /* Worker metadata never takes the CU scheduler lock. */
    QemuCond cache_cond;
    QemuThread cache_workers[2];
    uint64_t cache_next;
    unsigned cache_dropped;
    unsigned cache_work; /* retained LOAD blocks */
    unsigned cache_dirty; /* retirement scan only after a state change */
    FemuCacheItem cache[128];
    uint64_t cache_free[2]; /* cache_lock; preserve lowest-free-index order. */
    uint64_t *cu_next_avail;    /* when each compute unit is free again */
    QemuThread *workers;        /* one per compute unit */
    QemuCond job_cond;
    QTAILQ_HEAD(, FemuCsdJob) pending;
    QTAILQ_HEAD(, FemuCsdJob) running;
    /* programs whose last reference went, destroyed by a worker unlocked */
    QSLIST_HEAD(, FemuCsdProgram) dead;
    bool stopping;
    bool overrun_warned;
    QTAILQ_HEAD(, FemuCsdGraph) graphs;
    unsigned graph_count, ready_count;
    QTAILQ_HEAD(, FemuCsdGraph) graph_ready;
    pqueue_t *graph_events;
    uint64_t graph_deadline; /* cached heap minimum for the FTL idle path */
    GHashTable *sessions, *spills;
    uint32_t next_session;
    uint64_t next_spill, spill_pages, spill_base;
    uint8_t *spill_space, *spill_bitmap;
};

static void csd_cache_ring_stop(FemuCsdState *,FemuCsdSession *);
static void *csd_worker(void *opaque);
static void *csd_cache_worker(void *opaque);
static void csd_job_free_locked(FemuCsdJob *job);
static void csd_graph_destroy_locked(FemuCsdState *csd, FemuCsdGraph *g);
static void csd_graph_computed_locked(FemuCsdState *csd, FemuCsdJob *job,
                                      uint16_t status, int64_t result,
                                      int64_t host_ns);
/* Same min-heap implementation as the NVMe completion poller. The CSD lock
 * protects insertions from CU workers; only the sole FTL thread consumes it. */
static int graph_cmp(pqueue_pri_t a, pqueue_pri_t b) { return a > b; }
static pqueue_pri_t graph_pri(void *p) { return ((FemuCsdGraphNode *)p)->deadline; }
static void graph_set_pri(void *p, pqueue_pri_t n) { ((FemuCsdGraphNode *)p)->deadline = n; }
static size_t graph_pos(void *p) { return ((FemuCsdGraphNode *)p)->pos; }
static void graph_set_pos(void *p, size_t n) { ((FemuCsdGraphNode *)p)->pos = n; }
static void csd_graph_cache_deadline(FemuCsdState *csd)
{
    FemuCsdGraphNode *v = pqueue_peek(csd->graph_events);
    qatomic_set(&csd->graph_deadline, v ? v->deadline : UINT64_MAX);
}
static void csd_graph_event_locked(FemuCsdState *csd, FemuCsdGraphNode *v)
{
    /* At most 64 graphs * 64 nodes; preallocated, so insertion cannot grow. */
    int rc = pqueue_insert(csd->graph_events, v);
    g_assert(rc == 0);
    csd_graph_cache_deadline(csd);
}
static void csd_graph_ready_locked(FemuCsdState *csd, FemuCsdGraph *g)
{
    if (!g->queued) {
        g->queued = true;
        QTAILQ_INSERT_TAIL(&csd->graph_ready, g, ready_entry);
        qatomic_inc(&csd->ready_count);
    }
}
static void csd_session_free(gpointer pointer)
{
    FemuCsdSession *s = pointer;
    g_hash_table_destroy(s->cancelled);
    g_free(s);
}

static void csd_check_size(void)
{
    QEMU_BUILD_BUG_ON(sizeof(struct femu_graph_header) != 32);
    QEMU_BUILD_BUG_ON(sizeof(struct femu_cache_slot)*FEMU_CACHE_RX_SLOTS>FEMU_CACHE_PAGE_BYTES);
    QEMU_BUILD_BUG_ON(sizeof(struct femu_graph_node) != 64);
    QEMU_BUILD_BUG_ON(sizeof(struct femu_graph_range) != 16);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdAllocFdmCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdDeallocAfdmCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdNvmToAfdmCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdExecCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdMemoryRange) != 32);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdReadAfdmCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdWriteAfdmCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdCreateGroupCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdSetQosCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdDeleteGroupCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdLoadProgramCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdProgramActivationCmd) != 64);
    QEMU_BUILD_BUG_ON(sizeof(NvmeCsdMrsMgmtCmd) != 64);
}

static FemuCsdState *csd_state(FemuCtrl *n)
{
    return n->csd_ctrl_state;
}

/* AFDM references held by the graph cover both CU reads and direct DMA. */
static void csd_afdm_unref(gpointer opaque)
{
    FemuCsdAfdm *afdm=opaque;
    if(!afdm || --afdm->refs) return;
    afdm->csd->fdm_used-=afdm->size;
    g_free(afdm->data); g_free(afdm);
}

static void csd_program_unload(FemuCsdProgram *program)
{
    if (program->module) {
        g_module_close(program->module);
        program->module = NULL;
        program->shared_lib_fn = NULL;
    }

#ifdef CONFIG_FEMU_CSD_UBPF
    if (program->ubpf_vm) {
        ubpf_destroy(program->ubpf_vm);
        program->ubpf_vm = NULL;
        program->ubpf_jit_fn = NULL;
    }
#endif
}

static void csd_program_destroy(FemuCsdProgram *program)
{
    csd_program_unload(program);
    g_free(program->data);
    g_free(program);
}

/*
 * Closing a library runs its destructors, which may take any time, so the
 * last reference only queues the program; a worker closes it unlocked.
 */
static void csd_program_unref(gpointer opaque)
{
    FemuCsdProgram *program = opaque;
    FemuCsdState *csd;

    if (!program || --program->refs) {
        return;
    }

    csd = program->csd;
    csd->prog_used -= program->size;
    QSLIST_INSERT_HEAD(&csd->dead, program, dead);
    qemu_cond_signal(&csd->job_cond);
}

static void csd_mrs_free(gpointer opaque)
{
    FemuCsdMrs *mrs = opaque;

    if (!mrs) {
        return;
    }

    g_free(mrs->ranges);
    g_free(mrs);
}

static void csd_init_ctrl_str(FemuCtrl *n, NvmeNamespace *ns)
{
    static int csd_id;
    const char *mn = "FEMU Computational Storage Controller";
    const char *sn = "vCSD";

    nvme_set_ctrl_name(n, ns, mn, sn, &csd_id);
}

static void csd_init(FemuCtrl *n, NvmeNamespace *ns, Error **errp)
{

    FemuCsdState *csd;
    struct ssd *ssd;
    uint32_t i;

    csd_check_size();

    /* CSD builds a bbssd ssd from the shared geometry, so it needs the same
     * bounds check bbssd does; without it an oversized axis aliases in the PPA. */
    if (bb_check_geometry(n, errp)) {
        return;
    }

    /*
     * And the same capacity check. This mode runs the black-box FTL verbatim
     * -- the request router sends both through it -- so it needs the reserve
     * that check enforces just as much. Without it a namespace exposing the
     * whole of the media was accepted here while black box refused it, and
     * the collector then had nowhere to relocate to.
     */
    if (bb_check_capacity(n, ns, errp)) {
        return;
    }

    if (n->csd_params.spill_size_mb) {
        NvmeNamespace check = *ns;
        uint64_t page = (uint64_t)n->bb_params.secsz * n->bb_params.secs_per_pg;
        uint64_t bytes;
        if (n->num_namespaces != 1 || n->ns_mgmt || n->power_loss || n->streams ||
            n->blk_pe_limit || nvme_ns_ms(ns) ||
            (n->subsys && n->subsys->endgrp.fdp.enabled) ||
            (n->bb_params.mapping_scheme && strcmp(n->bb_params.mapping_scheme, "page")) ||
            n->csd_params.spill_size_mb > (UINT64_MAX - ns->size - page) / MiB) {
            error_setg(errp, "CSD hidden spill requires one page-mapped namespace without power_loss, metadata, Streams, FDP or retirement");
            return;
        }
        bytes = n->csd_params.spill_size_mb * MiB;
        check.size = QEMU_ALIGN_UP(ns->size, page) + bytes;
        if (bb_check_capacity(n, &check, errp)) {
            return;
        }
    }

    /* It also takes the black-box FDP write path, so the same refusals. */
    if (bb_init_fdp(n, ns, "csd", errp)) {
        return;
    }

    if (n->csd_params.fdm_size_mb == 0) {
        error_setg(errp, "CSD mode requires fdm_size to be non-zero");
        return;
    }

    if (n->csd_params.fdm_size_mb > UINT64_MAX / MiB) {
        error_setg(errp, "CSD fdm_size is too large");
        return;
    }

    if (n->csd_params.nr_cu == 0 || n->csd_params.nr_cu > 64) {
        error_setg(errp, "CSD nr_cu must be in range [1, 64]");
        return;
    }

    if (n->csd_params.nr_thread == 0) {
        error_setg(errp, "CSD nr_thread must be non-zero");
        return;
    }

    if (n->csd_params.csf_runtime_scale == 0) {
        error_setg(errp, "CSD csf_runtime_scale must be non-zero");
        return;
    }

    if (!n->ns_mgmt) {
        csd_init_ctrl_str(n, ns);
    }

    ssd = ns->ssd = g_malloc0(sizeof(*ssd));
    if (!n->ssd) {
        n->ssd = ssd;
    }
    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    ssd_init(n, ns);

    csd = g_new0(FemuCsdState, 1);
    csd->params = n->csd_params;
    csd->cache_next = 1;
    csd->cache_free[0]=csd->cache_free[1]=UINT64_MAX;
    csd->fdm_capacity = n->csd_params.fdm_size_mb * MiB;
    csd->next_afdm_id = 1;
    csd->next_group_id = 1;
    csd->next_rsid = 1;
    csd->afdms = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                       csd_afdm_unref);
    csd->programs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                          csd_program_unref);
    csd->groups = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                        g_free);
    csd->mrs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                     csd_mrs_free);
    csd->cu_next_avail = g_new0(uint64_t, csd->params.nr_cu);
    csd->sessions = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, csd_session_free);
    csd->spills = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
    csd->next_session = 1;
    csd->next_spill = 1;
    if (csd->params.spill_size_mb) {
        uint64_t page = (uint64_t)ssd->sp.secsz * ssd->sp.secs_per_pg;
        uint64_t bytes = csd->params.spill_size_mb * MiB;
        csd->spill_pages = bytes / page;
        csd->spill_base = DIV_ROUND_UP(ns->size, page);
        csd->spill_space = g_malloc0(bytes);
        csd->spill_bitmap = g_malloc0(DIV_ROUND_UP(csd->spill_pages, 8));
    }
    qemu_mutex_init(&csd->lock);
    qemu_mutex_init(&csd->cache_lock);
    csd->n = n;
    qemu_cond_init(&csd->job_cond);
    qemu_cond_init(&csd->cache_cond);
    QTAILQ_INIT(&csd->pending);
    QTAILQ_INIT(&csd->running);
    QTAILQ_INIT(&csd->graphs);
    QTAILQ_INIT(&csd->graph_ready);
    csd->graph_events = pqueue_init(64 * FEMU_CSD_GRAPH_MAX_NODES + 1,
        graph_cmp, graph_pri, graph_set_pri, graph_pos, graph_set_pos);
    g_assert(csd->graph_events);
    csd->graph_deadline = UINT64_MAX;
    QSLIST_INIT(&csd->dead);
    csd->workers = g_new0(QemuThread, csd->params.nr_cu);
    for (i = 0; i < csd->params.nr_cu; i++) {
        qemu_thread_create(&csd->workers[i], "femu-csd-cu", csd_worker, csd,
                           QEMU_THREAD_JOINABLE);
    }
    n->csd_ctrl_state = csd;
    for (i = 0; i < ARRAY_SIZE(csd->cache_workers); i++) {
        qemu_thread_create(&csd->cache_workers[i], "femu-cache-dma", csd_cache_worker, csd,
                           QEMU_THREAD_JOINABLE);
    }

    femu_log("%s,CSD mode initialized: fdm=%" PRIu64 "MB, "
             "nr_cu=%u, nr_thread=%u\n",
             n->devname, csd->params.fdm_size_mb, csd->params.nr_cu,
             csd->params.nr_thread);
}

static void csd_exit(FemuCtrl *n)
{
    FemuCsdState *csd = csd_state(n);
    int i;

    /* Join both compute and DMA workers before releasing device state. */
    if (csd) {
        qemu_mutex_lock(&csd->lock);
        qemu_mutex_lock(&csd->cache_lock);
        csd->stopping = true;
        qemu_cond_broadcast(&csd->job_cond);
        qemu_cond_broadcast(&csd->cache_cond);
        qemu_mutex_unlock(&csd->cache_lock);
        qemu_mutex_unlock(&csd->lock);
        for (i = 0; i < ARRAY_SIZE(csd->cache_workers); i++) {
            qemu_thread_join(&csd->cache_workers[i]);
        }
        for (i = 0; i < csd->params.nr_cu; i++) {
            qemu_thread_join(&csd->workers[i]);
        }
    }

    /*
     * A computational namespace runs the black-box FTL through its own
     * ns->ssd, allocated here rather than by bbssd, so this mode releases it.
     * A controller with no black-box namespace never dispatches bb_exit(), so
     * leaving it to that one leaked the whole FTL.
     */
    for (i = 0; i < n->num_namespaces; i++) {
        NvmeNamespace *ns = &n->namespaces[i];

        if (!NS_CSD(ns) || !ns->ssd) {
            continue;
        }
        if (n->ssd == ns->ssd) {
            n->ssd = NULL;
        }
        ssd_free(ns->ssd);
        g_free(ns->ssd);
        ns->ssd = NULL;
    }

    if (!csd) {
        return;
    }

    qemu_mutex_lock(&csd->lock);
    while (!QTAILQ_EMPTY(&csd->pending)) {
        FemuCsdJob *job = QTAILQ_FIRST(&csd->pending);

        QTAILQ_REMOVE(&csd->pending, job, entry);
        if (!job->graph) {
            csd_job_free_locked(job);
        }
    }
    GHashTableIter cache_it; gpointer cache_session;
    g_hash_table_iter_init(&cache_it,csd->sessions);
    while(g_hash_table_iter_next(&cache_it,NULL,&cache_session)) csd_cache_ring_stop(csd,cache_session);
    while (!QTAILQ_EMPTY(&csd->graphs)) {
        csd_graph_destroy_locked(csd, QTAILQ_FIRST(&csd->graphs));
    }
    qemu_mutex_unlock(&csd->lock);

    pqueue_free(csd->graph_events);
    g_hash_table_destroy(csd->afdms);
    g_hash_table_destroy(csd->spills);
    g_hash_table_destroy(csd->sessions);
    g_free(csd->spill_space);
    g_free(csd->spill_bitmap);
    g_hash_table_destroy(csd->programs);
    g_hash_table_destroy(csd->groups);
    g_hash_table_destroy(csd->mrs);
    while (!QSLIST_EMPTY(&csd->dead)) {
        FemuCsdProgram *program = QSLIST_FIRST(&csd->dead);

        QSLIST_REMOVE_HEAD(&csd->dead, dead);
        csd_program_destroy(program);
    }
    qemu_mutex_destroy(&csd->cache_lock);
    qemu_mutex_destroy(&csd->lock);
    qemu_cond_destroy(&csd->job_cond);
    qemu_cond_destroy(&csd->cache_cond);
    g_free(csd->workers);
    g_free(csd->cu_next_avail);
    g_free(csd);
    n->csd_ctrl_state = NULL;
}

static FemuCsdProgram *csd_get_program_locked(FemuCsdState *csd, uint32_t id)
{
    if (id == 0) {
        return NULL;
    }

    return g_hash_table_lookup(csd->programs, GUINT_TO_POINTER(id));
}

static FemuCsdAfdm *csd_get_afdm_locked(FemuCsdState *csd, uint32_t id)
{
    if (id == 0) {
        return NULL;
    }

    return g_hash_table_lookup(csd->afdms, GUINT_TO_POINTER(id));
}

static FemuCsdGroup *csd_get_group_locked(FemuCsdState *csd, uint32_t id)
{
    if (id == 0) {
        return NULL;
    }

    return g_hash_table_lookup(csd->groups, GUINT_TO_POINTER(id));
}

static FemuCsdMrs *csd_get_mrs_locked(FemuCsdState *csd, uint32_t id)
{
    if (id == 0) {
        return NULL;
    }

    return g_hash_table_lookup(csd->mrs, GUINT_TO_POINTER(id));
}

static uint16_t csd_check_afdm_range(FemuCsdAfdm *afdm, uint64_t offset,
                                     uint64_t size)
{
    if (!afdm || size == 0 || offset > afdm->size ||
        size > afdm->size - offset) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (size > UINT32_MAX) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static uint16_t csd_parse_program(FemuCsdProgram *program, const char **path,
                                  const char **symbol)
{
    char *name;
    size_t path_len;
    size_t symbol_len;

    if (!program->data || program->size < 3) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    name = memchr(program->data, '\0', program->size);
    if (!name || name == (char *)program->data) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    path_len = name - (char *)program->data;
    if (path_len + 1 >= program->size) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    *path = (const char *)program->data;
    *symbol = name + 1;
    symbol_len = strnlen(*symbol, program->size - path_len - 1);
    if (symbol_len == 0 || path_len + symbol_len + 2 > program->size) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return NVME_SUCCESS;
}

/*
 * The name in a program comes from the guest, and loading one means the host
 * process opens that file and calls a symbol out of it. A guest must not be
 * able to name any file on the host, so the operator says where programs live
 * and the guest may only name one of the files in it: no separator, no parent
 * directory, no symbolic link out. With no directory configured the device
 * accepts only the program type that runs nothing.
 */
static uint16_t csd_program_path(FemuCtrl *n, const char *name, char **path)
{
    const char *dir = n->csd_params.program_dir;
    g_autofree char *joined = NULL;
    g_autofree char *real = NULL;
    g_autofree char *realdir = NULL;

    if (!dir || !dir[0]) {
        femu_err("CSD: loading a program needs csd_program_dir to be set\n");
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (!name[0] || strchr(name, '/') || !strcmp(name, ".") ||
        !strcmp(name, "..")) {
        femu_err("CSD: a program name must be a file in csd_program_dir, "
                 "got \"%s\"\n", name);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    joined = g_build_filename(dir, name, NULL);
    real = realpath(joined, NULL);
    realdir = realpath(dir, NULL);
    if (!real || !realdir || !g_str_has_prefix(real, realdir) ||
        real[strlen(realdir)] != '/') {
        femu_err("CSD: %s does not resolve inside csd_program_dir\n", joined);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    *path = g_steal_pointer(&real);

    return NVME_SUCCESS;
}

static uint16_t csd_load_shared_lib(FemuCtrl *n, FemuCsdProgram *program)
{
    const char *name;
    const char *symbol;
    g_autofree char *path = NULL;
    gpointer fn = NULL;
    uint16_t status;

    status = csd_parse_program(program, &name, &symbol);
    if (status) {
        return status;
    }

    status = csd_program_path(n, name, &path);
    if (status) {
        return status;
    }

    program->module = g_module_open(path, G_MODULE_BIND_LOCAL);
    if (!program->module) {
        femu_err("CSD: failed to load shared library %s: %s\n", path,
                 g_module_error());
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (!g_module_symbol(program->module, symbol, &fn) || !fn) {
        femu_err("CSD: failed to find shared library symbol %s: %s\n", symbol,
                 g_module_error());
        csd_program_unload(program);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    program->shared_lib_fn = (FemuCsdSharedLibFn)fn;
    return NVME_SUCCESS;
}

static uint16_t csd_load_ubpf(FemuCtrl *n, FemuCsdProgram *program, bool jit)
{
#ifdef CONFIG_FEMU_CSD_UBPF
    const char *name;
    const char *symbol;
    g_autofree char *path = NULL;
    g_autofree char *elf = NULL;
    gsize elf_size = 0;
    g_autoptr(GError) err = NULL;
    char *errmsg = NULL;
    uint16_t status;

    status = csd_parse_program(program, &name, &symbol);
    if (status) {
        return status;
    }

    status = csd_program_path(n, name, &path);
    if (status) {
        return status;
    }

    if (!g_file_get_contents(path, &elf, &elf_size, &err)) {
        femu_err("CSD: failed to read uBPF program %s: %s\n", path,
                 err ? err->message : "unknown error");
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    program->ubpf_vm = ubpf_create();
    if (!program->ubpf_vm) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (ubpf_load_elf_ex(program->ubpf_vm, elf, elf_size, symbol, &errmsg) < 0) {
        femu_err("CSD: failed to load uBPF ELF %s:%s: %s\n", path, symbol,
                 errmsg ? errmsg : "unknown error");
        free(errmsg);
        csd_program_unload(program);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (jit) {
        program->ubpf_jit_fn = ubpf_compile(program->ubpf_vm, &errmsg);
        if (!program->ubpf_jit_fn) {
            femu_err("CSD: failed to JIT uBPF ELF %s:%s: %s\n", path, symbol,
                     errmsg ? errmsg : "unknown error");
            free(errmsg);
            csd_program_unload(program);
            return NVME_INVALID_FIELD | NVME_DNR;
        }
    }

    return NVME_SUCCESS;
#else
    (void)n;
    return NVME_INVALID_FIELD | NVME_DNR;
#endif
}

static uint16_t csd_load_program_data(FemuCtrl *n, FemuCsdProgram *program,
                                      bool jit)
{
    switch (program->type) {
    case NVME_CSD_CSF_TYPE_PHANTOM:
        return NVME_SUCCESS;
    case NVME_CSD_CSF_TYPE_SHARED_LIB:
        return csd_load_shared_lib(n, program);
    case NVME_CSD_CSF_TYPE_EBPF:
        return csd_load_ubpf(n, program, jit);
    default:
        return NVME_INVALID_FIELD | NVME_DNR;
    }
}

static uint16_t csd_compute_load(FemuCtrl *n, NvmeCmd *cmd)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdLoadProgramCmd *load = (NvmeCsdLoadProgramCmd *)cmd;
    uint16_t pind = le16_to_cpu(load->pind);
    uint32_t psize = le32_to_cpu(load->psize);
    uint32_t numb = le32_to_cpu(load->numb);
    uint32_t loff = le32_to_cpu(load->loff);
    uint64_t pid = le64_to_cpu(load->pid);
    uint64_t prp1 = le64_to_cpu(load->prp1);
    uint64_t prp2 = le64_to_cpu(load->prp2);
    FemuCsdProgram *program;
    uint16_t status = NVME_SUCCESS;

    if (pind == 0 || psize > CSD_PROGRAM_MAX || loff > psize ||
        numb > psize - loff) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    qemu_mutex_lock(&csd->lock);

    if (load->sel) {
        program = csd_get_program_locked(csd, pind);
        if (!program) {
            qemu_mutex_unlock(&csd->lock);
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        if (program->active) {
            qemu_mutex_unlock(&csd->lock);
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        g_hash_table_remove(csd->programs, GUINT_TO_POINTER((uint32_t)pind));
        qemu_mutex_unlock(&csd->lock);
        return NVME_SUCCESS;
    }

    if (loff == 0) {
        FemuCsdProgram *held = csd_get_program_locked(csd, pind);
        uint64_t used = csd->prog_used;

        /*
         * The replace below frees whatever this index already held, unless an
         * Execute still holds it, in which case it counts until that returns.
         */
        if (held && held->refs == 1) {
            used -= held->size;
        }
        if (psize > CSD_PROGRAM_TOTAL_MAX - used) {
            qemu_mutex_unlock(&csd->lock);
            return NVME_CAP_EXCEEDED | NVME_DNR;
        }
        csd->prog_used += psize;

        program = g_new0(FemuCsdProgram, 1);
        program->id = pind;
        program->refs = 1;
        program->csd = csd;
        program->type = load->ptype;
        program->runtime = le32_to_cpu(load->runtime);
        program->runtime_scale = le16_to_cpu(load->runtime_scale);
        program->size = psize;
        program->pid = pid;
        program->indirect = load->indirect;
        program->loading = true;
        if (psize) {
            program->data = g_malloc0(psize);
        }
        g_hash_table_replace(csd->programs, GUINT_TO_POINTER((uint32_t)pind),
                             program);
    } else {
        /* a loaded program is immutable: Execute may be running it */
        program = csd_get_program_locked(csd, pind);
        if (!program || !program->loading || program->size != psize ||
            (load->pit == 1 && program->pid != pid) ||
            program->type != load->ptype) {
            qemu_mutex_unlock(&csd->lock);
            return NVME_INVALID_FIELD | NVME_DNR;
        }
    }

    if (numb) {
        status = dma_write_prp(n, program->data + loff, numb, prp1, prp2);
        if (status) {
            qemu_mutex_unlock(&csd->lock);
            return status | NVME_DNR;
        }
        program->load_size += numb;
    }

    if (program->load_size == program->size) {
        /*
         * Opening a library runs its constructors, which may take any time.
         * Keep the lock free for the pollers meanwhile; the program is still
         * loading, so nothing runs it, and admin commands, the only ones that
         * change programs, are serialized by the BQL.
         */
        program->refs++;
        qemu_mutex_unlock(&csd->lock);
        status = csd_load_program_data(n, program, load->jit);
        qemu_mutex_lock(&csd->lock);
        if (!status) {
            program->loading = false;
            program->active = false;
        }
        csd_program_unref(program);
    }

    qemu_mutex_unlock(&csd->lock);
    return status;
}

static uint16_t csd_compute_activate(FemuCtrl *n, NvmeCmd *cmd)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdProgramActivationCmd *activation =
        (NvmeCsdProgramActivationCmd *)cmd;
    uint16_t pind = activation->pind;
    uint8_t sel = activation->sel;
    FemuCsdProgram *program;

    if (pind == 0) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    qemu_mutex_lock(&csd->lock);
    program = csd_get_program_locked(csd, pind);
    if (!program || program->loading) {
        qemu_mutex_unlock(&csd->lock);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    switch (sel) {
    case 0:
        program->active = false;
        break;
    case 1:
        program->active = true;
        break;
    default:
        qemu_mutex_unlock(&csd->lock);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    qemu_mutex_unlock(&csd->lock);
    return NVME_SUCCESS;
}

static uint16_t csd_mrs_mgmt(FemuCtrl *n, NvmeCmd *cmd, NvmeCqe *cqe)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdMrsMgmtCmd *manage = (NvmeCsdMrsMgmtCmd *)cmd;
    uint16_t rsid = le16_to_cpu(manage->rsid);
    uint32_t sel = manage->sel;
    uint32_t numr = manage->numr;
    uint64_t prp1 = le64_to_cpu(manage->prp1);
    uint64_t prp2 = le64_to_cpu(manage->prp2);
    NvmeCsdMemoryRange *ranges = NULL;
    FemuCsdMrs *mrs;
    uint32_t id;
    uint16_t status = NVME_SUCCESS;

    switch (sel) {
    case 0:
        if (rsid != 0 || numr == 0 ||
            numr > CSD_EXEC_DATA_MAX / sizeof(NvmeCsdMemoryRange)) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        ranges = g_new0(NvmeCsdMemoryRange, numr);
        status = dma_write_prp(n, (uint8_t *)ranges,
                               numr * sizeof(*ranges), prp1, prp2);
        if (status) {
            g_free(ranges);
            return status;
        }

        qemu_mutex_lock(&csd->lock);
        id = csd->next_rsid++;
        if (id == 0) {
            csd->next_rsid = 1;
            id = csd->next_rsid++;
        }
        while (g_hash_table_contains(csd->mrs, GUINT_TO_POINTER(id))) {
            id = csd->next_rsid++;
            if (id == 0) {
                csd->next_rsid = 1;
                id = csd->next_rsid++;
            }
        }

        mrs = g_new0(FemuCsdMrs, 1);
        mrs->rsid = id;
        mrs->numr = numr;
        mrs->ranges = ranges;
        g_hash_table_insert(csd->mrs, GUINT_TO_POINTER(id), mrs);
        qemu_mutex_unlock(&csd->lock);

        cqe->n.result = id;
        return NVME_SUCCESS;

    case 1:
        if (rsid == 0) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        qemu_mutex_lock(&csd->lock);
        if (!csd_get_mrs_locked(csd, rsid)) {
            qemu_mutex_unlock(&csd->lock);
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        g_hash_table_remove(csd->mrs, GUINT_TO_POINTER((uint32_t)rsid));
        qemu_mutex_unlock(&csd->lock);
        cqe->n.result = 0;
        return NVME_SUCCESS;

    default:
        return NVME_INVALID_FIELD | NVME_DNR;
    }
}

/* map each range to its AFDM, holding a reference on each for the job */
static uint16_t csd_build_exec_args_locked(FemuCsdState *csd,
                                           NvmeCsdMemoryRange *ranges,
                                           uint32_t numr, FemuCsdJob *job)
{
    void **mr_addr = g_new0(void *, numr);
    long long *mr_len = g_new0(long long, numr);
    FemuCsdAfdm **afdms = g_new0(FemuCsdAfdm *, numr);

    for (uint32_t i = 0; i < numr; i++) {
        uint32_t nsid = le32_to_cpu(ranges[i].nsid);
        uint32_t len = le32_to_cpu(ranges[i].len);
        uint64_t sb = le64_to_cpu(ranges[i].sb);
        FemuCsdAfdm *afdm;

        if (nsid != NVME_CSD_MR_AFDM_NSID) {
            goto invalid;
        }

        afdm = csd_get_afdm_locked(csd, sb);
        if (!afdm) {
            goto invalid;
        }
        if (len == 0) {
            len = afdm->size > UINT32_MAX ? UINT32_MAX : afdm->size;
        }
        if (len > afdm->size) {
            goto invalid;
        }

        mr_addr[i] = afdm->data;
        mr_len[i] = len;
        afdms[i] = afdm;
    }

    for (uint32_t i = 0; i < numr; i++) {
        afdms[i]->refs++;
    }
    job->args.numr = numr;
    job->args.mr_addr = mr_addr;
    job->args.mr_len = mr_len;
    job->afdms = afdms;

    return NVME_SUCCESS;

invalid:
    g_free(mr_addr);
    g_free(mr_len);
    g_free(afdms);
    return NVME_INVALID_FIELD | NVME_DNR;
}

static void csd_job_free_locked(FemuCsdJob *job)
{
    for (int i = 0; job->afdms && i < job->args.numr; i++) {
        csd_afdm_unref(job->afdms[i]);
    }
    csd_program_unref(job->program);
    g_free(job->afdms);
    g_free(job->args.mr_addr);
    g_free(job->args.mr_len);
    g_free(job->data);
    g_free(job);
}

static uint16_t csd_run_program(FemuCsdProgram *program, FemuCsdArgs *args,
                                int64_t *result)
{
    uint64_t copy_size;

    switch (program->type) {
    case NVME_CSD_CSF_TYPE_PHANTOM:
        if (args->numr >= 2) {
            copy_size = MIN(args->mr_len[0], args->mr_len[1]);
            memcpy(args->mr_addr[0], args->mr_addr[1], copy_size);
            *result = copy_size > INT64_MAX ? INT64_MAX : copy_size;
        }
        return NVME_SUCCESS;
    case NVME_CSD_CSF_TYPE_SHARED_LIB:
        if (!program->shared_lib_fn) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        *result = program->shared_lib_fn(args);
        return NVME_SUCCESS;
    case NVME_CSD_CSF_TYPE_EBPF:
#ifdef CONFIG_FEMU_CSD_UBPF
        if (!program->ubpf_vm) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        if (program->ubpf_jit_fn) {
            *result = program->ubpf_jit_fn((struct ubpf_jit_args *)args);
        } else {
            uint64_t ubpf_result;

            if (ubpf_exec(program->ubpf_vm, (struct ubpf_jit_args *)args,
                          &ubpf_result) < 0) {
                return NVME_INVALID_FIELD | NVME_DNR;
            }
            *result = ubpf_result;
        }
        return NVME_SUCCESS;
#else
        return NVME_INVALID_FIELD | NVME_DNR;
#endif
    default:
        return NVME_INVALID_FIELD | NVME_DNR;
    }
}

/*
 * Hold the compute unit that frees up first for runtime from start, and
 * return when the hold ends; nr_cu bounds how many programs run at once.
 */
static int64_t csd_hold_cu_locked(FemuCsdState *csd, uint64_t start,
                                  uint64_t runtime)
{
    uint32_t cu = 0;
    uint32_t i;

    for (i = 1; i < csd->params.nr_cu; i++) {
        if (csd->cu_next_avail[i] < csd->cu_next_avail[cu]) {
            cu = i;
        }
    }
    if (csd->cu_next_avail[cu] > start) {
        start = csd->cu_next_avail[cu];
    }
    csd->cu_next_avail[cu] = start + runtime;
    return csd->cu_next_avail[cu];
}

/* Set the result and completion time of a run. */
static void csd_finish_locked(FemuCsdState *csd, FemuCsdJob *job,
                              uint16_t status, int64_t result, int64_t host_ns)
{
    NvmeRequest *req = job->req;
    uint64_t runtime = job->runtime;

    req->status = status;
    if (!status) {
        req->cqe.res64 = cpu_to_le64((uint64_t)result);
    }

    if (job->reserved_end) {
        /* a known runtime took its unit in arrival order at submission */
        req->expire_time = job->reserved_end;
    } else if (!status) {
        /*
         * A program that names no runtime is charged the time it took the
         * host to run it, scaled: by the program's own factor (tenths) when
         * it gave one, else by the device's csf_runtime_scale. Its unit is
         * known only now.
         */
        uint64_t scale10 = job->program->runtime_scale ?
                           job->program->runtime_scale :
                           (uint64_t)csd->params.csf_runtime_scale * 10;

        runtime = host_ns > 0 ? host_ns * scale10 / 10 : 0;
        if (runtime) {
            req->expire_time = csd_hold_cu_locked(csd, job->arrival, runtime);
        }
    }
    req->reqlat = req->expire_time - job->arrival;

    /*
     * The completion carries the program's result, so it cannot go out
     * before the host has run the program: a runtime shorter than the host's
     * own is not reached.
     */
    if (!status && !csd->overrun_warned && host_ns > 0 &&
        runtime < (uint64_t)host_ns) {
        csd->overrun_warned = true;
        warn_report("femu-csd: a program took longer on the host than its "
                    "modelled run time; its completions follow host time");
    }
}

/*
 * One thread per compute unit runs the programs, so a long one holds neither
 * a poller nor any lock the pollers or the admin queue take. Program IDs do
 * not serialize calls: the host orders dependencies and the CSF owns the
 * synchronization of any state shared by concurrent calls.
 */
static void *csd_worker(void *opaque)
{
    femu_bind_device_thread();
    FemuCsdState *csd = opaque;
    FemuCtrl *n = csd->n;
    FemuCsdProgram *dead;
    FemuCsdJob *job;
    struct rte_ring *ring;
    int64_t result, host_ns;
    uint64_t run_begin=0,run_end=0;
    struct timespec cpu_begin,cpu_end;
    uint16_t status;

    qemu_mutex_lock(&csd->lock);
    while (!csd->stopping) {
        dead = QSLIST_FIRST(&csd->dead);
        if (dead) {
            QSLIST_REMOVE_HEAD(&csd->dead, dead);
            qemu_mutex_unlock(&csd->lock);
            csd_program_destroy(dead);
            qemu_mutex_lock(&csd->lock);
            continue;
        }

        job = QTAILQ_FIRST(&csd->pending);
        if (!job) {
            qemu_cond_wait(&csd->job_cond, &csd->lock);
            continue;
        }
        QTAILQ_REMOVE(&csd->pending, job, entry);
        if (!job->req && !job->graph) {
            csd_job_free_locked(job);
            continue;
        }
        if (job->graph && (!job->graph->req || job->graph->status)) {
            job->graph->nodes[job->graph_node].state = GRAPH_COMPUTED;
            job->graph->nodes[job->graph_node].deadline = 0;
            csd_graph_event_locked(csd, &job->graph->nodes[job->graph_node]);
            continue;
        }

        if (job->graph && (job->graph->flags & FEMU_CSD_GRAPH_PROFILE)) {
            job->graph->session->cu_queue_ns +=
                qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - job->arrival;
        }
        QTAILQ_INSERT_TAIL(&csd->running, job, entry);
        qemu_mutex_unlock(&csd->lock);

        bool profiling=job->graph && (job->graph->flags & FEMU_CSD_GRAPH_PROFILE);
        if(profiling) clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_begin);
        result = 0;
        host_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        run_begin=host_ns;
        status = csd_run_program(job->program, &job->args, &result);
        run_end=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        host_ns = run_end - host_ns;
        if(profiling) clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_end);

        qemu_mutex_lock(&csd->lock);
        if (job->graph) {
            uint32_t session=job->graph->session->id;
            uint64_t cookie=job->graph->cookie;
            int64_t arrival=job->arrival;
            unsigned node=job->graph_node;
            uint64_t returned=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            csd_graph_computed_locked(csd, job, status, result, host_ns);
            QTAILQ_REMOVE(&csd->running, job, entry);
            qemu_mutex_unlock(&csd->lock);
            if(profiling) fprintf(stderr,"CSD_CU_SPAN session=%u cookie=%" PRIu64 " node=%u queued=%" PRId64
                " begin=%" PRIu64 " end=%" PRIu64 " returned=%" PRIu64 " cpu_ns=%" PRIu64 "\n",
                session,cookie,node,arrival,run_begin,run_end,returned,
                (uint64_t)((cpu_end.tv_sec-cpu_begin.tv_sec)*1000000000LL+cpu_end.tv_nsec-cpu_begin.tv_nsec));
            qemu_mutex_lock(&csd->lock);
            continue;
        } else if (job->req) {
            csd_finish_locked(csd, job, status, result, host_ns);
        }

        /*
         * Hand the request to the poller that fetched it. Its ring is shared
         * with the FTL thread and may be full for a moment; keep the job on
         * the running list meanwhile, so a queue deletion can still drop it.
         */
        ring = n->use_ftl_thread ? n->to_poller[job->poller] :
                                   n->to_ftl[job->poller];
        while (job->req && !csd->stopping &&
               femu_ring_enqueue(ring, (void *)&job->req, 1) != 1) {
            qemu_mutex_unlock(&csd->lock);
            g_usleep(100);
            qemu_mutex_lock(&csd->lock);
        }
        QTAILQ_REMOVE(&csd->running, job, entry);
        csd_job_free_locked(job);
    }
    qemu_mutex_unlock(&csd->lock);

    return NULL;
}

/* called by the poller once it has finished with the request */
static void csd_exec_defer(FemuCtrl *n, NvmeRequest *req, int poller)
{
    FemuCsdState *csd = csd_state(n);
    FemuCsdJob *job = req->opaque;

    req->opaque = NULL;
    job->poller = poller;
    qemu_mutex_lock(&csd->lock);
    QTAILQ_INSERT_TAIL(&csd->pending, job, entry);
    qemu_cond_signal(&csd->job_cond);
    qemu_mutex_unlock(&csd->lock);
}

/*
 * A submission queue is about to be freed with the dataplane paused. Forget
 * its requests held by Execute jobs, so a program that returns later does not
 * complete into freed memory; the caller then drains the rings, which covers
 * any completion posted before this.
 */
void femu_csd_drain_sq(FemuCtrl *n, NvmeSQueue *sq)
{
    FemuCsdState *csd = csd_state(n);
    FemuCsdJob *job, *next;

    if (!csd) {
        return;
    }

    qemu_mutex_lock(&csd->lock);
    FemuCsdGraph *g;
    QTAILQ_FOREACH(g, &csd->graphs, entry) {
        if (g->req && g->req->sq == sq) {
            g->req = NULL;
            g->status = NVME_CMD_ABORT_REQ;
            g->result = 0;
            csd_graph_ready_locked(csd, g);
        }
    }
    QTAILQ_FOREACH_SAFE(job, &csd->pending, entry, next) {
        if (job->req && job->req->sq == sq) {
            QTAILQ_REMOVE(&csd->pending, job, entry);
            csd_job_free_locked(job);
        }
    }
    QTAILQ_FOREACH(job, &csd->running, entry) {
        if (job->req && job->req->sq == sq) {
            job->req = NULL;
        }
    }
    qemu_mutex_unlock(&csd->lock);
}

/*
 * Check the command and take what the program needs here on the poller, then
 * leave the run itself to a compute unit thread.
 */
static uint16_t csd_exec(FemuCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdExecCmd *exec = (NvmeCsdExecCmd *)cmd;
    uint16_t pind = le16_to_cpu(exec->pind);
    uint16_t rsid = le16_to_cpu(exec->rsid);
    uint32_t numr = le32_to_cpu(exec->numr);
    uint32_t dlen = le32_to_cpu(exec->dlen);
    uint32_t group_id = exec->group;
    uint64_t runtime = le32_to_cpu(exec->runtime);
    FemuCsdProgram *program;
    FemuCsdMrs *mrs;
    uint8_t *data = NULL;
    NvmeCsdMemoryRange *ranges = NULL;
    FemuCsdJob *job;
    uint16_t status;

    if (dlen == 0 && numr > 0) {
        dlen = numr * sizeof(NvmeCsdMemoryRange);
    }

    if (pind == 0 || (!rsid && !numr && dlen) ||
        numr > CSD_EXEC_DATA_MAX / sizeof(NvmeCsdMemoryRange)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (rsid != 0 && numr != 0) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (dlen < numr * sizeof(NvmeCsdMemoryRange) || dlen > CSD_EXEC_DATA_MAX) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (numr) {
        data = g_malloc0(dlen);
        status = dma_write_cmd(n, cmd, data, dlen);
        if (status) {
            g_free(data);
            return status;
        }
        ranges = (NvmeCsdMemoryRange *)data;
    }

    job = g_new0(FemuCsdJob, 1);
    qemu_mutex_lock(&csd->lock);
    program = csd_get_program_locked(csd, pind);
    if (!program || !program->active ||
        (group_id != 0 && !csd_get_group_locked(csd, group_id))) {
        status = NVME_INVALID_FIELD | NVME_DNR;
        goto fail;
    }

    if (runtime == 0) {
        runtime = program->runtime;
    }

    if (rsid) {
        mrs = csd_get_mrs_locked(csd, rsid);
        if (!mrs) {
            status = NVME_INVALID_FIELD | NVME_DNR;
            goto fail;
        }
        ranges = mrs->ranges;
        numr = mrs->numr;
        dlen = 0;
    }

    status = csd_build_exec_args_locked(csd, ranges, numr, job);
    if (status) {
        goto fail;
    }
    program->refs++;
    /*
     * A known runtime takes its compute unit now, in arrival order, so the
     * order the host happens to finish programs in does not change it.
     */
    if (runtime) {
        job->reserved_end = csd_hold_cu_locked(csd, req->expire_time, runtime);
    }
    qemu_mutex_unlock(&csd->lock);

    job->program = program;
    job->args.cparam1 = le64_to_cpu(exec->cparam1);
    job->args.cparam2 = le64_to_cpu(exec->cparam2);
    job->args.data_buffer = data && dlen > numr * sizeof(NvmeCsdMemoryRange) ?
                            data + numr * sizeof(NvmeCsdMemoryRange) : NULL;
    job->args.buffer_len = job->args.data_buffer ?
                           dlen - numr * sizeof(NvmeCsdMemoryRange) : 0;
    job->data = data;
    job->runtime = runtime;
    job->arrival = req->expire_time;
    job->req = req;
    req->opaque = job;
    req->defer = csd_exec_defer;

    return NVME_SUCCESS;

fail:
    qemu_mutex_unlock(&csd->lock);
    g_free(job);
    g_free(data);
    return status;
}

static uint16_t csd_normalize_prio(int8_t *prio)
{
    if (*prio == 0) {
        *prio = 5;
    }

    if (*prio < 1 || *prio > 9) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static uint16_t csd_create_group(FemuCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdCreateGroupCmd *create = (NvmeCsdCreateGroupCmd *)cmd;
    FemuCsdGroup *group;
    uint32_t id;
    int8_t prio = create->prio;
    uint16_t status;

    status = csd_normalize_prio(&prio);
    if (status) {
        return status;
    }

    group = g_new0(FemuCsdGroup, 1);
    group->prio = prio;
    group->qos_flags = create->qos_flags;
    group->bandwidth = le32_to_cpu(create->bandwidth);
    group->deadline = le32_to_cpu(create->deadline);

    qemu_mutex_lock(&csd->lock);
    id = csd->next_group_id++;
    if (id == 0) {
        csd->next_group_id = 1;
        id = csd->next_group_id++;
    }
    group->id = id;
    g_hash_table_insert(csd->groups, GUINT_TO_POINTER(id), group);
    qemu_mutex_unlock(&csd->lock);

    req->cqe.n.result = id;
    return NVME_SUCCESS;
}

static uint16_t csd_set_qos(FemuCtrl *n, NvmeCmd *cmd)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdSetQosCmd *set = (NvmeCsdSetQosCmd *)cmd;
    uint32_t id = le32_to_cpu(set->id);
    int8_t prio = set->prio;
    FemuCsdGroup *group;
    uint16_t status;

    status = csd_normalize_prio(&prio);
    if (status) {
        return status;
    }

    qemu_mutex_lock(&csd->lock);
    group = csd_get_group_locked(csd, id);
    if (!group) {
        qemu_mutex_unlock(&csd->lock);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    group->prio = prio;
    group->qos_flags = set->qos_flags;
    group->bandwidth = le32_to_cpu(set->bandwidth);
    group->deadline = le32_to_cpu(set->deadline);
    qemu_mutex_unlock(&csd->lock);

    return NVME_SUCCESS;
}

static uint16_t csd_delete_group(FemuCtrl *n, NvmeCmd *cmd)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdDeleteGroupCmd *delete = (NvmeCsdDeleteGroupCmd *)cmd;
    uint32_t id = le32_to_cpu(delete->id);

    qemu_mutex_lock(&csd->lock);
    if (!csd_get_group_locked(csd, id)) {
        qemu_mutex_unlock(&csd->lock);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    g_hash_table_remove(csd->groups, GUINT_TO_POINTER(id));
    qemu_mutex_unlock(&csd->lock);

    return NVME_SUCCESS;
}

static uint16_t csd_alloc_fdm(FemuCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdAllocFdmCmd *alloc = (NvmeCsdAllocFdmCmd *)cmd;
    FemuCsdAfdm *afdm;
    uint64_t size = le64_to_cpu(alloc->size);
    uint32_t id;

    if (alloc->type != NVME_CSD_FDM_TYPE_HOST || size == 0) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    qemu_mutex_lock(&csd->lock);
    if (size > csd->fdm_capacity - csd->fdm_used) {
        qemu_mutex_unlock(&csd->lock);
        return NVME_CAP_EXCEEDED | NVME_DNR;
    }

    id = csd->next_afdm_id++;
    if (id == 0) {
        csd->next_afdm_id = 1;
        id = csd->next_afdm_id++;
    }

    afdm = g_new0(FemuCsdAfdm, 1);
    afdm->id = id;
    afdm->refs = 1;
    afdm->csd = csd;
    afdm->size = size;
    afdm->data = g_malloc0(size);

    g_hash_table_insert(csd->afdms, GUINT_TO_POINTER(id), afdm);
    csd->fdm_used += size;
    qemu_mutex_unlock(&csd->lock);

    req->cqe.n.result = id;
    return NVME_SUCCESS;
}

static uint16_t csd_dealloc_afdm(FemuCtrl *n, NvmeCmd *cmd)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdDeallocAfdmCmd *dealloc = (NvmeCsdDeallocAfdmCmd *)cmd;
    uint32_t id = le32_to_cpu(dealloc->id);
    FemuCsdAfdm *afdm;

    qemu_mutex_lock(&csd->lock);
    afdm = csd_get_afdm_locked(csd, id);
    if (!afdm) {
        qemu_mutex_unlock(&csd->lock);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    g_hash_table_remove(csd->afdms, GUINT_TO_POINTER(id));
    qemu_mutex_unlock(&csd->lock);

    return NVME_SUCCESS;
}

static uint16_t csd_read_afdm(FemuCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdReadAfdmCmd *read = (NvmeCsdReadAfdmCmd *)cmd;
    uint32_t id = le32_to_cpu(read->id);
    uint64_t offset = le64_to_cpu(read->offset);
    uint64_t size = le64_to_cpu(read->size);
    FemuCsdAfdm *afdm;
    uint16_t status;

    qemu_mutex_lock(&csd->lock);
    afdm = csd_get_afdm_locked(csd, id);
    status = csd_check_afdm_range(afdm, offset, size);
    if (!status) {
        afdm->refs++;
        qemu_mutex_unlock(&csd->lock);
        status = dma_read_cmd(n, cmd, afdm->data + offset, size);
        qemu_mutex_lock(&csd->lock);
        csd_afdm_unref(afdm);
    }
    qemu_mutex_unlock(&csd->lock);

    if (status) {
        return status | NVME_DNR;
    }

    req->xfer_bytes = size; req->is_write = 0;
    req->cqe.n.result = size;
    return NVME_SUCCESS;
}

static uint16_t csd_write_afdm(FemuCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdWriteAfdmCmd *write = (NvmeCsdWriteAfdmCmd *)cmd;
    uint32_t id = le32_to_cpu(write->id);
    uint64_t offset = le64_to_cpu(write->offset);
    uint64_t size = le64_to_cpu(write->size);
    FemuCsdAfdm *afdm;
    uint16_t status;

    qemu_mutex_lock(&csd->lock);
    afdm = csd_get_afdm_locked(csd, id);
    status = csd_check_afdm_range(afdm, offset, size);
    if (!status) {
        afdm->refs++;
        qemu_mutex_unlock(&csd->lock);
        status = dma_write_cmd(n, cmd, afdm->data + offset, size);
        qemu_mutex_lock(&csd->lock);
        csd_afdm_unref(afdm);
    }
    qemu_mutex_unlock(&csd->lock);

    if (status) {
        return status | NVME_DNR;
    }

    req->xfer_bytes = size; req->is_write = 1;
    req->cqe.n.result = size;
    return NVME_SUCCESS;
}

/* Optional local counters distinguish FTL simulation from the RAM-backed
 * data copy. They do not change commands, I/O ordering or public interfaces. */
typedef struct FemuCsdIoTiming {
    uint64_t ftl_ns,backend_lock_ns,copy_ns,csd_lock_ns;
} FemuCsdIoTiming;
static uint16_t csd_nvm_copy_locked(FemuCtrl *n, NvmeNamespace *ns, NvmeRequest *req,
                             uint64_t *lat, FemuCsdAfdm *held,FemuCsdIoTiming *timing)
{
    FemuCsdState *csd = csd_state(n);
    NvmeCsdNvmToAfdmCmd *copy = (NvmeCsdNvmToAfdmCmd *)&req->cmd;
    bool write = copy->opcode == NVME_CMD_CSD_AFDM_TO_NVM;
    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].lbads;
    uint32_t id = le32_to_cpu(copy->id);
    uint64_t offset = le64_to_cpu(copy->offset);
    uint64_t slba = le64_to_cpu(copy->slba);
    uint64_t nlb = le16_to_cpu(copy->nlb) + 1;
    uint64_t size = nlb << data_shift;
    /*
     * Address the backend within this namespace's slice, as the NVM read/write
     * path does. Without the shift a CSD namespace packed after another one
     * would copy from the earlier namespace's data instead of its own.
     */
    uint64_t nvm_offset = ns->backend_offset + (slba << data_shift);
    NvmeRequest internal = *req;
    NvmeRwCmd *rw = (NvmeRwCmd *)&internal.cmd;
    FemuCsdAfdm *afdm;
    uint16_t status;

    /* The command describes data only, not metadata or power-loss staging. */
    if (n->power_loss || nvme_ns_ms(ns)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (!ns->ssd) {
        return NVME_INTERNAL_DEV_ERROR | NVME_DNR;
    }

    /*
     * Bound the copy by this namespace's slice rather than the whole backend:
     * the LBA test already limits it to the namespace, and checking the slice
     * keeps a range that passes here from reaching another namespace's data.
     */
    if (slba > le64_to_cpu(ns->id_ns.nsze) ||
        nlb > le64_to_cpu(ns->id_ns.nsze) - slba ||
        size > ns->size || nvm_offset > ns->backend_offset + ns->size - size) {
        return NVME_LBA_RANGE | NVME_DNR;
    }

    if (!write) {
        if (find_next_bit(ns->uncorrectable, slba + nlb, slba) < slba + nlb) {
            return NVME_UNRECOVERED_READ | NVME_DNR;
        }
        status = nvme_check_dulbe(n, ns, slba, slba + nlb);
        if (status) {
            return status;
        }
    }

    afdm = held ? held : csd_get_afdm_locked(csd, id);
    status = csd_check_afdm_range(afdm, offset, size);
    if (!status) {
        /* Called by the sole FTL owner with csd->lock held. Retain the AFDM
         * while its table entry may be removed; graph/cancellation state is
         * updated only after reacquiring the lock. NAND state needs no CSD
         * lock because no other thread is allowed to execute the FTL. */
        afdm->refs++;
        qemu_mutex_unlock(&csd->lock);
        uint64_t host_pages = ns->ssd->host_write_pages;

        /* Reuse normal mapping, NAND timing and GC, without a Guest DMA. */
        memset(&internal.cmd, 0, sizeof(internal.cmd));
        internal.cmd.opcode = write ? NVME_CMD_WRITE : NVME_CMD_READ;
        internal.cmd.nsid = cpu_to_le32(ns->id);
        internal.slba = slba;
        internal.nlb = nlb;
        internal.is_write = write;
        internal.status = NVME_SUCCESS;
        rw->slba = cpu_to_le64(slba);
        rw->nlb = cpu_to_le16(nlb - 1);
        /* Spill writes reach NAND before completion, even with VWC enabled. */
        rw->control = cpu_to_le16(write ? NVME_RW_FUA : 0);
        uint64_t stamp=timing?qemu_clock_get_ns(QEMU_CLOCK_REALTIME):0;
        *lat = bb_ftl_process_req(n, ns, &internal);
        if(timing) timing->ftl_ns+=qemu_clock_get_ns(QEMU_CLOCK_REALTIME)-stamp;
        if (write) {
            ns->ssd->host_write_pages = host_pages;
        }
        status = internal.status;
        if (!status) {
            void *nvm = (uint8_t *)n->mbe->logical_space + nvm_offset;

            if(timing) stamp=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            backend_lock_range(n->mbe, nvm_offset, size);
            if(timing) { uint64_t at=qemu_clock_get_ns(QEMU_CLOCK_REALTIME); timing->backend_lock_ns+=at-stamp; stamp=at; }
            if (write) {
                memcpy(nvm, afdm->data + offset, size);
            } else {
                memcpy(afdm->data + offset, nvm, size);
            }
            if(timing) timing->copy_ns+=qemu_clock_get_ns(QEMU_CLOCK_REALTIME)-stamp;
            backend_unlock_range(n->mbe, nvm_offset, size);
            if (write) {
                nvme_mark_written(ns, slba, nlb);
                nvme_note_user_write(n);
            }
        }
        if(timing) stamp=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        qemu_mutex_lock(&csd->lock);
        if(timing) timing->csd_lock_ns+=qemu_clock_get_ns(QEMU_CLOCK_REALTIME)-stamp;
        csd_afdm_unref(afdm);
    }
    if (status) {
        return status;
    }

    req->cqe.n.result = size;
    return NVME_SUCCESS;
}

/* Graph state and hidden spill mappings are protected by csd->lock. All FTL
 * work below is performed by the controller's existing sole FTL thread. */
#define GRAPH_IO_MASK ((1ULL << 60) - 1)
static bool csd_graph_span(FemuCsdGraph *g, uint32_t offset, uint64_t bytes)
{
    uint64_t start = sizeof(struct femu_graph_header) +
                     (uint64_t)g->count * sizeof(struct femu_graph_node);
    return offset >= start && offset <= g->bytes && bytes <= g->bytes - offset;
}

static void csd_graph_destroy_locked(FemuCsdState *csd, FemuCsdGraph *g)
{
    qemu_mutex_lock(&csd->cache_lock);
    for (unsigned i=0;i<ARRAY_SIZE(csd->cache);i++)
        if (csd->cache[i].event.token && csd->cache[i].graph==g) {
            csd->cache[i].event.token=0; qatomic_dec(&csd->cache_work);
        }
    qemu_mutex_unlock(&csd->cache_lock);
    if (g->queued) {
        QTAILQ_REMOVE(&csd->graph_ready, g, ready_entry);
        qatomic_dec(&csd->ready_count);
    }
    for (unsigned i = 0; i < g->count; i++) {
        g_free(g->nodes[i].reads);
        g_free(g->nodes[i].read_completions);
        if (g->nodes[i].pos) { pqueue_remove(csd->graph_events, &g->nodes[i]); }
        if (g->nodes[i].job) {
            csd_job_free_locked(g->nodes[i].job);
        }
        if (g->nodes[i].afdm) {
            csd_afdm_unref(g->nodes[i].afdm);
        }
    }
    csd_graph_cache_deadline(csd);
    QTAILQ_REMOVE(&csd->graphs, g, entry);
    g->session->graphs--;
    qatomic_dec(&csd->graph_count);
    g_free(g->data);
    g_free(g);
}

static uint64_t csd_spill_free_locked(FemuCsdState *csd, FemuCsdSpill *s)
{
    NvmeNamespace *ns = s->session->ns;
    struct ssd *ssd = ns->ssd;
    uint64_t page_bytes = (uint64_t)ssd->sp.secsz * ssd->sp.secs_per_pg;
    NvmeDsmRange *range = g_new0(NvmeDsmRange, 1);
    *range = (NvmeDsmRange){
        .slba = cpu_to_le64(((csd->spill_base + s->page) * page_bytes) >> ns->lbaf.lbads),
        .nlb = cpu_to_le32((s->pages * page_bytes) >> ns->lbaf.lbads)
    };
    NvmeRequest req = { .ns = ns, .stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
        .dsm_ranges = range, .dsm_nr_ranges = 1 }; /* ssd_trim owns the range */
    req.cmd.opcode = NVME_CMD_DSM;
    qemu_mutex_unlock(&csd->lock);
    uint64_t lat = bb_ftl_process_req(csd->n, ns, &req);
    qemu_mutex_lock(&csd->lock);
    for (uint64_t i = s->page; i < s->page + s->pages; i++) {
        csd->spill_bitmap[i / 8] &= ~(1U << (i % 8));
    }
    g_hash_table_remove(csd->spills, &s->id);
    return lat;
}

static uint16_t csd_graph_session(FemuCsdState *csd, NvmeNamespace *ns,
                                   NvmeRequest *req, uint64_t *lat)
{
    uint32_t action = le32_to_cpu(req->cmd.cdw10);
    uint32_t id = le32_to_cpu(req->cmd.cdw11);
    FemuCsdSession *s = g_hash_table_lookup(csd->sessions, GUINT_TO_POINTER(id));
    if (action == FEMU_GRAPH_OPEN) {
        if (g_hash_table_size(csd->sessions) >= 1024 || !csd->next_session) {
            return NVME_CAP_EXCEEDED | NVME_DNR;
        }
        s = g_new0(FemuCsdSession, 1);
        s->id = csd->next_session++; s->ns = ns;
        s->cancelled = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
        g_hash_table_insert(csd->sessions, GUINT_TO_POINTER(s->id), s);
        req->cqe.res64 = cpu_to_le64(s->id);
        return NVME_SUCCESS;
    }
    if (!s || s->ns != ns) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (action == FEMU_GRAPH_ROLLBACK) {
        uint64_t cookie = le32_to_cpu(req->cmd.cdw12) | (uint64_t)le32_to_cpu(req->cmd.cdw13) << 32;
        GHashTableIter it; gpointer value;
        g_hash_table_iter_init(&it, csd->spills);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            FemuCsdSpill *spill = value;
            if (spill->session == s && spill->creator == cookie && spill->busy) {
                return NVME_INVALID_FIELD | NVME_DNR;
            }
        }
        for (;;) {
            FemuCsdSpill *found = NULL;
            g_hash_table_iter_init(&it, csd->spills);
            while (g_hash_table_iter_next(&it, NULL, &value)) {
                FemuCsdSpill *spill = value;
                if (spill->session == s && spill->creator == cookie) { found = spill; break; }
            }
            if (!found) { break; }
            *lat = MAX(*lat, csd_spill_free_locked(csd, found));
        }
        return NVME_SUCCESS;
    }
    if (action == FEMU_GRAPH_CANCEL || action == FEMU_GRAPH_FORGET_CANCEL) {
        uint64_t cookie = le32_to_cpu(req->cmd.cdw12) |
                          (uint64_t)le32_to_cpu(req->cmd.cdw13) << 32;
        if (action == FEMU_GRAPH_FORGET_CANCEL) {
            g_hash_table_remove(s->cancelled, &cookie);
            return NVME_SUCCESS;
        }
        FemuCsdGraph *g;
        QTAILQ_FOREACH(g, &csd->graphs, entry) {
            if (g->session == s && g->cookie == cookie) {
                g->status = NVME_CMD_ABORT_REQ;
                g->result = 0;
                csd_graph_ready_locked(csd, g);
                return NVME_SUCCESS;
            }
        }
        if (g_hash_table_size(s->cancelled) >= 64) { return NVME_CAP_EXCEEDED | NVME_DNR; }
        uint64_t *key = g_new(uint64_t, 1);
        *key = cookie;
        g_hash_table_replace(s->cancelled, key, GUINT_TO_POINTER(1));
        return NVME_SUCCESS;
    }
    if (action != FEMU_GRAPH_CLOSE || s->graphs) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    GHashTableIter it;
    gpointer value;
    /* Removing through the helper invalidates an iterator: restart after a
     * release. Session close is cold-path, bounded by the spill page count. */
    for (;;) {
        FemuCsdSpill *found = NULL;
        g_hash_table_iter_init(&it, csd->spills);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            FemuCsdSpill *spill = value;
            if (spill->session == s) { found = spill; break; }
        }
        if (!found) { break; }
        *lat = MAX(*lat, csd_spill_free_locked(csd, found));
    }
    csd_cache_ring_stop(csd,s);
    if (s->profile) {
        fprintf(stderr, "CSD_GRAPH session=%u graphs=%" PRIu64 " graph_ns=%" PRIu64
            " events=%" PRIu64 " io_calls=%" PRIu64 " io_model_ns=%" PRIu64
            " io_work_ns=%" PRIu64 " cu_calls=%" PRIu64 " cu_wall_ns=%" PRIu64
            " cu_queue_ns=%" PRIu64 " event_late_ns=%" PRIu64 " dispatch_ns=%" PRIu64 "\n",
            s->id, s->completed_graphs, s->graph_ns, s->events, s->io_calls, s->io_ns,
            s->io_work_ns, s->cu_calls, s->cu_ns, s->cu_queue_ns, s->late_ns, s->dispatch_ns);
    }
    g_hash_table_remove(csd->sessions, GUINT_TO_POINTER(id));
    return NVME_SUCCESS;
}

/* Mark eligible input without probing Guest memory in the FTL/CU lock.
 * DMA workers check credits; a full receive ring drops optional work. */
static uint16_t csd_cache_prepare(FemuCsdState *csd,FemuCsdGraphNode *v,
                                  uint64_t offset,uint64_t bytes)
{
    v->cache_candidate=false;
    FemuCacheRing *ring=v->graph->session->cache_ring;
    if((v->wire.operation!=FEMU_GRAPH_LOAD && !(v->job && v->io_action==4)) ||
       !(v->graph->flags&FEMU_CSD_GRAPH_CACHE) || !ring) return NVME_SUCCESS;
    if((offset|bytes)%FEMU_CACHE_PAGE_BYTES || ring->stopping) {
        qatomic_inc(&csd->cache_dropped); return NVME_SUCCESS;
    }
    /* Credit availability is checked by the DMA workers, outside csd->lock. */
    v->cache_candidate=true;
    return NVME_SUCCESS;
}

static void csd_cache_wake_graph(FemuCsdState *csd, FemuCsdGraph *g)
{
    if(!g->active) { csd_graph_ready_locked(csd,g); return; }
    for(unsigned i=0;i<g->count;i++) if(g->nodes[i].state==GRAPH_CACHE_WAIT) {
        csd_graph_ready_locked(csd,g); return;
    }
}

static void csd_cache_publish(FemuCsdState *csd, FemuCsdGraphNode *v, uint64_t now)
{
    if (!v->cache_candidate || !v->graph->session->cache_ring) return;
    v->cache_candidate=false;
    qemu_mutex_lock(&csd->cache_lock);
    FemuCacheRing *ring=v->graph->session->cache_ring;
    uint64_t page=(v->cache_lba << v->graph->session->ns->lbaf.lbads)/FEMU_CACHE_PAGE_BYTES;
    uint64_t pages=v->cache_bytes/FEMU_CACHE_PAGE_BYTES;
    ring->offered_bytes+=v->cache_bytes;
    bool duplicate=!((v->cache_lba << v->graph->session->ns->lbaf.lbads)%FEMU_CACHE_PAGE_BYTES) &&
        page<ring->npages && pages<=ring->npages-page;
    for(uint64_t p=page;duplicate && p<page+pages;++p)
        duplicate=(ring->sent[p/8]&(1U<<(p%8)))!=0;
    /* Successful deliveries only. Do not search the retained-block queue on
     * every read: population must not delay the FTL for optional coalescing. */
    if(duplicate) {
        ring->duplicate_bytes+=v->cache_bytes;
        qemu_mutex_unlock(&csd->cache_lock); return;
    }
    unsigned word=csd->cache_free[0]?0:1;
    uint64_t free_bits=csd->cache_free[word];
    if (!free_bits) {
        qatomic_inc(&csd->cache_dropped); qemu_mutex_unlock(&csd->cache_lock); return;
    }
    FemuCacheItem *item=&csd->cache[word*64+__builtin_ctzll(free_bits)];
    csd->cache_free[word]=free_bits&(free_bits-1);
    *item = (FemuCacheItem){ .graph=v->graph,
        .node=v-v->graph->nodes, .internal_read=v->job!=NULL,
        .session=v->graph->session->id,
        .ring=v->graph->session->cache_ring,.slot=-1,
        .data=v->cache_afdm->data+v->cache_offset,
        .event={.token=csd->cache_next++, .cookie=v->graph->cookie,
        .offset=v->cache_offset,
        .lba=v->cache_lba,
        .bytes=v->cache_bytes, .load_ns=now, .afdm=v->cache_afdm->id,
        .nsid=v->graph->session->ns->id} };
    uintptr_t first=(uintptr_t)item->data;
    for (unsigned i=0;i<v->graph->count;i++) {
        FemuCsdJob *job=v->graph->nodes[i].job;
        if (!job || v->graph->nodes[i].state==GRAPH_DONE) continue;
        for (int j=0;j<job->args.numr;j++) {
            uintptr_t at=(uintptr_t)job->args.mr_addr[j];
            if (first<at+job->args.mr_len[j] && at<first+item->event.bytes) {
                item->consumers|=1ULL<<i; break;
            }
        }
    }
    if (!item->consumers) item->cu_ns=now;
    v->graph->cache_pending++;
    qatomic_inc(&csd->cache_work);
    qemu_cond_broadcast(&csd->cache_cond);
    qemu_mutex_unlock(&csd->cache_lock);
}

static void csd_cache_log(const FemuCacheItem *item, uint64_t now)
{
    fprintf(stderr, "CACHE_BLOCK token=%" PRIu64 " session=%u cookie=%" PRIu64
        " offset=%" PRIu64 " bytes=%" PRIu64 " load=%" PRIu64 " cu=%" PRIu64
        " dma_start=%" PRIu64 " copy_end=%" PRIu64 " link_end=%" PRIu64 " dma=%" PRIu64 " release=%" PRIu64 " dma_bytes=%" PRIu64
        " dma_lock_ns=%" PRIu64 " update_lock_ns=%" PRIu64 " publish_start=%" PRIu64
        " publish_end=%" PRIu64 " segments=%" PRIu64 " copy_cpu_ns=%" PRIu64 " node=%u internal=%u\n",
        item->event.token, item->session, item->event.cookie, item->event.offset,
        item->event.bytes, item->event.load_ns, item->cu_ns, item->dma_start_ns, item->copy_end_ns, item->link_end_ns, item->dma_ns,
        now, item->dma_bytes,item->dma_lock_ns,item->update_lock_ns,
        item->publish_start_ns,item->publish_end_ns,item->segments,item->copy_cpu_ns,
        item->node,item->internal_read);
}

static void csd_cache_finish(FemuCsdState *csd, FemuCacheItem *item)
{
    FemuCsdGraph *g = item->graph;
    item->event.token=0;
    unsigned slot=item-csd->cache;
    csd->cache_free[slot/64]|=UINT64_C(1)<<(slot%64);
    qatomic_dec(&csd->cache_work);
    if (g) { g->cache_pending--; csd_cache_wake_graph(csd,g); }
}

/* STOP is synchronous with actual memory writes, not simulated link time. */
static void csd_cache_ring_stop(FemuCsdState *csd,FemuCsdSession *session)
{
    FemuCacheRing *ring=session->cache_ring;
    if(!ring) return;
    qemu_mutex_lock(&csd->cache_lock);
    ring->stopping=true;
    while(ring->active) qemu_cond_wait(&csd->cache_cond,&csd->cache_lock);
    for(unsigned i=0;i<ARRAY_SIZE(csd->cache);i++) {
        FemuCacheItem *item=&csd->cache[i];
        if(item->event.token && item->ring==ring) {
            item->ring=NULL; item->dropped=true;
        }
    }
    qatomic_set(&csd->cache_dirty,1);
    if(session->profile) fprintf(stderr,"CACHE_RING session=%u offered_bytes=%" PRIu64
        " duplicate_bytes=%" PRIu64 " dma_bytes=%" PRIu64 "\n",
        session->id,ring->offered_bytes,ring->duplicate_bytes,ring->dma_bytes);
    session->cache_ring=NULL; qemu_mutex_destroy(&ring->slots_lock); g_free(ring->sent); g_free(ring);
    qemu_mutex_unlock(&csd->cache_lock);
}
static uint16_t csd_cache_control(FemuCtrl *n,NvmeCmd *cmd)
{
    FemuCsdState *csd=csd_state(n);
    unsigned action=le32_to_cpu(cmd->cdw10),id=le32_to_cpu(cmd->cdw11);
    FemuCacheRing *ring=NULL;
    if(action==FEMU_CACHE_RX_REGISTER) {
        ring=g_new0(FemuCacheRing,1);
        uint16_t status=dma_write_cmd(n,cmd,(void *)&ring->rx,sizeof(ring->rx));
        if(status) { g_free(ring); return status; }
        ring->rx.descriptor_dma=le64_to_cpu(ring->rx.descriptor_dma);
        if(!ring->rx.descriptor_dma || ring->rx.descriptor_dma%FEMU_CACHE_PAGE_BYTES) goto invalid;
        for(unsigned i=0;i<FEMU_CACHE_RX_SLOTS;i++)
            for(unsigned j=0;j<FEMU_CACHE_RX_PAGES;j++) {
                uint64_t *address=&ring->rx.pages[i][j];
                *address=le64_to_cpu(*address);
                if(!*address || *address%FEMU_CACHE_PAGE_BYTES ||
                   *address>UINT64_MAX-FEMU_CACHE_PAGE_BYTES) goto invalid;
            }
    } else if(action!=FEMU_CACHE_RX_STOP) return NVME_INVALID_FIELD|NVME_DNR;
    qemu_mutex_lock(&csd->lock);
    FemuCsdSession *session=g_hash_table_lookup(csd->sessions,GUINT_TO_POINTER(id));
    if(action==FEMU_CACHE_RX_STOP) {
        if(session) csd_cache_ring_stop(csd,session);
    } else if(!session || session->cache_ring) {
        qemu_mutex_unlock(&csd->lock); goto invalid;
    } else {
        ring->npages=(le64_to_cpu(session->ns->id_ns.nsze) << session->ns->lbaf.lbads)/FEMU_CACHE_PAGE_BYTES;
        ring->sent=g_malloc0(DIV_ROUND_UP(ring->npages,8));
        qemu_mutex_init(&ring->slots_lock);
        session->cache_ring=ring;
    }
    qemu_mutex_unlock(&csd->lock);
    return NVME_SUCCESS;
invalid:
    g_free(ring); return NVME_INVALID_FIELD|NVME_DNR;
}

/* Two device DMA workers share the bounded retained-block array. Neither
 * ordinary NVMe pollers nor the FTL owner perform these software copies. */
static bool csd_cache_dma(FemuCsdState *csd)
{
    FemuCtrl *n=csd->n;
    uint64_t entered=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    qemu_mutex_lock(&csd->cache_lock);
    uint64_t locked=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if(csd->stopping) { qemu_mutex_unlock(&csd->cache_lock); return false; }
    if(!n->dataplane_started) {
        qemu_cond_timedwait(&csd->cache_cond,&csd->cache_lock,10);
        qemu_mutex_unlock(&csd->cache_lock); return true;
    }
    FemuCacheItem *item=NULL;
    uint64_t next=UINT64_MAX;
    uint64_t now=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    bool publish=false;
    for(unsigned i=0;i<ARRAY_SIZE(csd->cache);i++) {
        FemuCacheItem *v=&csd->cache[i];
        if(!v->event.token || v->dma_active || !v->ring || v->ring->stopping || v->published || v->dropped) continue;
        if(v->dma_start_ns) {
            if(now<v->dma_ns) { next=MIN(next,v->dma_ns); continue; }
            item=v; publish=true; break;
        }
        item=v; break;
    }
    if(!item) {
        if(next==UINT64_MAX) {
            /* New LOAD, another worker's copy completion, STOP or teardown
             * wakes us. Hold the same mutex while checking and sleeping. */
            qemu_cond_wait(&csd->cache_cond,&csd->cache_lock);
            qemu_mutex_unlock(&csd->cache_lock);
        } else {
            qemu_mutex_unlock(&csd->cache_lock);
            /* QemuCond has millisecond resolution. Only a pending link
             * deadline needs this short wait; an idle worker does not poll. */
            g_usleep(MAX(1,MIN(next-now,50000)/1000));
        }
        return true;
    }
    FemuCacheRing *ring=item->ring;
    int slot=item->slot;
    item->dma_active++; ring->active++;
    item->dma_lock_ns+=locked-entered;
    if(!publish) item->dma_start_ns=now;
    else {
        item->publish_start_ns=now;
        item->data_done=true;
        qatomic_set(&csd->cache_dirty,1);
    }
    bool profiling=item->graph && (item->graph->flags & FEMU_CSD_GRAPH_PROFILE);
    qemu_mutex_unlock(&csd->cache_lock);
    struct timespec cpu_begin,cpu_end;
    if(profiling && !publish) clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_begin);
    bool failed=false;
    if(!publish) {
        qemu_mutex_lock(&ring->slots_lock);
        for(unsigned j=0;j<FEMU_CACHE_RX_SLOTS;j++) {
            unsigned candidate=(ring->next+j)%FEMU_CACHE_RX_SLOTS;
            uint64_t ready=1;
            if(ring->busy[candidate] || femu_dma_read(n,ring->rx.descriptor_dma+
               candidate*sizeof(struct femu_cache_slot),&ready,sizeof(ready)) || ready) continue;
            ring->busy[candidate]=true; ring->next=(candidate+1)%FEMU_CACHE_RX_SLOTS;
            slot=candidate; break;
        }
        qemu_mutex_unlock(&ring->slots_lock);
        failed=slot<0;
    }
    if(publish) {
        struct femu_cache_slot record={.token=cpu_to_le64(item->event.token),
            .cookie=cpu_to_le64(item->event.cookie),.lba=cpu_to_le64(item->event.lba),
            .bytes=cpu_to_le64(item->event.bytes),.load_ns=cpu_to_le64(item->event.load_ns),
            .dma_start_ns=cpu_to_le64(item->dma_start_ns),.dma_ns=cpu_to_le64(now),
            .nsid=cpu_to_le32(item->event.nsid)};
        uint64_t address=ring->rx.descriptor_dma+slot*sizeof(record),ready=cpu_to_le64(1);
        failed=femu_dma_write(n,address,&record,sizeof(record)) ||
               femu_dma_write(n,address,&ready,sizeof(ready));
    } else if(!failed) {
        uint64_t bytes=item->event.bytes,at=0;
        while(at<bytes) {
            unsigned page=at/FEMU_CACHE_PAGE_BYTES;
            uint64_t address=ring->rx.pages[slot][page];
            uint64_t length=MIN(bytes-at,FEMU_CACHE_PAGE_BYTES);
            while(at+length<bytes && (at+length)%FEMU_CACHE_PAGE_BYTES==0 &&
                  ring->rx.pages[slot][(at+length)/FEMU_CACHE_PAGE_BYTES]==address+length)
                length+=MIN(bytes-at-length,FEMU_CACHE_PAGE_BYTES);
            item->segments++;
            if(femu_dma_write(n,address,item->data+at,length)) { failed=true; break; }
            at+=length;
        }
    }
    if(profiling && !publish) clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_end);
    uint64_t end=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    NvmeRequest transfer={.ns=NULL};
    if(!publish && !failed) {
        transfer.ns=&n->namespaces[item->event.nsid-1];
        transfer.stime=transfer.expire_time=now;
        transfer.xfer_bytes=item->event.bytes;
        nvme_charge_link(n,&transfer);
    }
    if((publish || failed) && slot>=0) {
        qemu_mutex_lock(&ring->slots_lock);
        ring->busy[slot]=false;
        qemu_mutex_unlock(&ring->slots_lock);
    }
    uint64_t update_begin=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    qemu_mutex_lock(&csd->cache_lock);
    item->slot=slot;
    item->update_lock_ns+=qemu_clock_get_ns(QEMU_CLOCK_REALTIME)-update_begin;
    if(publish) item->publish_end_ns=end;
    if(publish || failed) {
        item->published=!failed; item->dropped=failed;
        qatomic_set(&csd->cache_dirty,1);
        if(failed) qatomic_inc(&csd->cache_dropped);
        if(publish) item->dma_ns=end;
        uint64_t physical=item->event.lba << n->namespaces[item->event.nsid-1].lbaf.lbads;
        if(publish && !failed && !(physical%FEMU_CACHE_PAGE_BYTES)) {
            uint64_t page=physical/FEMU_CACHE_PAGE_BYTES;
            uint64_t pages=item->event.bytes/FEMU_CACHE_PAGE_BYTES;
            for(uint64_t p=page;p<page+pages && p<ring->npages;++p)
                ring->sent[p/8]|=1U<<(p%8);
        }
    } else {
        ring->dma_bytes+=item->event.bytes;
        item->dma_bytes=item->event.bytes;
        if(profiling) item->copy_cpu_ns=(cpu_end.tv_sec-cpu_begin.tv_sec)*1000000000LL+cpu_end.tv_nsec-cpu_begin.tv_nsec;
        item->copy_end_ns=end; item->link_end_ns=transfer.expire_time;
        item->dma_ns=MAX(end,(uint64_t)transfer.expire_time);
        if(qemu_clock_get_ns(QEMU_CLOCK_REALTIME)>=item->dma_ns) {
            item->data_done=true; qatomic_set(&csd->cache_dirty,1);
        }
    }
    item->dma_active--; ring->active--;
    qemu_cond_broadcast(&csd->cache_cond);
    qemu_mutex_unlock(&csd->cache_lock);
    return true;
}

static void *csd_cache_worker(void *opaque)
{
    femu_bind_device_thread();
    FemuCsdState *csd=opaque;
    rcu_register_thread();
    while(csd_cache_dma(csd)) {}
    rcu_unregister_thread();
    return NULL;
}

/* Called after clearing dataplane_started. Taking the cache mutex prevents
 * a new DMA claim from racing this check; active copies signal completion. */
void femu_csd_cache_quiesce(FemuCtrl *n)
{
    FemuCsdState *csd=csd_state(n);
    if(!csd) return;
    qemu_mutex_lock(&csd->cache_lock);
    for(;;) {
        bool active=false;
        for(unsigned i=0;i<ARRAY_SIZE(csd->cache);i++) active|=csd->cache[i].dma_active!=0;
        if(!active) break;
        qemu_cond_wait(&csd->cache_cond,&csd->cache_lock);
    }
    qemu_mutex_unlock(&csd->cache_lock);
}

/* Controller reset invalidates the Guest's registered receive mappings. */
void femu_csd_cache_reset(FemuCtrl *n)
{
    FemuCsdState *csd=csd_state(n);
    if(!csd) return;
    qemu_mutex_lock(&csd->lock);
    GHashTableIter it; gpointer session;
    g_hash_table_iter_init(&it,csd->sessions);
    while(g_hash_table_iter_next(&it,NULL,&session)) csd_cache_ring_stop(csd,session);
    qemu_mutex_unlock(&csd->lock);
}

/* The FTL only retires input references, after both CU and DMA completion. */
void femu_csd_cache_poll(FemuCtrl *n)
{
    FemuCsdState *csd=csd_state(n);
    if(!csd || !qatomic_read(&csd->cache_work) ||
       !qatomic_xchg(&csd->cache_dirty,0)) return;
    FemuCacheItem logs[128];
    unsigned count=0;
    qemu_mutex_lock(&csd->lock);
    qemu_mutex_lock(&csd->cache_lock);
    uint64_t now=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    for(unsigned i=0;i<ARRAY_SIZE(csd->cache);i++) {
        FemuCacheItem *item=&csd->cache[i];
        if(item->event.token && (item->data_done || item->published || item->dropped))
            csd_cache_wake_graph(csd,item->graph);
        if(item->event.token && !item->dma_active && item->cu_ns &&
           (item->published || item->dropped)) {
            if(item->graph->flags&FEMU_CSD_GRAPH_PROFILE) logs[count++]=*item;
            csd_cache_finish(csd,item);
        }
    }
    qemu_mutex_unlock(&csd->cache_lock);
    qemu_mutex_unlock(&csd->lock);
    for(unsigned i=0;i<count;i++) csd_cache_log(&logs[i],now);
}

static void csd_graph_defer(FemuCtrl *n, NvmeRequest *req, int poller)
{
    FemuCsdState *csd = csd_state(n);
    FemuCsdGraph *g = req->opaque;
    req->opaque = NULL;
    qemu_mutex_lock(&csd->lock);
    g->poller = poller;
    csd_graph_ready_locked(csd, g);
    qemu_mutex_unlock(&csd->lock);
}

static uint16_t csd_exec_graph(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                               NvmeRequest *req)
{
    FemuCsdState *csd = csd_state(n);
    uint32_t bytes = le32_to_cpu(cmd->cdw10);
    uint16_t status = NVME_INVALID_FIELD | NVME_DNR;
    FemuCsdGraph *g;
    if (bytes < sizeof(struct femu_graph_header) || bytes > FEMU_CSD_GRAPH_MAX_BYTES ||
        n->power_loss || nvme_ns_ms(ns)) {
        return status;
    }
    g = g_new0(FemuCsdGraph, 1);
    g->data = g_malloc(bytes); g->bytes = bytes;
    status = dma_write_cmd(n, cmd, g->data, bytes);
    if (status) { g_free(g->data); g_free(g); return status; }
    struct femu_graph_header h;
    memcpy(&h, g->data, sizeof(h));
    g->count = le32_to_cpu(h.count); g->flags = le32_to_cpu(h.flags);
    g->cookie = le64_to_cpu(h.cookie);
    status = NVME_INVALID_FIELD | NVME_DNR;
    qemu_mutex_lock(&csd->lock);
    g->session = g_hash_table_lookup(csd->sessions, GUINT_TO_POINTER(le64_to_cpu(h.session)));
    if (!g->count || g->count > FEMU_CSD_GRAPH_MAX_NODES ||
        bytes < sizeof(h) + g->count * sizeof(struct femu_graph_node) ||
        le32_to_cpu(h.bytes) != bytes || h.reserved ||
        (g->flags & ~(FEMU_CSD_GRAPH_SERIAL | FEMU_CSD_GRAPH_PROFILE | FEMU_CSD_GRAPH_CACHE)) || !g->cookie ||
        le64_to_cpu(h.session) > UINT32_MAX || !g->session ||
        g->session->ns != ns || csd->graph_count >= 64) {
        goto fail;
    }
    if (g_hash_table_remove(g->session->cancelled, &g->cookie)) {
        status = NVME_CMD_ABORT_REQ;
        goto fail;
    }
    FemuCsdGraph *other;
    QTAILQ_FOREACH(other, &csd->graphs, entry) {
        if (other->session == g->session && other->cookie == g->cookie) { goto fail; }
    }
    uint64_t all = UINT64_MAX >> (64 - g->count), done = 0;
    for (unsigned i = 0; i < g->count; i++) {
        FemuCsdGraphNode *v = &g->nodes[i];
        struct femu_graph_node *w = &v->wire;
        memcpy(w, g->data + sizeof(h) + i * sizeof(*w), sizeof(*w));
        w->dependencies = le64_to_cpu(w->dependencies);
        w->operation = le32_to_cpu(w->operation); w->program = le32_to_cpu(w->program);
        w->count = le32_to_cpu(w->count); w->payload = le32_to_cpu(w->payload);
        w->afdm = le32_to_cpu(w->afdm); w->afdm_offset = le64_to_cpu(w->afdm_offset);
        w->data_offset = le32_to_cpu(w->data_offset); w->data_bytes = le32_to_cpu(w->data_bytes);
        v->graph = g;
        v->remaining = ctpop64(w->dependencies);
        if (!v->remaining) { g->ready |= 1ULL << i; }
        if ((w->dependencies & ~all) || w->reserved) { goto fail; }
        if (w->operation == FEMU_GRAPH_EXEC) {
            FemuCsdProgram *p = csd_get_program_locked(csd, w->program);
            if (!p || !p->active || w->count > 64 ||
                (w->count && !csd_graph_span(g, w->payload, (uint64_t)w->count * sizeof(NvmeCsdMemoryRange))) ||
                (w->data_bytes && !csd_graph_span(g, w->data_offset, w->data_bytes))) { goto fail; }
            FemuCsdJob *job = v->job = g_new0(FemuCsdJob, 1);
            status = csd_build_exec_args_locked(csd,
                w->count ? (void *)(g->data + w->payload) : NULL, w->count, job);
            if (status) { goto fail; }
            p->refs++; job->program = p; job->runtime = p->runtime;
            job->graph = g; job->graph_node = i;
            job->args.cparam1 = le64_to_cpu(w->cparam1); job->args.cparam2 = le64_to_cpu(w->cparam2);
            job->args.data_buffer = w->data_bytes ? g->data + w->data_offset : NULL;
            job->args.buffer_len = w->data_bytes;
        } else if (w->operation == FEMU_GRAPH_LOAD || w->operation == FEMU_GRAPH_STORE) {
            FemuCsdAfdm *m = csd_get_afdm_locked(csd, w->afdm);
            if (!m || !w->count || !csd_graph_span(g, w->payload,
                    (uint64_t)w->count * sizeof(struct femu_graph_range))) { goto fail; }
            uint64_t total = 0;
            for (unsigned r = 0; r < w->count; r++) {
                struct femu_graph_range range;
                memcpy(&range, g->data + w->payload + r * sizeof(range), sizeof(range));
                uint64_t blocks = le32_to_cpu(range.blocks), lba = le64_to_cpu(range.lba);
                if (le32_to_cpu(range.nsid) != ns->id || !blocks ||
                    lba > le64_to_cpu(ns->id_ns.nsze) || blocks > le64_to_cpu(ns->id_ns.nsze) - lba ||
                    blocks > (UINT64_MAX - total) >> ns->lbaf.lbads) { goto fail; }
                total += blocks << ns->lbaf.lbads;
            }
            if (csd_check_afdm_range(m, w->afdm_offset, total)) { goto fail; }
            v->afdm = m; m->refs++;
        } else { goto fail; }
        status = NVME_INVALID_FIELD | NVME_DNR;
    }
    for (unsigned pass = 0; pass < g->count; pass++) {
        uint64_t before = done;
        for (unsigned i = 0; i < g->count; i++) {
            if (!(g->nodes[i].wire.dependencies & ~done)) { done |= 1ULL << i; }
        }
        if (done == all) { break; }
        if (done == before) { goto fail; }
    }
    if (done != all) { goto fail; }
    for (unsigned i = 0; i < g->count; i++) {
        uint64_t deps = g->nodes[i].wire.dependencies;
        while (deps) {
            unsigned j = ctz64(deps); deps &= deps - 1;
            g->nodes[j].dependents |= 1ULL << i;
        }
    }
    g->req = req; g->session->graphs++; qatomic_inc(&csd->graph_count);
    QTAILQ_INSERT_TAIL(&csd->graphs, g, entry);
    req->opaque = g; req->defer = csd_graph_defer;
    qemu_mutex_unlock(&csd->lock);
    return NVME_SUCCESS;
fail:
    for (unsigned i = 0; i < MIN(g->count, FEMU_CSD_GRAPH_MAX_NODES); i++) {
        if (g->nodes[i].job) { csd_job_free_locked(g->nodes[i].job); }
        if (g->nodes[i].afdm) { csd_afdm_unref(g->nodes[i].afdm); }
    }
    qemu_mutex_unlock(&csd->lock);
    g_free(g->data); g_free(g); return status;
}

static void csd_graph_computed_locked(FemuCsdState *csd, FemuCsdJob *job,
                                      uint16_t status, int64_t result, int64_t host_ns)
{
    FemuCsdGraphNode *v = &job->graph->nodes[job->graph_node];
    NvmeRequest completion = { .expire_time = job->arrival };
    job->req = &completion;
    csd_finish_locked(csd, job, status, result, host_ns);
    job->req = NULL;
    v->result = result; v->status = status;
    v->deadline = MAX(completion.expire_time, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    v->state = GRAPH_COMPUTED;
    if (job->graph->flags & FEMU_CSD_GRAPH_PROFILE) {
        job->graph->session->cu_calls++;
        job->graph->session->cu_ns += host_ns;
    }
    csd_graph_event_locked(csd, v);
}

static uint16_t csd_graph_spill_begin(FemuCsdState *csd, FemuCsdGraph *g,
                                      FemuCsdGraphNode *v)
{
    FemuCsdJob *job = v->job;
    uint64_t action = v->result >> 60, value = v->result & GRAPH_IO_MASK;
    if (!job->args.numr || v->wire.cparam2 || !value) { return NVME_INVALID_FIELD | NVME_DNR; }
    uint64_t available = job->args.mr_len[job->args.numr - 1];
    FemuCsdSpill *spill;
    if (action == 1) {
        uint64_t page_bytes = (uint64_t)g->session->ns->ssd->sp.secsz * g->session->ns->ssd->sp.secs_per_pg;
        uint64_t unit = MAX(page_bytes, 1ULL << g->session->ns->lbaf.lbads);
        uint64_t pages = DIV_ROUND_UP(value, unit) * (unit / page_bytes), run = 0, first = 0;
        if (value > available || pages > csd->spill_pages || csd->next_spill > GRAPH_IO_MASK) {
            return NVME_CAP_EXCEEDED | NVME_DNR;
        }
        for (uint64_t i = 0; i < csd->spill_pages; i++) {
            if (csd->spill_bitmap[i / 8] & (1U << (i % 8))) { run = 0; }
            else if (run || i % (unit / page_bytes) == 0) { run++; }
            if (run == pages) { first = i + 1 - run; break; }
        }
        if (run != pages) { return NVME_CAP_EXCEEDED | NVME_DNR; }
        spill = g_new0(FemuCsdSpill, 1);
        *spill = (FemuCsdSpill){ .id = csd->next_spill++, .page = first,
            .pages = pages, .bytes = value, .creator = g->cookie, .session = g->session };
        for (uint64_t i = first; i < first + pages; i++) { csd->spill_bitmap[i / 8] |= 1U << (i % 8); }
        g_hash_table_insert(csd->spills, &spill->id, spill);
    } else {
        spill = g_hash_table_lookup(csd->spills, &value);
        if (!spill || spill->session != g->session || spill->busy ||
            (action == 2 && spill->bytes > available)) { return NVME_INVALID_FIELD | NVME_DNR; }
    }
    v->io_action = action; v->io_token = spill->id; v->io_bytes = spill->bytes;
    v->transferred = 0; v->spill = spill; spill->busy++;
    v->state = GRAPH_IO;
    return NVME_SUCCESS;
}

static uint16_t csd_graph_read_begin(FemuCsdGraph *g, FemuCsdGraphNode *v)
{
    FemuCsdJob *job = v->job;
    NvmeNamespace *ns = g->session->ns;
    uint64_t count = v->result & GRAPH_IO_MASK, total = 0;
    if (!job->args.numr || v->wire.cparam2 || !count ||
        count > FEMU_CSD_GRAPH_MAX_NODES ||
        count * sizeof(*v->reads) > job->args.mr_len[job->args.numr - 1]) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    struct femu_graph_read *reads = g_new(struct femu_graph_read, count);
    memcpy(reads, job->args.mr_addr[job->args.numr - 1], count * sizeof(*reads));
    /* Validate the whole snapshot before issuing I/O. In particular a target
     * may overlap the descriptor MR without changing subsequent requests. */
    for (unsigned i = 0; i < count; i++) {
        struct femu_graph_read *r = &reads[i];
        r->nsid = le32_to_cpu(r->nsid); r->blocks = le32_to_cpu(r->blocks);
        r->afdm_index = le32_to_cpu(r->afdm_index); r->lba = le64_to_cpu(r->lba);
        r->offset = le64_to_cpu(r->offset);
        uint64_t bytes = (uint64_t)r->blocks << ns->lbaf.lbads;
        if (r->reserved || r->nsid != ns->id || !r->blocks ||
            r->afdm_index >= job->args.numr ||
            !job->afdms[r->afdm_index] ||
            r->lba > le64_to_cpu(ns->id_ns.nsze) ||
            r->blocks > le64_to_cpu(ns->id_ns.nsze) - r->lba ||
            r->offset > job->args.mr_len[r->afdm_index] ||
            bytes > job->args.mr_len[r->afdm_index] - r->offset ||
            bytes > UINT64_MAX - total) {
            g_free(reads); return NVME_INVALID_FIELD | NVME_DNR;
        }
        total += bytes;
    }
    /* Aliased destinations keep descriptor order, including any HPCP DMA
     * that must finish before the same bytes are overwritten. */
    v->serial_reads = false;
    for (unsigned i = 0; i < count; i++) {
        for (unsigned j = 0; j < i; j++) {
            if (job->afdms[reads[i].afdm_index] == job->afdms[reads[j].afdm_index] &&
                reads[i].offset < reads[j].offset + ((uint64_t)reads[j].blocks << ns->lbaf.lbads) &&
                reads[j].offset < reads[i].offset + ((uint64_t)reads[i].blocks << ns->lbaf.lbads)) {
                v->serial_reads = true;
            }
        }
    }
    v->reads = reads; v->range = 0; v->range_blocks = 0;
    v->io_action = 4; v->io_token = count; v->io_bytes = total;
    v->transferred = 0; v->state = GRAPH_IO;
    return NVME_SUCCESS;
}

static void csd_graph_io(FemuCsdState *csd, FemuCsdGraph *g, FemuCsdGraphNode *v,
                          uint64_t now)
{
    FemuCtrl *n = csd->n;
    NvmeNamespace *ns = g->session->ns;
    bool batch_read = (v->job && v->io_action == 4 && !v->serial_reads) ||
                      (!v->job && v->wire.operation == FEMU_GRAPH_LOAD);
    uint64_t completed = now;
    unsigned issued = 0;
next_read:
    if (g->status) { return; }
    uint64_t lat = 0, bytes = 0;
    uint64_t begin = (g->flags & FEMU_CSD_GRAPH_PROFILE) ?
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
    uint16_t status;
    FemuCsdIoTiming timing={0};
    struct timespec cpu_begin,cpu_end;
    if(begin) clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_begin);
    NvmeRequest req = { .ns = ns, .stime = now, .expire_time = now };
    if (v->job && v->io_action == 4) {
        const struct femu_graph_read *r = &v->reads[v->range];
        uint32_t blocks = MIN((uint64_t)65536, r->blocks - v->range_blocks);
        if ((g->flags & FEMU_CSD_GRAPH_CACHE) && g->session->cache_ring)
            blocks=MIN(blocks,FEMU_CACHE_BLOCK_BYTES >> ns->lbaf.lbads);
        bytes = (uint64_t)blocks << ns->lbaf.lbads;
        FemuCsdAfdm *afdm=v->job->afdms[r->afdm_index];
        uint64_t offset=r->offset+(v->range_blocks << ns->lbaf.lbads);
        uintptr_t first=(uintptr_t)afdm->data+offset;
        /* Reentry can reuse this input region. Never overwrite bytes still
         * being read by DMA; completion wakes this graph through ready_queue. */
        bool busy=false;
        qemu_mutex_lock(&csd->cache_lock);
        for(unsigned i=0;i<ARRAY_SIZE(csd->cache);i++) {
            FemuCacheItem *item=&csd->cache[i];
            uintptr_t at=(uintptr_t)item->data;
            if(!item->event.token || item->graph!=g || item->data_done || item->published || item->dropped ||
               first>=at+item->event.bytes || at>=first+bytes) continue;
            if(!item->dma_active && !item->dma_start_ns) {
                item->dropped=true; qatomic_inc(&csd->cache_dropped);
                qatomic_set(&csd->cache_dirty,1);
            } else busy=true;
        }
        qemu_mutex_unlock(&csd->cache_lock);
        if(busy) {
            if (issued) {
                v->deadline = completed;
                v->state = GRAPH_IO_DONE;
                csd_graph_event_locked(csd, v);
                return;
            }
            if(!v->cache_wait_begin) v->cache_wait_begin=now;
            v->state=GRAPH_CACHE_WAIT;
            return;
        }
        if(v->cache_wait_begin) {
            if(begin) {
                qemu_mutex_unlock(&csd->lock);
                fprintf(stderr,"CACHE_REUSE_WAIT session=%u cookie=%" PRIu64
                " node=%u begin=%" PRIu64 " end=%" PRIu64 "\n",
                g->session->id,g->cookie,(unsigned)(v-g->nodes),v->cache_wait_begin,now);
                qemu_mutex_lock(&csd->lock);
            }
            v->cache_wait_begin=0;
        }
        NvmeCsdNvmToAfdmCmd *copy = (void *)&req.cmd;
        copy->opcode = NVME_CMD_CSD_NVM_TO_AFDM;
        copy->slba = cpu_to_le64(r->lba + v->range_blocks);
        copy->nlb = cpu_to_le16(blocks - 1);
        copy->offset = cpu_to_le64(r->offset + (v->range_blocks << ns->lbaf.lbads));
        v->cache_afdm=afdm;
        status=csd_cache_prepare(csd,v,offset,bytes);
        if(status) { g->status=status; return; }
        status = csd_nvm_copy_locked(n, ns, &req, &lat, afdm,begin?&timing:NULL);
        if (status) { g->status = status; return; }
        v->cache_lba=le64_to_cpu(copy->slba); v->cache_bytes=bytes; v->cache_offset=offset;
        v->transferred += bytes; v->range_blocks += blocks;
        if (v->range_blocks == r->blocks) { v->range++; v->range_blocks = 0; }
    } else if (v->job) {
        FemuCsdSpill *s = v->spill;
        if (v->io_action == 3) {
            s->busy--; v->spill = NULL;
            lat = csd_spill_free_locked(csd, s);
            v->transferred = v->io_bytes;
        } else {
            uint64_t page_bytes = (uint64_t)ns->ssd->sp.secsz * ns->ssd->sp.secs_per_pg;
            uint64_t offset = s->page * page_bytes + v->transferred;
            bytes = MIN(v->io_bytes - v->transferred, 65536ULL << ns->lbaf.lbads);
            bool write = v->io_action == 1;
            req.cmd.opcode = write ? NVME_CMD_WRITE : NVME_CMD_READ;
            req.slba = (csd->spill_base * page_bytes + offset) >> ns->lbaf.lbads;
            req.nlb = DIV_ROUND_UP(bytes, 1ULL << ns->lbaf.lbads); req.is_write = write;
            NvmeRwCmd *rw = (void *)&req.cmd;
            rw->slba = cpu_to_le64(req.slba); rw->nlb = cpu_to_le16(req.nlb - 1);
            rw->control = cpu_to_le16(write ? NVME_RW_FUA : 0);
            /* This FTL owner retains the graph, busy spill and AFDM. */
            qemu_mutex_unlock(&csd->lock);
            uint64_t host_pages = ns->ssd->host_write_pages;
            uint64_t stamp=begin?qemu_clock_get_ns(QEMU_CLOCK_REALTIME):0;
            lat = bb_ftl_process_req(n, ns, &req);
            if(begin) timing.ftl_ns+=qemu_clock_get_ns(QEMU_CLOCK_REALTIME)-stamp;
            ns->ssd->host_write_pages = host_pages;
            if (req.status) {
                qemu_mutex_lock(&csd->lock);
                g->status = req.status; return;
            }
            uint8_t *io = v->job->args.mr_addr[v->job->args.numr - 1];
            if(begin) stamp=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            if (write) { memcpy(csd->spill_space + offset, io + v->transferred, bytes); }
            else { memcpy(io + v->transferred, csd->spill_space + offset, bytes); }
            if(begin) { uint64_t at=qemu_clock_get_ns(QEMU_CLOCK_REALTIME); timing.copy_ns+=at-stamp; stamp=at; }
            qemu_mutex_lock(&csd->lock);
            if(begin) timing.csd_lock_ns+=qemu_clock_get_ns(QEMU_CLOCK_REALTIME)-stamp;
            v->transferred += bytes;
        }
    } else {
        struct femu_graph_range range;
        memcpy(&range, g->data + v->wire.payload + v->range * sizeof(range), sizeof(range));
        uint32_t blocks = MIN((uint64_t)65536, le32_to_cpu(range.blocks) - v->range_blocks);
        if ((g->flags & FEMU_CSD_GRAPH_CACHE) && g->session->cache_ring && v->wire.operation == FEMU_GRAPH_LOAD)
            blocks=MIN(blocks,FEMU_CACHE_BLOCK_BYTES >> ns->lbaf.lbads);
        bytes = (uint64_t)blocks << ns->lbaf.lbads;
        NvmeCsdNvmToAfdmCmd *copy = (void *)&req.cmd;
        copy->opcode = v->wire.operation == FEMU_GRAPH_STORE ? NVME_CMD_CSD_AFDM_TO_NVM : NVME_CMD_CSD_NVM_TO_AFDM;
        copy->slba = cpu_to_le64(le64_to_cpu(range.lba) + v->range_blocks);
        copy->nlb = cpu_to_le16(blocks - 1); copy->id = cpu_to_le32(v->wire.afdm);
        copy->offset = cpu_to_le64(v->wire.afdm_offset + v->transferred);
        v->cache_afdm=v->afdm;
        status = csd_cache_prepare(csd, v, le64_to_cpu(copy->offset), bytes);
        if (status) { g->status = status; return; }
        v->cache_bytes=bytes;
        status = csd_nvm_copy_locked(n, ns, &req, &lat, v->afdm,begin?&timing:NULL);
        if (status) { g->status = status; return; }
        v->cache_lba=le64_to_cpu(copy->slba); v->cache_bytes=bytes;
        v->cache_offset=le64_to_cpu(copy->offset);
        v->transferred += bytes; v->range_blocks += blocks;
        if (v->range_blocks == le32_to_cpu(range.blocks)) { v->range++; v->range_blocks = 0; }
    }
    /* FTL calls still have one owner. Their common arrival lets NAND/channel
     * state schedule independent ranges concurrently; the batch completes
     * at its slowest deadline, rather than summing per-range waits. */
    v->deadline = now + lat;
    completed = MAX(completed, v->deadline);
    v->state = GRAPH_IO_DONE;
    if (batch_read && v->cache_candidate) {
        if (!v->read_completions) {
            v->read_completions = g_new(FemuCsdReadCompletion, FEMU_CSD_GRAPH_MAX_NODES);
        }
        v->read_completions[v->read_completed++] = (FemuCsdReadCompletion){
            v->cache_afdm, v->cache_lba, v->cache_offset, v->cache_bytes };
        v->cache_candidate = false;
    }
    if (begin) {
        g->session->io_calls++; g->session->io_ns += lat;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_end);
        uint64_t end=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        g->session->io_work_ns += end - begin;
        qemu_mutex_unlock(&csd->lock);
        fprintf(stderr,"CSD_IO_SPAN session=%u cookie=%" PRIu64 " node=%u action=%u begin=%" PRIu64
            " end=%" PRIu64 " deadline=%" PRIu64 " bytes=%" PRIu64 " cpu_ns=%" PRIu64
            " ftl_ns=%" PRIu64 " backend_lock_ns=%" PRIu64 " copy_ns=%" PRIu64 " csd_lock_ns=%" PRIu64 "\n",
            g->session->id,g->cookie,(unsigned)(v-g->nodes),v->job?(unsigned)v->io_action:v->wire.operation,
            begin,end,v->deadline,v->io_action==3?0:bytes,
            (uint64_t)((cpu_end.tv_sec-cpu_begin.tv_sec)*1000000000LL+cpu_end.tv_nsec-cpu_begin.tv_nsec),
            timing.ftl_ns,timing.backend_lock_ns,timing.copy_ns,timing.csd_lock_ns);
        qemu_mutex_lock(&csd->lock);
    }
    if (batch_read && ++issued < FEMU_CSD_GRAPH_MAX_NODES &&
        (v->job ? v->transferred < v->io_bytes : v->range < v->wire.count)) {
        goto next_read;
    }
    v->deadline = completed;
    csd_graph_event_locked(csd, v);
}

static void csd_graph_compute_ready(FemuCsdState *csd, FemuCsdGraphNode *v, uint64_t now)
{
    FemuCsdJob *job = v->job;
    job->arrival = now;
    job->reserved_end = job->runtime ? csd_hold_cu_locked(csd, now, job->runtime) : 0;
    v->state = GRAPH_COMPUTING;
    QTAILQ_INSERT_TAIL(&csd->pending, job, entry);
    qemu_cond_signal(&csd->job_cond);
}

static void csd_cache_consumed(FemuCsdState *csd, FemuCsdGraphNode *v, uint64_t now)
{
    if(!v->graph->cache_pending) return;
    qemu_mutex_lock(&csd->cache_lock);
    unsigned index=v-v->graph->nodes;
    for (unsigned i=0;i<ARRAY_SIZE(csd->cache);i++) {
        FemuCacheItem *item=&csd->cache[i];
        if (item->event.token && item->graph==v->graph && (item->consumers&(1ULL<<index))) {
            item->consumers&=~(1ULL<<index);
            if (!item->consumers) {
                item->cu_ns=now; qatomic_set(&csd->cache_dirty,1);
            }
        }
    }
    qemu_mutex_unlock(&csd->cache_lock);
}

static void csd_graph_node_done(FemuCsdState *csd, FemuCsdGraphNode *v)
{
    FemuCsdGraph *g = v->graph;
    unsigned index = v - g->nodes;
    if (v->job) {
        /* The compute event deadline excludes FTL event-dispatch lateness. */
        uint64_t now=v->state==GRAPH_COMPUTED ? v->deadline :
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        csd_cache_consumed(csd,v,now);
    }
    v->state = GRAPH_DONE; g->done |= 1ULL << index; g->active--;
    if (v->spill) { v->spill->busy--; v->spill = NULL; }
    if (!g->status) {
        g->result = v->result;
        uint64_t next = v->dependents;
        while (next) {
            unsigned i = ctz64(next); next &= next - 1;
            if (!--g->nodes[i].remaining) { g->ready |= 1ULL << i; }
        }
    }
    if (!g->active || (!g->status && !(g->flags & FEMU_CSD_GRAPH_SERIAL) && g->ready)) {
        csd_graph_ready_locked(csd, g);
    }
}

void femu_csd_graph_poll(FemuCtrl *n)
{
    FemuCsdState *csd = csd_state(n);
    if (!csd || !qatomic_read(&csd->graph_count)) { return; }
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (!qatomic_read(&csd->ready_count) && now < qatomic_read(&csd->graph_deadline)) { return; }
    qemu_mutex_lock(&csd->lock);
    now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    FemuCsdGraphNode *v;
    while ((v = pqueue_peek(csd->graph_events)) && v->deadline <= now) {
        FemuCsdGraph *g = v->graph;
        uint64_t begin = (g->flags & FEMU_CSD_GRAPH_PROFILE) ? now : 0;
        pqueue_pop(csd->graph_events);
        if (begin) { g->session->events++; g->session->late_ns += now - v->deadline; }
        if (g->status) { csd_graph_node_done(csd, v); }
        else if (v->state == GRAPH_COMPUTED) {
            csd_cache_consumed(csd,v,v->deadline);
            if (v->status || (int64_t)v->result < 0) {
                g->status = v->status ? v->status : NVME_INTERNAL_DEV_ERROR;
                g->result = v - g->nodes + 1;
                csd_graph_node_done(csd, v);
            } else if ((v->result >> 60) >= 1 && (v->result >> 60) <= 4) {
                g->status = (v->result >> 60) == 4 ? csd_graph_read_begin(g, v) :
                    csd_graph_spill_begin(csd, g, v);
                if (!g->status) { csd_graph_io(csd, g, v, now); }
                if (g->status) { g->result = v - g->nodes + 1; csd_graph_node_done(csd, v); }
            } else { csd_graph_node_done(csd, v); }
        } else {
            /* All ranges in this bounded window are complete. Keep every
             * LOAD's HPCP metadata; only then resume/continue the parent. */
            for (unsigned i = 0; i < v->read_completed; i++) {
                const FemuCsdReadCompletion *r = &v->read_completions[i];
                v->cache_afdm = r->afdm; v->cache_lba = r->lba;
                v->cache_offset = r->offset; v->cache_bytes = r->bytes;
                v->cache_candidate = true;
                csd_cache_publish(csd, v, now);
            }
            v->read_completed = 0;
            csd_cache_publish(csd,v,now);
            bool done = v->job ? v->transferred == v->io_bytes : v->range == v->wire.count;
            if (!done) {
                csd_graph_io(csd, g, v, now);
                if (g->status) { g->result = v - g->nodes + 1; csd_graph_node_done(csd, v); }
            } else if (v->job) {
                g_free(v->reads); v->reads = NULL;
                if (v->spill) { v->spill->busy--; v->spill = NULL; }
                v->job->args.cparam2 = v->io_token; v->io_action = 0;
                csd_graph_compute_ready(csd, v, now);
            } else { v->result = v->transferred; csd_graph_node_done(csd, v); }
        }
        now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (begin) { g->session->dispatch_ns += now - begin; }
    }
    csd_graph_cache_deadline(csd);
    FemuCsdGraph *g;
    while ((g = QTAILQ_FIRST(&csd->graph_ready))) {
        QTAILQ_REMOVE(&csd->graph_ready, g, ready_entry); g->queued = false;
        qatomic_dec(&csd->ready_count);
        if (!g->poller && g->req) { continue; } /* defer will enqueue it again */
        uint64_t begin = (g->flags & FEMU_CSD_GRAPH_PROFILE) ? now : 0;
        for(unsigned i=0;i<g->count;i++) {
            v=&g->nodes[i];
            if(v->state!=GRAPH_CACHE_WAIT) continue;
            if(!g->status) csd_graph_io(csd,g,v,now);
            if(g->status) csd_graph_node_done(csd,v);
            now=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        }
        while (!g->status && g->ready && (!(g->flags & FEMU_CSD_GRAPH_SERIAL) || !g->active)) {
            unsigned i = ctz64(g->ready); g->ready &= g->ready - 1;
            v = &g->nodes[i]; g->active++;
            if (v->job) { csd_graph_compute_ready(csd, v, now); }
            else {
                csd_graph_io(csd, g, v, now);
                if (g->status) { g->result = i + 1; csd_graph_node_done(csd, v); }
            }
            now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        }
        if (begin) { g->session->dispatch_ns += now - begin; }
        uint64_t all = UINT64_MAX >> (64 - g->count);
        if (g->active || (!g->status && g->done != all)) { continue; }
        if (g->status) {
            /* All issued CU/I/O work has stopped. Consumers below a failed
             * node will never run, so they cannot acknowledge cached input.
             * Drop optional population, but keep each reference until its
             * actual DMA worker leaves (cache_poll checks dma_active). */
            if (g->cache_pending) {
                qemu_mutex_lock(&csd->cache_lock);
                for (unsigned i=0;i<ARRAY_SIZE(csd->cache);i++) {
                    FemuCacheItem *item=&csd->cache[i];
                    if (!item->event.token || item->graph!=g) { continue; }
                    item->consumers=0;
                    if (!item->cu_ns) { item->cu_ns=now; }
                    item->dropped=true;
                }
                qatomic_set(&csd->cache_dirty,1);
                qemu_mutex_unlock(&csd->cache_lock);
            }
            GHashTableIter it; gpointer value;
            for (;;) {
                FemuCsdSpill *found = NULL;
                g_hash_table_iter_init(&it, csd->spills);
                while (g_hash_table_iter_next(&it, NULL, &value)) {
                    FemuCsdSpill *s = value;
                    if (s->session == g->session && s->creator == g->cookie && !s->busy) { found = s; break; }
                }
                if (!found) { break; }
                csd_spill_free_locked(csd, found);
            }
        }
        if (!g->cu_done_ns) g->cu_done_ns=now;
        /* Input is reused only after CU and actual DMA both finish. Host
         * cache publication never holds a graph or its AFDM references. */
        if(g->cache_pending) continue;
        if (g->req) {
            NvmeRequest *req = g->req;
            req->status = g->status; req->cqe.res64 = cpu_to_le64(g->result);
            req->expire_time = now; req->reqlat = now - req->stime;
            struct rte_ring *ring = n->to_poller[g->poller];
            if (femu_ring_enqueue(ring, (void *)&req, 1) != 1) {
                csd_graph_ready_locked(csd, g); break;
            }
            if (g->flags & FEMU_CSD_GRAPH_CACHE) {
                uint64_t compute_end=0;
                for(unsigned i=0;i<g->count;i++) {
                    if(g->nodes[i].job) compute_end=MAX(compute_end,g->nodes[i].deadline);
                }
                /* Input lifetime and the end of the full CSF chain differ. */
                fprintf(stderr,"CACHE_GRAPH session=%u cookie=%" PRIu64
                    " compute=%" PRIu64 " work=%" PRIu64 " done=%" PRIu64 "\n",
                    g->session->id,g->cookie,compute_end,g->cu_done_ns,now);
            }
            if (g->flags & FEMU_CSD_GRAPH_PROFILE) {
                uint64_t lba=0;
                for(unsigned i=0;i<g->count;i++) if(g->nodes[i].wire.operation==FEMU_GRAPH_LOAD) {
                    struct femu_graph_range range;
                    memcpy(&range,g->data+g->nodes[i].wire.payload,sizeof(range));
                    lba=le64_to_cpu(range.lba); break;
                }
                fprintf(stderr,"CSD_GRAPH_TIME session=%u cookie=%" PRIu64 " seen=%" PRId64
                    " work=%" PRIu64 " done=%" PRIu64 " lba=%" PRIu64 " nodes=%u\n",
                    g->session->id,g->cookie,req->stime,g->cu_done_ns,now,lba,g->count);
                g->session->profile = true; g->session->completed_graphs++;
                g->session->graph_ns += req->reqlat;
            }
        }
        csd_graph_destroy_locked(csd, g);
    }
    qemu_mutex_unlock(&csd->lock);
}

bool csd_ftl_storage(FemuCtrl *n, NvmeNamespace *ns, NvmeRequest *req,
                     uint64_t *lat)
{
    if (req->cmd.opcode != FEMU_CSD_GRAPH_SESSION &&
        req->cmd.opcode != NVME_CMD_CSD_NVM_TO_AFDM &&
        req->cmd.opcode != NVME_CMD_CSD_AFDM_TO_NVM) {
        return false;
    }
    *lat = 0;
    if (req->status == NVME_SUCCESS) {
        FemuCsdState *csd = csd_state(n);
        qemu_mutex_lock(&csd->lock);
        req->status = req->cmd.opcode == FEMU_CSD_GRAPH_SESSION ?
            csd_graph_session(csd, ns, req, lat) : csd_nvm_copy_locked(n, ns, req, lat, NULL,NULL);
        qemu_mutex_unlock(&csd->lock);
    }
    return true;
}

static uint16_t csd_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        return nvme_rw(n, ns, cmd, req);
    case NVME_CMD_CSD_ALLOC_FDM:
        return csd_alloc_fdm(n, cmd, req);
    case NVME_CMD_CSD_DEALLOC_AFDM:
        return csd_dealloc_afdm(n, cmd);
    case NVME_CMD_CSD_NVM_TO_AFDM:
    case NVME_CMD_CSD_AFDM_TO_NVM:
        /* The controller FTL thread validates and executes internal copies. */
        return NVME_SUCCESS;
    case NVME_CMD_CSD_EXEC:
        return csd_exec(n, cmd, req);
    case FEMU_CACHE_CONTROL:
        return csd_cache_control(n,cmd);
    case FEMU_CSD_EXEC_GRAPH:
        return csd_exec_graph(n, ns, cmd, req);
    case FEMU_CSD_GRAPH_SESSION:
        return NVME_SUCCESS; /* session cleanup/trim runs on the FTL owner */
    case NVME_CMD_CSD_READ_AFDM:
        return csd_read_afdm(n, cmd, req);
    case NVME_CMD_CSD_WRITE_AFDM:
        return csd_write_afdm(n, cmd, req);
    case NVME_CMD_CSD_CREATE_GROUP:
        return csd_create_group(n, cmd, req);
    case NVME_CMD_CSD_SET_QOS:
        return csd_set_qos(n, cmd);
    case NVME_CMD_CSD_DELETE_GROUP:
        return csd_delete_group(n, cmd);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t csd_get_info(FemuCtrl *n, NvmeCmd *cmd, NvmeCqe *cqe)
{
    FemuCsdState *csd = csd_state(n);
    uint64_t value;

    qemu_mutex_lock(&csd->lock);
    switch (le32_to_cpu(cmd->cdw10)) {
    case NVME_CSD_INFO_VERSION:
        value = 1;
        break;
    case NVME_CSD_INFO_FDM_BYTES:
        value = csd->fdm_capacity;
        break;
    case NVME_CSD_INFO_FDM_USED:
        value = csd->fdm_used;
        break;
    case NVME_CSD_INFO_NR_CU:
        value = csd->params.nr_cu;
        break;
    case NVME_CSD_INFO_FEATURES:
        value = NVME_CSD_FEATURE_STORE | NVME_CSD_FEATURE_RESULT64;
        break;
    default:
        qemu_mutex_unlock(&csd->lock);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    qemu_mutex_unlock(&csd->lock);
    cqe->res64 = cpu_to_le64(value);
    return NVME_SUCCESS;
}

static uint16_t csd_admin_cmd(FemuCtrl *n, NvmeCmd *cmd, NvmeCqe *cqe)
{
    switch (cmd->opcode) {
    case NVME_ADM_CMD_CSD_GET_INFO:
        return csd_get_info(n, cmd, cqe);
    case NVME_ADM_CMD_CSD_MRS_MGMT:
        return csd_mrs_mgmt(n, cmd, cqe);
    case NVME_ADM_CMD_CSD_COMPUTE_LOAD:
    case NVME_ADM_CMD_CSD_COMPUTE_LOAD_DATA:
        return csd_compute_load(n, cmd);
    case NVME_ADM_CMD_CSD_COMPUTE_ACTIVATE:
        return csd_compute_activate(n, cmd);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

int nvme_register_csd(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init_ctrl_name   = csd_init_ctrl_str,
        .init             = csd_init,
        .exit             = csd_exit,
        .rw_check_req     = NULL,
        .start_ctrl       = NULL,
        .admin_cmd        = NULL,
        .admin_cmd_cqe    = csd_admin_cmd,
        .io_cmd           = csd_io_cmd,
        .get_log          = NULL,
    };

    return 0;
}
