#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/rcu.h"
#include <gmodule.h>
#include <sys/mman.h>

#include "csd.h"
#include "csd-cache.h"
#include "../bbssd/ftl.h"

#ifdef CONFIG_FEMU_CSD_UBPF
#include <ubpf.h>
#endif

typedef int64_t (*FemuCsdSharedLibFn)(FemuCsdArgs *args);

#define CSD_EXEC_DATA_MAX (1U << 20)
#define CSD_CACHE_ITEMS 128
#define CSD_CACHE_ENQUEUE_BATCH 16
#define CSD_CACHE_WORKERS 2
#define CSD_COPY_WORKERS 2
#define CSD_COPY_QUEUE (2 * FEMU_CSD_GRAPH_MAX_NODES)
#define CSD_COPY_MIN_BYTES (128 * KiB)
/* Dispatch credits reference the same bounded cache array. A full read
 * window can be assigned even while the FTL services other graph events. */
#define CSD_CACHE_LANE FEMU_CSD_GRAPH_MAX_NODES
#define CSD_CACHE_SAMPLE_NS 1000000
#define CSD_CACHE_CREDIT_SAMPLE_NS 50000

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
typedef struct FemuCsdWorker {
    QemuThread thread;
    FemuCsdState *csd;
    unsigned index;
} FemuCsdWorker;
typedef struct FemuCacheWorker {
    FemuCsdState *csd;
    QemuThread thread;
    QemuEvent wake;
    unsigned index, next_slot[2];
    unsigned submitted, completed, collected;
    struct FemuCacheDispatch *lane;
    QTAILQ_HEAD(, FemuCachePublication) publications;
} FemuCacheWorker;
typedef struct FemuCacheRing {
    struct femu_cache_rx rx;
    bool busy[FEMU_CACHE_RX_SLOTS];
    bool stopping, queued;
    unsigned ready_tail[FEMU_CACHE_READY_QUEUES];
    uint64_t credit_sample_ns;
    unsigned credits[2][CSD_CACHE_WORKERS]; /* scheduler-owned hints by slot size */
    unsigned active; /* scheduler-owned outstanding copies and publications */
    uint8_t *resident;
    struct femu_cache_stats stats;
} FemuCacheRing;
typedef struct FemuCachePublication {
    QTAILQ_ENTRY(FemuCachePublication) entry;
    FemuCacheRing *ring;
    struct FemuCacheProfile *profile;
    struct femu_cache_slot record;
    uint64_t due_ns, start_ns, end_ns;
    unsigned slot, worker;
    bool done, cancelled, failed;
} FemuCachePublication;
struct FemuCacheItem {
    struct {
        uint64_t token,cookie,offset,lba,bytes,load_ns;
        uint32_t afdm,nsid;
    } event;
    FemuCacheRing *ring;
    uint8_t *data;
    struct femu_graph_read_pool *pool;
    struct FemuCsdAfdm *source_afdm;
    struct FemuCacheProfile *summary_profile;
    uint16_t first_slot, slot_count;
    int slot;
    FemuCsdGraph *graph;
    unsigned session;
    unsigned node;
    bool internal_read;
    uint64_t dma_start_ns, dma_ns, copy_begin_ns, copy_end_ns, link_begin_ns, link_end_ns;
    uint64_t cu_ns, consumers, dma_bytes;
    uint64_t dma_lock_ns,update_lock_ns,publish_start_ns,publish_end_ns,segments,copy_cpu_ns;
    uint64_t slot_lock_ns, waiters, no_copy_done_ns;
    uint64_t dma_report_ns, dma_collect_ns; /* first worker pass, summary only */
    unsigned dma_active;
    bool published, dropped, data_done, wake_counted;
};
enum { CACHE_IDLE, CACHE_QUEUED, CACHE_ACTIVE, CACHE_CANCELLED, CACHE_PUBLISHING };
typedef struct FemuCacheDispatch {
    FemuCacheItem *item;
    FemuCacheItem result;
    uint64_t submit_ns, done_ns, busy_ns;
    bool summary, profiling, credit_miss;
} FemuCacheDispatch;

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

typedef struct FemuCacheWait {
    uint64_t count, sum_ns, union_ns, first_ns;
    unsigned active;
} FemuCacheWait;

typedef struct FemuFtlProfile {
    uint64_t register_ns, pin_drop_bytes, unprotected_drop_bytes, pinned_dma_bytes;
    uint64_t collect_ns, dispatch_ns, sweep_ns;
    uint64_t graph_lock_ns, io_lock_ns, cache_lock_ns;
} FemuFtlProfile;

typedef struct FemuCacheProfile {
    FemuCacheWait reuse, tail;
    uint64_t reuse_dma_ns, reuse_notify_ns, reuse_dispatch_ns, reuse_unsplit_ns;
    uint64_t bytes, copies, copy_ns, copy_cpu_ns, link_queue_ns, link_after_copy_ns;
    uint64_t wakes, wake_ns, wake_max_ns, ftl_lock_ns, sweep_lock_ns, dma_lock_ns, slots_lock_ns;
    uint64_t merged_reads, inline_publishes;
    uint64_t begin_ns, queue_full_start, worker_ns[CSD_CACHE_WORKERS];
    uint64_t slot_samples, slot_small_full, slot_large_full, slot_all_full;
    uint64_t slot_small_ready, slot_large_ready;
    uint64_t dispatch_count, admission_ns, lane_ns, lane_max_ns, collect_ns;
    uint64_t copied_lane_ns, credit_count, credit_lane_ns;
    uint64_t link_pending_count, collect_to_link_ns;
    uint64_t publish_count, publish_late_ns, publish_collect_ns;
    uint64_t reuse_queue_ns, reuse_worker_ns, reuse_harvest_ns;
    uint64_t reuse_post_harvest_link_ns, reuse_observe_ns;
    FemuFtlProfile ftl_start;
    uint64_t ftl_cpu_start;
    uint64_t read_issue_count, read_issue_ns, read_resume_count, read_resume_ns;
    uint64_t resume_before_event_ns, resume_register_ns;
    uint64_t resume_collect_ns, resume_dispatch_ns;
    bool enabled;
} FemuCacheProfile;

typedef struct FemuCsdSession {
    uint32_t id;
    unsigned graphs;
    FemuCacheRing *cache_ring;
    NvmeNamespace *ns;
    /* Enabled only by the existing SNIA_FEMU_PROFILE switch on graph requests. */
    bool profile;
    uint64_t completed_graphs, graph_ns, events, io_calls, io_ns, io_work_ns;
    uint64_t cu_calls, cu_ns, cu_queue_ns, late_ns, dispatch_ns;
    FemuCacheProfile cache_profile;
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
    struct femu_graph_read_pool *pool;
} FemuCsdReadCompletion;
typedef struct FemuCsdGraphNode {
    struct femu_graph_node wire;
    FemuCsdJob *job;
    FemuCsdAfdm *afdm;
    unsigned state, range;
    uint64_t deadline, result, transferred, range_blocks;
    uint64_t cache_lba, cache_bytes, cache_offset;
    struct femu_graph_read_pool *cache_pool;
    FemuCsdAfdm *cache_afdm;
    uint64_t cache_wait_begin, cache_wait_ready, cache_wait_notified;
    uint64_t cache_wait_started, cache_wait_reported, cache_wait_collected;
    uint64_t read_copy_ready_ns;
    bool cache_candidate;
    uint64_t io_action, io_token, io_bytes;
    FemuCsdSpill *spill;
    struct femu_graph_read *reads;
    FemuCsdReadCompletion *read_completions;
    unsigned read_completed;
    unsigned copies_pending;
    bool serial_reads;
    uint16_t status;
    FemuCsdGraph *graph;
    uint64_t dependents;
    unsigned remaining;
    size_t pos;
} FemuCsdGraphNode;

typedef struct FemuCsdReadCopy {
    FemuCsdGraphNode *node;
    uint8_t *dest;
    uint64_t offset, bytes;
} FemuCsdReadCopy;

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
    DECLARE_BITMAP(cache_items, CSD_CACHE_ITEMS);
    uint64_t cu_done_ns;
    uint64_t cache_tail_begin;
    uint16_t status;
    FemuCsdGraphNode nodes[FEMU_CSD_GRAPH_MAX_NODES];
    QTAILQ_ENTRY(FemuCsdGraph) entry, ready_entry;
};

struct FemuCsdState {
    FemuCtrl *n;
    CsdCtrlParams params;
    uint64_t fdm_capacity;
    uint64_t fdm_used;
    uint8_t *device_memory, *fdm_base;
    size_t device_memory_bytes;
    GModule *workmem_module;
    void (*workmem_detach)(void);
    GList *fdm_ranges; /* In address order, including AFDMs held by in-flight I/O. */
    uint64_t prog_used;         /* bytes held by loaded programs */
    uint32_t next_afdm_id;
    uint32_t next_group_id;
    uint32_t next_rsid;
    GHashTable *afdms;
    GHashTable *programs;
    GHashTable *groups;
    GHashTable *mrs;
    QemuMutex lock;
    QemuThread copy_workers[CSD_COPY_WORKERS];
    QemuCond copy_cond;
    FemuCsdReadCopy copies[CSD_COPY_QUEUE];
    unsigned copy_head, copy_count;
    FemuCacheWorker cache_workers[CSD_CACHE_WORKERS];
    FemuCacheDispatch cache_lanes[CSD_CACHE_WORKERS][CSD_CACHE_LANE];
    QemuEvent cache_completion;
    bool cache_draining, cache_cancel_publications;
    unsigned cache_next_worker, cache_next_item;
    uint64_t cache_deadline;
    uint64_t cache_next;
    unsigned cache_dropped;
    unsigned cache_work; /* retained LOAD blocks */
    unsigned cache_dirty; /* retirement scan only after a state change */
    unsigned cache_refill; /* a worker lane is running low */
    unsigned cache_urgent; /* a completed DMA can unblock a graph */
    bool cache_sweep; /* stopped or traced entries need deferred retirement */
    unsigned cache_profile_sessions;
    FemuFtlProfile ftl_profile; /* FTL updates under the scheduler lock */
    clockid_t ftl_cpu_clock;
    bool ftl_cpu_clock_valid;
    uint64_t cache_full_begin, cache_full_ns; /* scheduler lock */
    uint64_t cache_next_sample; /* FTL thread only */
    DECLARE_BITMAP(cache_runnable, CSD_CACHE_ITEMS); /* scheduler lock */
    DECLARE_BITMAP(cache_publishing, CSD_CACHE_ITEMS); /* scheduler lock */
    FemuCacheItem cache[CSD_CACHE_ITEMS];
    FemuCachePublication cache_publications[CSD_CACHE_ITEMS];
    uint64_t *cu_next_avail;    /* when each compute unit is free again */
    FemuCsdWorker *workers;     /* one per compute unit */
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
static void csd_cache_finish(FemuCsdState *, FemuCacheItem *);
static void csd_cache_log(const FemuCacheItem *, uint64_t);
static void csd_cache_wait_end(FemuCacheWait *, uint64_t, uint64_t);
static void csd_cache_profile_reuse_end(FemuCsdGraphNode *, uint64_t);
static void *csd_worker(void *opaque);
static void *csd_copy_worker(void *opaque);
static void *csd_cache_worker(void *opaque);
static void csd_cache_wait(FemuCsdState *csd, FemuCacheRing *ring);
static void csd_cache_dispatch(FemuCsdState *csd);
static void csd_cache_collect(FemuCsdState *csd, bool ftl);
static void csd_cache_refill(FemuCsdState *csd);
static void csd_cache_remove_work(FemuCsdState *csd);
static uint64_t csd_cache_full_time(FemuCsdState *csd, uint64_t now);
static void csd_job_free_locked(FemuCsdJob *job);
static void csd_graph_destroy_locked(FemuCsdState *csd, FemuCsdGraph *g);

static uint64_t csd_ftl_stamp(FemuCsdState *csd)
{
    return qatomic_read(&csd->cache_profile_sessions) ?
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
}

static void csd_ftl_lock(FemuCsdState *csd, uint64_t *wait_ns)
{
    uint64_t start = csd_ftl_stamp(csd);
    qemu_mutex_lock(&csd->lock);
    if (start) {
        *wait_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - start;
    }
}

static uint64_t csd_ftl_cpu_ns(FemuCsdState *csd)
{
    struct timespec cpu;
    if (!csd->ftl_cpu_clock_valid || clock_gettime(csd->ftl_cpu_clock, &cpu)) {
        return 0;
    }
    return cpu.tv_sec * UINT64_C(1000000000) + cpu.tv_nsec;
}
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
    if (afdm->csd->fdm_base) {
        afdm->csd->fdm_ranges = g_list_remove(afdm->csd->fdm_ranges, afdm);
    } else {
        g_free(afdm->data);
    }
    g_free(afdm);
}

static bool csd_device_memory_init(FemuCsdState *csd, const char *program_dir,
                                   Error **errp)
{
    const char *enabled = getenv("FEMU_CSD_HUGEPAGES");
    if (!enabled || strcmp(enabled, "1")) {
        return true;
    }
    const size_t huge = 2 * MiB;
    const char *value = getenv("CSD_WORKMEM_SIZE_MB");
    uint64_t mib;
    if (!program_dir || !value || qemu_strtou64(value, NULL, 10, &mib) ||
        !mib || mib > (SIZE_MAX - csd->fdm_capacity) / MiB ||
        (mib * MiB + csd->fdm_capacity) % huge) {
        error_setg(errp, "CSD HugeTLB needs a program directory and a valid, 2 MiB aligned combined memory size");
        return false;
    }
    size_t workmem = mib * MiB;
    size_t bytes = workmem + csd->fdm_capacity;
    g_autofree char *path = g_build_filename(program_dir, "libcs-workmem.so", NULL);
    GModule *module = g_module_open(path, G_MODULE_BIND_LOCAL);
    int (*attach)(void *, size_t);
    void (*detach)(void);
    if (!module || !g_module_symbol(module, "cs_work_attach", (gpointer *)&attach) ||
        !g_module_symbol(module, "cs_work_detach", (gpointer *)&detach)) {
        error_setg(errp, "Cannot bind CSD workmem allocator at %s: %s", path, g_module_error());
        if (module) {
            g_module_close(module);
        }
        return false;
    }
    /* Linux MAP_HUGE_2MB uses the base-2 page size in flag bits 26..31. */
    uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (21 << 26), -1, 0);
    if (memory == MAP_FAILED || madvise(memory, bytes, MADV_POPULATE_WRITE)) {
        error_setg_errno(errp, errno, "Cannot prefault CSD memory; need %zu free 2 MiB HugeTLB pages on the device NUMA nodes",
                         bytes / huge);
        if (memory != MAP_FAILED) {
            munmap(memory, bytes);
        }
        g_module_close(module);
        return false;
    }
    if (attach(memory, workmem)) {
        error_setg_errno(errp, errno, "Cannot attach CSD workmem");
        munmap(memory, bytes);
        g_module_close(module);
        return false;
    }
    csd->device_memory = memory;
    csd->device_memory_bytes = bytes;
    csd->fdm_base = memory + workmem;
    csd->workmem_module = module;
    csd->workmem_detach = detach;
    info_report("CSD device memory: %zu MiB workmem + %" PRIu64 " MiB FDM, %zu prefaulted HugeTLB pages",
                workmem / MiB, csd->fdm_capacity / MiB, bytes / huge);
    return true;
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

    if (!n->ns_mgmt) {
        csd_init_ctrl_str(n, ns);
    }

    csd = g_new0(FemuCsdState, 1);
    csd->fdm_capacity = n->csd_params.fdm_size_mb * MiB;
    if (!csd_device_memory_init(csd, n->csd_params.program_dir, errp)) {
        g_free(csd);
        return;
    }

    ssd = ns->ssd = g_malloc0(sizeof(*ssd));
    if (!n->ssd) {
        n->ssd = ssd;
    }
    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    ssd_init(n, ns);

    csd->params = n->csd_params;
    csd->cache_next = 1;
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
    csd->n = n;
    qemu_cond_init(&csd->job_cond);
    qemu_cond_init(&csd->copy_cond);
    qemu_event_init(&csd->cache_completion, false);
    csd->cache_deadline = UINT64_MAX;
    QTAILQ_INIT(&csd->pending);
    QTAILQ_INIT(&csd->running);
    QTAILQ_INIT(&csd->graphs);
    QTAILQ_INIT(&csd->graph_ready);
    csd->graph_events = pqueue_init(64 * FEMU_CSD_GRAPH_MAX_NODES + 1,
        graph_cmp, graph_pri, graph_set_pri, graph_pos, graph_set_pos);
    g_assert(csd->graph_events);
    csd->graph_deadline = UINT64_MAX;
    QSLIST_INIT(&csd->dead);
    csd->workers = g_new0(FemuCsdWorker, csd->params.nr_cu);
    for (i = 0; i < csd->params.nr_cu; i++) {
        FemuCsdWorker *worker = &csd->workers[i];
        char name[32];
        worker->csd = csd;
        worker->index = i;
        snprintf(name, sizeof(name), "femu-csd-cu%u", i);
        qemu_thread_create(&worker->thread, name, csd_worker, worker,
                           QEMU_THREAD_JOINABLE);
    }
    n->csd_ctrl_state = csd;
    for (i = 0; i < ARRAY_SIZE(csd->copy_workers); i++) {
        qemu_thread_create(&csd->copy_workers[i], "femu-nvm-copy",
                           csd_copy_worker, csd, QEMU_THREAD_JOINABLE);
    }
    for (i = 0; i < ARRAY_SIZE(csd->cache_workers); i++) {
        FemuCacheWorker *worker = &csd->cache_workers[i];
        worker->csd = csd;
        worker->index = i;
        worker->lane = csd->cache_lanes[i];
        worker->next_slot[0] = worker->next_slot[1] = i;
        QTAILQ_INIT(&worker->publications);
        qemu_event_init(&worker->wake, false);
        qemu_thread_create(&worker->thread, "femu-cache-dma", csd_cache_worker, worker,
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

    /* Join compute and copy workers before releasing device state. */
    if (csd) {
        qemu_mutex_lock(&csd->lock);
        csd_cache_wait(csd, NULL);
        qatomic_set(&csd->stopping, true);
        qemu_cond_broadcast(&csd->job_cond);
        qemu_cond_broadcast(&csd->copy_cond);
        qemu_mutex_unlock(&csd->lock);
        for (i = 0; i < ARRAY_SIZE(csd->copy_workers); i++) {
            qemu_thread_join(&csd->copy_workers[i]);
        }
        for (i = 0; i < ARRAY_SIZE(csd->cache_workers); i++) {
            qemu_event_set(&csd->cache_workers[i].wake);
            qemu_thread_join(&csd->cache_workers[i].thread);
            qemu_event_destroy(&csd->cache_workers[i].wake);
        }
        for (i = 0; i < csd->params.nr_cu; i++) {
            qemu_thread_join(&csd->workers[i].thread);
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
    if (csd->workmem_module) {
        csd->workmem_detach();
        g_module_close(csd->workmem_module);
        munmap(csd->device_memory, csd->device_memory_bytes);
    }
    qemu_mutex_destroy(&csd->lock);
    qemu_cond_destroy(&csd->job_cond);
    qemu_cond_destroy(&csd->copy_cond);
    qemu_event_destroy(&csd->cache_completion);
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
        /* Native CSFs without explicit timing finish when the real call
         * returns. CU threads/cgroups already bound their execution: do not
         * charge throttled wall time to a second virtual CU timeline. The
         * original per-program scale remains part of the native load ABI. */
        runtime = MAX(host_ns, 0);
        if (job->program->runtime_scale) {
            runtime = runtime * job->program->runtime_scale / 10;
            req->expire_time = csd_hold_cu_locked(csd, job->arrival, runtime);
        } else {
            req->expire_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
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
    FemuCsdWorker *worker = opaque;
    FemuCsdState *csd = worker->csd;
    FemuCtrl *n = csd->n;
    FemuCsdProgram *dead;
    FemuCsdJob *job;
    struct rte_ring *ring;
    int64_t result, host_ns;
    uint64_t run_begin=0,run_end=0;
    struct timespec cpu_begin,cpu_end;
    uint16_t status;

    /* The launcher creates one CPU-controller group per CU. Only this
     * thread moves: vCPUs, FTL, NVMe pollers and DMA remain in their groups. */
    const char *cgroup = getenv("FEMU_CU_CGROUP");
    if (cgroup && *cgroup) {
        g_autofree char *path = g_strdup_printf("%s/cu%u/tasks", cgroup,
                                                worker->index);
        int fd = open(path, O_WRONLY | O_CLOEXEC);
        char tid[32];
        int bytes = snprintf(tid, sizeof(tid), "%d\n", qemu_get_thread_id());
        if (fd < 0 || write(fd, tid, bytes) != bytes) {
            error_report("CSD CU %u cannot join %s: %s", worker->index,
                         path, strerror(errno));
            exit(EXIT_FAILURE);
        }
        close(fd);
        femu_log("CSD CU %u tid=%d cgroup=%s\n", worker->index,
                 qemu_get_thread_id(), path);
    }

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

    GList *next = csd->fdm_ranges;
    uint64_t offset = 0;
    if (csd->fdm_base) {
        for (; next; next = next->next) {
            const FemuCsdAfdm *live = next->data;
            uint64_t begin = live->data - csd->fdm_base;
            if (offset <= begin && size <= begin - offset) {
                break;
            }
            offset = QEMU_ALIGN_UP(begin + live->size, 16);
        }
        if (offset > csd->fdm_capacity || size > csd->fdm_capacity - offset) {
            qemu_mutex_unlock(&csd->lock);
            return NVME_CAP_EXCEEDED | NVME_DNR;
        }
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
    if (csd->fdm_base) {
        afdm->data = csd->fdm_base + offset;
        memset(afdm->data, 0, size);
        csd->fdm_ranges = g_list_insert_before(csd->fdm_ranges, next, afdm);
    } else {
        afdm->data = g_malloc0(size);
    }

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
                                   uint64_t *lat, FemuCsdAfdm *held,
                                   FemuCsdIoTiming *timing, FemuCsdReadCopy *deferred)
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
        if (!status && deferred) {
            /* The graph holds a reference to the destination until completion. */
            *deferred = (FemuCsdReadCopy){
                .dest = afdm->data + offset, .offset = nvm_offset, .bytes = size,
            };
        } else if (!status) {
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
        if (held) {
            csd_ftl_lock(csd, &csd->ftl_profile.io_lock_ns);
        } else {
            qemu_mutex_lock(&csd->lock);
        }
        if(timing) timing->csd_lock_ns+=qemu_clock_get_ns(QEMU_CLOCK_REALTIME)-stamp;
        csd_afdm_unref(afdm);
    }
    if (status) {
        return status;
    }

    req->cqe.n.result = size;
    return NVME_SUCCESS;
}

static void *csd_copy_worker(void *opaque)
{
    FemuCsdState *csd = opaque;
    SsdDramBackend *backend = csd->n->mbe;

    femu_bind_device_thread();
    qemu_mutex_lock(&csd->lock);
    while (true) {
        while (!csd->copy_count && !qatomic_read(&csd->stopping)) {
            qemu_cond_wait(&csd->copy_cond, &csd->lock);
        }
        if (!csd->copy_count) {
            break;
        }
        FemuCsdReadCopy copy = csd->copies[csd->copy_head];
        FemuCsdGraphNode *v = copy.node;
        bool profile = v->graph->flags & FEMU_CSD_GRAPH_PROFILE;
        csd->copy_head = (csd->copy_head + 1) % CSD_COPY_QUEUE;
        csd->copy_count--;
        qemu_mutex_unlock(&csd->lock);

        uint64_t begin = profile ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
        backend_lock_range(backend, copy.offset, copy.bytes);
        uint64_t locked = profile ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
        memcpy(copy.dest, (uint8_t *)backend->logical_space + copy.offset, copy.bytes);
        backend_unlock_range(backend, copy.offset, copy.bytes);
        if (profile) {
            fprintf(stderr, "CSD_IO_COPY_SPAN session=%u cookie=%" PRIu64
                    " node=%u bytes=%" PRIu64 " begin=%" PRIu64
                    " locked=%" PRIu64 " end=%" PRIu64 "\n",
                    v->graph->session->id, v->graph->cookie,
                    (unsigned)(v - v->graph->nodes), copy.bytes, begin, locked,
                    (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
        }

        qemu_mutex_lock(&csd->lock);
        if (!--v->copies_pending) {
            if (v->graph->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY) {
                v->read_copy_ready_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            }
            if (v->state == GRAPH_IO_DONE) {
                csd_graph_event_locked(csd, v);
            }
        }
    }
    qemu_mutex_unlock(&csd->lock);
    return NULL;
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
    if (g->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        FemuCacheProfile *p = &g->session->cache_profile;
        if (g->cache_tail_begin) {
            csd_cache_wait_end(&p->tail, g->cache_tail_begin, now);
        }
        for (unsigned i = 0; i < g->count; i++) {
            if (g->nodes[i].cache_wait_begin) {
                csd_cache_profile_reuse_end(&g->nodes[i], now);
            }
        }
    }
    for (unsigned i=0;i<ARRAY_SIZE(csd->cache);i++)
        if (csd->cache[i].event.token && csd->cache[i].graph==g) {
            clear_bit(i, csd->cache_runnable);
            csd->cache[i].event.token=0; csd_cache_remove_work(csd);
        }
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
        if (!csd->ftl_cpu_clock_valid) {
            csd->ftl_cpu_clock_valid =
                !pthread_getcpuclockid(pthread_self(), &csd->ftl_cpu_clock);
        }
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
    uint64_t profile_end = s->cache_profile.enabled ?
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
    uint64_t profile_cpu_end = profile_end ? csd_ftl_cpu_ns(csd) : 0;
    csd_cache_ring_stop(csd,s);
    if (s->cache_profile.enabled) {
        const FemuCacheProfile *p = &s->cache_profile;
        struct FemuLinkProfile link;
        pthread_spin_lock(&csd->n->pcie_lock);
        link = csd->n->pcie_profile;
        csd->n->pcie_profile_users--;
        pthread_spin_unlock(&csd->n->pcie_lock);
        fprintf(stderr, "PCIE_SUMMARY session=%u"
                " tx_requests=%" PRIu64 " tx_affected=%" PRIu64
                " tx_queue_ns=%" PRIu64 " tx_exposed_ns=%" PRIu64
                " tx_exposed_max_ns=%" PRIu64 " afdm_requests=%" PRIu64
                " afdm_affected=%" PRIu64 " afdm_exposed_ns=%" PRIu64 "\n",
                s->id, link.requests, link.affected, link.delay_ns,
                link.exposed_ns, link.max_exposed_ns, link.afdm_requests,
                link.afdm_affected, link.afdm_exposed_ns);
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        uint64_t queue_full = csd_cache_full_time(csd, now) - p->queue_full_start;
        fprintf(stderr, "CACHE_SUMMARY session=%u"
                " reuse_count=%" PRIu64 " reuse_ns=%" PRIu64 " reuse_union_ns=%" PRIu64
                " reuse_dma_ns=%" PRIu64 " reuse_notify_ns=%" PRIu64
                " reuse_dispatch_ns=%" PRIu64 " reuse_unsplit_ns=%" PRIu64
                " tail_count=%" PRIu64 " tail_ns=%" PRIu64 " tail_union_ns=%" PRIu64
                " wakes=%" PRIu64 " ready_to_ftl_ns=%" PRIu64 " ready_to_ftl_max_ns=%" PRIu64
                " copies=%" PRIu64 " bytes=%" PRIu64 " copy_ns=%" PRIu64 " copy_cpu_ns=%" PRIu64
                " link_queue_ns=%" PRIu64 " link_after_copy_ns=%" PRIu64
                " ftl_cache_lock_ns=%" PRIu64 " ftl_sweep_lock_ns=%" PRIu64
                " dma_cache_lock_ns=%" PRIu64 " slot_lock_ns=%" PRIu64
                " merged_reads=%" PRIu64 " inline_publishes=%" PRIu64
                " window_ns=%" PRIu64 " queue_full_ns=%" PRIu64
                " worker0_busy_ns=%" PRIu64 " worker1_busy_ns=%" PRIu64
                " slot_samples=%" PRIu64 " slot_small_full=%" PRIu64
                " slot_large_full=%" PRIu64 " slot_all_full=%" PRIu64
                " slot_small_ready=%" PRIu64 " slot_large_ready=%" PRIu64
                " dispatch_count=%" PRIu64 " admission_ns=%" PRIu64
                " lane_ns=%" PRIu64 " lane_max_ns=%" PRIu64 " collect_ns=%" PRIu64
                " copied_lane_ns=%" PRIu64 " credit_count=%" PRIu64 " credit_lane_ns=%" PRIu64
                " link_pending_count=%" PRIu64 " collect_to_link_ns=%" PRIu64
                " publish_count=%" PRIu64 " publish_late_ns=%" PRIu64
                " publish_collect_ns=%" PRIu64
                " reuse_queue_ns=%" PRIu64 " reuse_worker_ns=%" PRIu64 " reuse_harvest_ns=%" PRIu64
                " reuse_post_harvest_link_ns=%" PRIu64 " reuse_observe_ns=%" PRIu64
                " pcie_mbps=%u\n",
                s->id, p->reuse.count, p->reuse.sum_ns, p->reuse.union_ns,
                p->reuse_dma_ns, p->reuse_notify_ns, p->reuse_dispatch_ns, p->reuse_unsplit_ns,
                p->tail.count, p->tail.sum_ns, p->tail.union_ns,
                p->wakes, p->wake_ns, p->wake_max_ns,
                p->copies, p->bytes, p->copy_ns, p->copy_cpu_ns,
                p->link_queue_ns, p->link_after_copy_ns,
                p->ftl_lock_ns, p->sweep_lock_ns, p->dma_lock_ns, p->slots_lock_ns,
                p->merged_reads, p->inline_publishes,
                now - p->begin_ns, queue_full, p->worker_ns[0], p->worker_ns[1],
                p->slot_samples, p->slot_small_full, p->slot_large_full,
                p->slot_all_full, p->slot_small_ready, p->slot_large_ready,
                p->dispatch_count, p->admission_ns, p->lane_ns, p->lane_max_ns, p->collect_ns,
                p->copied_lane_ns, p->credit_count, p->credit_lane_ns,
                p->link_pending_count, p->collect_to_link_ns,
                p->publish_count, p->publish_late_ns, p->publish_collect_ns,
                p->reuse_queue_ns, p->reuse_worker_ns, p->reuse_harvest_ns,
                p->reuse_post_harvest_link_ns, p->reuse_observe_ns,
                csd->n->pcie_bandwidth_mbps);
        const FemuFtlProfile *start = &p->ftl_start, *end = &csd->ftl_profile;
        fprintf(stderr, "FTL_SUMMARY session=%u window_ns=%" PRIu64
                " cpu_ns=%" PRIu64 " register_ns=%" PRIu64
                " pin_drop_bytes=%" PRIu64 " unprotected_drop_bytes=%" PRIu64
                " pinned_dma_bytes=%" PRIu64 " collect_ns=%" PRIu64
                " dispatch_ns=%" PRIu64 " sweep_ns=%" PRIu64
                " graph_lock_ns=%" PRIu64 " io_lock_ns=%" PRIu64
                " cache_lock_ns=%" PRIu64 " read_issue_count=%" PRIu64
                " read_issue_ns=%" PRIu64 " read_resume_count=%" PRIu64
                " read_resume_ns=%" PRIu64 " resume_before_event_ns=%" PRIu64
                " resume_register_ns=%" PRIu64 " resume_collect_ns=%" PRIu64
                " resume_dispatch_ns=%" PRIu64 "\n",
                s->id, profile_end - p->begin_ns,
                p->ftl_cpu_start && profile_cpu_end ? profile_cpu_end - p->ftl_cpu_start : 0,
                end->register_ns - start->register_ns,
                end->pin_drop_bytes - start->pin_drop_bytes,
                end->unprotected_drop_bytes - start->unprotected_drop_bytes,
                end->pinned_dma_bytes - start->pinned_dma_bytes,
                end->collect_ns - start->collect_ns,
                end->dispatch_ns - start->dispatch_ns, end->sweep_ns - start->sweep_ns,
                end->graph_lock_ns - start->graph_lock_ns, end->io_lock_ns - start->io_lock_ns,
                end->cache_lock_ns - start->cache_lock_ns, p->read_issue_count,
                p->read_issue_ns, p->read_resume_count, p->read_resume_ns,
                p->resume_before_event_ns, p->resume_register_ns,
                p->resume_collect_ns, p->resume_dispatch_ns);
        qatomic_dec(&csd->cache_profile_sessions);
    }
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
static bool csd_cache_resident(FemuCacheRing *ring,uint64_t lba,uint64_t bytes,unsigned lbads,bool refresh,FemuCtrl *n)
{
    uint64_t first=(lba<<lbads)/FEMU_CACHE_PAGE_BYTES,count=bytes/FEMU_CACHE_PAGE_BYTES;
    uint64_t at=first/8,end=(first+count+7)/8;
    uint8_t bits[FEMU_CACHE_RX_PAGES/8+1];
    if(!count || end>ring->rx.resident_bytes || end-at>sizeof(bits)) return false;
    if(refresh && !femu_dma_read(n,ring->rx.resident_dma+at,bits,end-at))
        for(uint64_t i=at;i<end;i++) qatomic_fetch_or(&ring->resident[i],bits[i-at]);
    for(uint64_t i=at;i<end;i++) {
        uint8_t mask=255;
        if(i==at) mask&=255U<<(first%8);
        if(i+1==end && (first+count)%8) mask&=(1U<<((first+count)%8))-1;
        if((qatomic_read(&ring->resident[i])&mask)!=mask) return false;
    }
    return true;
}
static uint16_t csd_cache_prepare(FemuCsdState *csd,FemuCsdGraphNode *v,
                                  uint64_t offset,uint64_t bytes,uint64_t lba)
{
    v->cache_candidate=false;
    FemuCacheRing *ring=v->graph->session->cache_ring;
    if((v->wire.operation!=FEMU_GRAPH_LOAD && !(v->job && v->io_action==4)) ||
       !(v->graph->flags&FEMU_CSD_GRAPH_CACHE) || !ring) return NVME_SUCCESS;
    if (v->job && !v->cache_pool) {
        if (v->graph->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY) {
            csd->ftl_profile.unprotected_drop_bytes += bytes;
        }
        return NVME_SUCCESS;
    }
    qatomic_fetch_add(&ring->stats.candidate_bytes,bytes);
    if((offset|bytes)%FEMU_CACHE_PAGE_BYTES) {
        qatomic_fetch_add(&ring->stats.error_dropped_bytes,bytes);
        qatomic_inc(&csd->cache_dropped); return NVME_SUCCESS;
    }
    if(csd_cache_resident(ring,lba,bytes,v->graph->session->ns->lbaf.lbads,false,csd->n)) {
        qatomic_fetch_add(&ring->stats.resident_bytes,bytes); return NVME_SUCCESS;
    }
    /* Credit availability is checked by the DMA workers, outside csd->lock. */
    v->cache_candidate=true;
    return NVME_SUCCESS;
}

static void csd_cache_wake_graph(FemuCsdState *csd, FemuCacheItem *item)
{
    FemuCsdGraph *g = item->graph;
    if(!g->active) { csd_graph_ready_locked(csd,g); return; }
    bool waiting = false;
    for (unsigned i = 0; i < g->count; i++) {
        if (g->nodes[i].state == GRAPH_CACHE_WAIT) {
            waiting = true;
            if (item->waiters & (UINT64_C(1) << i)) {
                csd_graph_ready_locked(csd, g);
                QTAILQ_REMOVE(&csd->graph_ready, g, ready_entry);
                QTAILQ_INSERT_HEAD(&csd->graph_ready, g, ready_entry);
                return;
            }
        }
    }
    if (waiting) csd_graph_ready_locked(csd, g);
}

static void csd_cache_signal_done(FemuCsdState *csd, const FemuCacheItem *item)
{
    qatomic_set(&csd->cache_dirty, 1);
    if (qatomic_read(&item->waiters)) {
        qatomic_set(&csd->cache_urgent, 1);
    }
}

/* Keep the full interval across dispatch and retirement under the scheduler lock. */
static uint64_t csd_cache_full_time(FemuCsdState *csd, uint64_t now)
{
    return csd->cache_full_ns +
           (csd->cache_full_begin ? now - csd->cache_full_begin : 0);
}

static void csd_cache_remove_work(FemuCsdState *csd)
{
    if (csd->cache_full_begin) {
        csd->cache_full_ns = csd_cache_full_time(csd, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
        csd->cache_full_begin = 0;
    }
    qatomic_dec(&csd->cache_work);
}

static void csd_cache_wait_start(FemuCacheWait *wait, uint64_t now)
{
    wait->count++;
    if (!wait->active++) {
        wait->first_ns = now;
    }
}

static void csd_cache_wait_end(FemuCacheWait *wait, uint64_t begin, uint64_t now)
{
    wait->sum_ns += now - begin;
    if (!--wait->active) {
        wait->union_ns += now - wait->first_ns;
    }
}

/* Measure when the FTL first sees reusable data, independently of the
 * graph's subsequent dispatch. Count each transfer once. */
static void csd_cache_profile_wake(FemuCacheItem *item, uint64_t now)
{
    if (!(item->graph->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY) ||
        (!item->data_done && !item->published && !item->dropped) || item->wake_counted) {
        return;
    }
    uint64_t ready = item->dma_bytes ? MAX(item->copy_end_ns, item->link_end_ns) :
        item->no_copy_done_ns;
    FemuCacheProfile *p = &item->graph->session->cache_profile;
    if (item->dma_bytes) {
        uint64_t delay = now > ready ? now - ready : 0;
        p->wakes++;
        p->wake_ns += delay;
        p->wake_max_ns = MAX(p->wake_max_ns, delay);
    }
    uint64_t waiters = item->waiters;
    while (ready && waiters) {
        unsigned index = ctz64(waiters);
        waiters &= waiters - 1;
        FemuCsdGraphNode *node = &item->graph->nodes[index];
        if (node->cache_wait_begin && ready >= node->cache_wait_ready) {
            node->cache_wait_ready = ready;
            node->cache_wait_notified = now;
            node->cache_wait_started = item->dma_start_ns;
            node->cache_wait_reported = item->dma_report_ns;
            node->cache_wait_collected = item->dma_collect_ns;
        }
    }
    item->wake_counted = true;
}

static void csd_cache_profile_reuse_end(FemuCsdGraphNode *node, uint64_t now)
{
    FemuCacheProfile *p = &node->graph->session->cache_profile;
    uint64_t begin = node->cache_wait_begin;
    csd_cache_wait_end(&p->reuse, begin, now);
    if (node->cache_wait_ready) {
        uint64_t ready = MIN(now, MAX(begin, node->cache_wait_ready));
        uint64_t notified = MIN(now, MAX(ready, node->cache_wait_notified));
        p->reuse_dma_ns += ready - begin;
        p->reuse_notify_ns += notified - ready;
        p->reuse_dispatch_ns += now - notified;
        /* The modeled link may end before or after the worker reports.
         * Clip each milestone to this node's actual blocking interval. */
        uint64_t started = MIN(notified, MAX(begin, node->cache_wait_started));
        uint64_t reported = MIN(notified, MAX(started, node->cache_wait_reported));
        uint64_t collected = MIN(notified, MAX(reported, node->cache_wait_collected));
        uint64_t safe = MAX(collected, ready);
        p->reuse_queue_ns += started - begin;
        p->reuse_worker_ns += reported - started;
        p->reuse_harvest_ns += collected - reported;
        p->reuse_post_harvest_link_ns += safe - collected;
        p->reuse_observe_ns += notified - safe;
    } else {
        p->reuse_unsplit_ns += now - begin;
    }
    node->cache_wait_ready = node->cache_wait_notified = 0;
}

static bool csd_cache_pin(struct femu_graph_read_pool *pool, uint8_t *data,
                          uint64_t bytes, unsigned *first, unsigned *count)
{
    uint32_t width = le32_to_cpu(pool->slot_bytes);
    uint32_t slots = le32_to_cpu(pool->slots);
    uint64_t at = (uintptr_t)data - ((uintptr_t)pool - (uint64_t)slots * width);
    *first = at / width;
    *count = DIV_ROUND_UP(at + bytes, width) - *first;
    unsigned additional = 0;
    for (unsigned i = *first; i < *first + *count; i++) {
        additional += !qatomic_read(&pool->readers[i]);
    }
    if (pool->pinned + additional > le32_to_cpu(pool->pin_limit)) {
        return false;
    }
    for (unsigned i = *first; i < *first + *count; i++) {
        qatomic_inc(&pool->readers[i]);
    }
    pool->pinned += additional;
    return true;
}

static bool csd_cache_publish_locked(FemuCsdState *csd, FemuCsdGraphNode *v, uint64_t now,
                                     FemuCacheItem *retired, unsigned *logged, bool profile)
{
    FemuCacheItem *item = NULL, *completed = NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(csd->cache); i++) {
        FemuCacheItem *entry = &csd->cache[i];
        if (!entry->event.token) {
            item = entry;
            break;
        }
        if (!completed && qatomic_read(&entry->dma_active) == CACHE_IDLE &&
            (entry->published || entry->dropped)) {
            completed = entry;
        }
    }
    /* LOAD completions can arrive before the FTL end-of-sweep cleanup. */
    if (!item && completed) {
        item = completed;
        if (item->graph && (item->graph->flags & FEMU_CSD_GRAPH_PROFILE)) {
            retired[(*logged)++] = *item;
        }
        csd_cache_finish(csd, item);
    }
    uint8_t *source = v->cache_afdm->data + v->cache_offset;
    struct femu_graph_read_pool *pool = v->cache_pool;
    unsigned first_slot = 0, slot_count = 0;
    bool pin_full = item && pool &&
        !csd_cache_pin(pool, source, v->cache_bytes, &first_slot, &slot_count);
    if (!item || pin_full) {
        if (profile && pin_full) {
            csd->ftl_profile.pin_drop_bytes += v->cache_bytes;
        }
        qatomic_fetch_add(&v->graph->session->cache_ring->stats.queue_dropped_bytes,v->cache_bytes);
        qatomic_inc(&csd->cache_dropped);
        return false;
    }
    *item = (FemuCacheItem){ .graph=pool ? NULL : v->graph,
        .node=v-v->graph->nodes, .internal_read=v->job!=NULL,
        .session=v->graph->session->id,
        .ring=v->graph->session->cache_ring,.slot=-1,
        .data=source, .pool=pool, .first_slot=first_slot, .slot_count=slot_count,
        .source_afdm=pool ? v->cache_afdm : NULL,
        .summary_profile=profile ? &v->graph->session->cache_profile : NULL,
        .event={.token=csd->cache_next++, .cookie=v->graph->cookie,
        .offset=v->cache_offset,
        .lba=v->cache_lba,
        .bytes=v->cache_bytes, .load_ns=now, .afdm=v->cache_afdm->id,
        .nsid=v->graph->session->ns->id} };
    /* Consumer timestamps are only used by the per-block trace. */
    if (!pool && (v->graph->flags & FEMU_CSD_GRAPH_PROFILE)) {
        uintptr_t first=(uintptr_t)source;
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
    }
    if (pool) {
        /* The queued copy owns AFDM independently of the compute graph. */
        v->cache_afdm->refs++;
    } else {
        set_bit(item - csd->cache, v->graph->cache_items);
        v->graph->cache_pending++;
    }
    set_bit(item - csd->cache, csd->cache_runnable);
    if (qatomic_fetch_inc(&csd->cache_work) == CSD_CACHE_ITEMS - 1) {
        csd->cache_full_begin = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }
    return true;
}

static void csd_cache_publish_reads(FemuCsdState *csd, FemuCsdGraphNode *v, uint64_t now)
{
    unsigned pending = v->read_completed + v->cache_candidate;
    FemuCsdReadCompletion direct = {v->cache_afdm, v->cache_lba,
                                    v->cache_offset, v->cache_bytes, v->cache_pool};
    v->cache_candidate = false;
    if (!pending || !v->graph->session->cache_ring) {
        v->read_completed = 0;
        return;
    }
    bool profile = v->graph->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY;
    uint64_t begin = profile ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
    /* Pack adjacent NAND reads into one DMA item if they fit in the same
     * Guest slot class and are contiguous in both FDM and the namespace. */
    unsigned merged = 0;
    unsigned lbads = v->graph->session->ns->lbaf.lbads;
    for (unsigned i = 0; i < v->read_completed; i++) {
        FemuCsdReadCompletion r = v->read_completions[i];
        if (merged) {
            FemuCsdReadCompletion *last = &v->read_completions[merged - 1];
            uint64_t small = FEMU_CACHE_SLOT_BYTES(FEMU_CACHE_LARGE_SLOTS);
            uint64_t limit = last->bytes > small && r.bytes > small ?
                             FEMU_CACHE_BLOCK_BYTES : small;
            if (last->afdm == r.afdm && last->pool == r.pool &&
                last->offset + last->bytes == r.offset &&
                last->lba + (last->bytes >> lbads) == r.lba &&
                last->bytes + r.bytes <= limit) {
                last->bytes += r.bytes;
                continue;
            }
        }
        v->read_completions[merged++] = r;
    }
    if (v->graph->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY) {
        v->graph->session->cache_profile.merged_reads += v->read_completed - merged;
    }
    pending -= v->read_completed - merged;
    v->read_completed = merged;
    if (profile) {
        csd->ftl_profile.register_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - begin;
    }
    for (unsigned first = 0; first < pending; first += CSD_CACHE_ENQUEUE_BATCH) {
        FemuCacheItem retired[CSD_CACHE_ENQUEUE_BATCH];
        unsigned logged = 0;
        csd_cache_collect(csd, true);
        begin = profile ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
        unsigned end = MIN(pending, first + CSD_CACHE_ENQUEUE_BATCH);
        for (unsigned i = first; i < end; i++) {
            const FemuCsdReadCompletion *r = i < v->read_completed ?
                &v->read_completions[i] : &direct;
            v->cache_afdm = r->afdm; v->cache_lba = r->lba;
            v->cache_offset = r->offset; v->cache_bytes = r->bytes;
            v->cache_pool = r->pool;
            csd_cache_publish_locked(csd, v, now, retired, &logged, profile);
        }
        if (profile) {
            csd->ftl_profile.register_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - begin;
        }
        csd_cache_dispatch(csd);
        if (logged) {
            qemu_mutex_unlock(&csd->lock);
            for (unsigned i = 0; i < logged; i++) {
                csd_cache_log(&retired[i], qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
            }
            qemu_mutex_lock(&csd->lock);
            if (!v->graph->session->cache_ring) {
                break;
            }
        }
    }
    v->read_completed = 0;
}

static struct femu_cache_slot csd_cache_slot_record(const FemuCacheItem *item)
{
    return (struct femu_cache_slot){
        .token = cpu_to_le64(item->event.token),
        .cookie = cpu_to_le64(item->event.cookie),
        .lba = cpu_to_le64(item->event.lba),
        .bytes = cpu_to_le64(item->event.bytes),
        .load_ns = cpu_to_le64(item->event.load_ns),
        .dma_start_ns = cpu_to_le64(item->dma_start_ns),
        .nsid = cpu_to_le32(item->event.nsid),
    };
}

static bool csd_cache_publish_slot(FemuCtrl *n, FemuCacheRing *ring, unsigned slot,
                                   struct femu_cache_slot *record, uint64_t now)
{
    uint64_t address = ring->rx.descriptor_dma + slot * sizeof(*record);
    uint64_t ready = cpu_to_le64(1);
    unsigned group = slot % FEMU_CACHE_READY_QUEUES;
    unsigned tail = ring->ready_tail[group];
    uint64_t queue = ring->rx.descriptor_dma + FEMU_CACHE_READY_OFFSET +
                     group * sizeof(struct femu_cache_ready_queue);

    record->dma_ns = cpu_to_le64(now);
    if (ring->queued) {
        uint32_t index = cpu_to_le32(slot);
        uint64_t entry = queue + offsetof(struct femu_cache_ready_queue, slots) +
                         tail * sizeof(index);
        if (femu_dma_write(n, entry, &index, sizeof(index))) {
            return true;
        }
    }
    if (femu_dma_write(n, address, record, sizeof(*record)) ||
        femu_dma_write(n, address, &ready, sizeof(ready))) {
        return true;
    }
    if (ring->queued) {
        unsigned next = (tail + 1) % FEMU_CACHE_READY_CAPACITY;
        uint32_t position = cpu_to_le32(next);
        smp_wmb();
        if (femu_dma_write(n, queue, &position, sizeof(position))) {
            return true;
        }
        ring->ready_tail[group] = next;
    }
    return false;
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

static void csd_cache_account_copy(FemuCacheItem *item)
{
    FemuCacheProfile *p = item->summary_profile;
    if (p) {
        p->dma_lock_ns += item->dma_lock_ns + item->update_lock_ns;
        p->slots_lock_ns += item->slot_lock_ns;
        if (item->dma_bytes) {
            p->copies++;
            p->bytes += item->dma_bytes;
            p->copy_ns += item->copy_end_ns - item->copy_begin_ns;
            p->copy_cpu_ns += item->copy_cpu_ns;
            if (item->link_begin_ns > item->dma_start_ns) {
                p->link_queue_ns += item->link_begin_ns - item->dma_start_ns;
            }
            if (item->link_end_ns > item->copy_end_ns) {
                p->link_after_copy_ns += item->link_end_ns - item->copy_end_ns;
            }
        }
    }
}

/* Ordinary LOADs retain their existing graph lifetime rules. */
static void csd_cache_release_input(FemuCsdState *csd, FemuCacheItem *item)
{
    FemuCsdGraph *g = item->graph;
    if (!g) return;
    if (item->summary_profile) {
        csd_cache_profile_wake(item, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    }
    csd_cache_account_copy(item);
    clear_bit(item - csd->cache, g->cache_items);
    g->cache_pending--;
    csd_cache_wake_graph(csd, item);
    item->graph = NULL;
}

/* Once the worker has stopped reading FDM, the receiver owns all copied
 * bytes. Link expiry and descriptor publication no longer need the source. */
static void csd_cache_unpin(FemuCsdState *csd, FemuCacheItem *item)
{
    struct femu_graph_read_pool *pool = item->pool;
    if (!pool) return;
    for (unsigned i = item->first_slot; i < item->first_slot + item->slot_count; i++) {
        uint16_t readers = qatomic_read(&pool->readers[i]) - 1;
        qatomic_store_release(&pool->readers[i], readers);
        pool->pinned -= !readers;
    }
    if (item->summary_profile) {
        csd->ftl_profile.pinned_dma_bytes += item->dma_bytes;
    }
    csd_cache_account_copy(item);
    item->pool = NULL;
    csd_afdm_unref(item->source_afdm);
    item->source_afdm = NULL;
}

static void csd_cache_finish(FemuCsdState *csd, FemuCacheItem *item)
{
    if (item->ring && item->slot >= 0) {
        qatomic_store_release(&item->ring->busy[item->slot], false);
    }
    csd_cache_release_input(csd, item);
    csd_cache_unpin(csd, item);
    item->event.token=0;
    clear_bit(item - csd->cache, csd->cache_runnable);
    csd_cache_remove_work(csd);
}

/* STOP is synchronous with actual memory writes, not simulated link time. */
static void csd_cache_ring_stop(FemuCsdState *csd,FemuCsdSession *session)
{
    FemuCacheRing *ring=session->cache_ring;
    if(!ring) return;
    qatomic_set(&ring->stopping, true);
    csd_cache_wait(csd, ring);
    for(unsigned i=0;i<ARRAY_SIZE(csd->cache);i++) {
        FemuCacheItem *item=&csd->cache[i];
        if(item->event.token && item->ring==ring) {
            if(!item->published && !item->dropped)
                qatomic_fetch_add(&ring->stats.stop_dropped_bytes,item->event.bytes);
            if (!item->graph) {
                csd_cache_finish(csd, item);
                continue;
            }
            item->ring=NULL; item->dropped=true;
            clear_bit(i, csd->cache_runnable);
        }
    }
    csd->cache_sweep = true;
    qatomic_set(&csd->cache_dirty,1);
    struct femu_cache_stats stats={
        .candidate_bytes=cpu_to_le64(ring->stats.candidate_bytes),.resident_bytes=cpu_to_le64(ring->stats.resident_bytes),
        .dma_bytes=cpu_to_le64(ring->stats.dma_bytes),.queue_dropped_bytes=cpu_to_le64(ring->stats.queue_dropped_bytes),
        .credit_dropped_bytes=cpu_to_le64(ring->stats.credit_dropped_bytes),.overwrite_dropped_bytes=cpu_to_le64(ring->stats.overwrite_dropped_bytes),
        .error_dropped_bytes=cpu_to_le64(ring->stats.error_dropped_bytes),.stop_dropped_bytes=cpu_to_le64(ring->stats.stop_dropped_bytes)};
    femu_dma_write(csd->n,ring->rx.descriptor_dma+FEMU_CACHE_STATS_OFFSET,&stats,sizeof(stats));
    session->cache_ring=NULL; g_free(ring->resident); g_free(ring);
}
static uint16_t csd_cache_control(FemuCtrl *n,NvmeCmd *cmd)
{
    FemuCsdState *csd=csd_state(n);
    unsigned action=le32_to_cpu(cmd->cdw10),id=le32_to_cpu(cmd->cdw11);
    FemuCacheRing *ring=NULL;
    if(action==FEMU_CACHE_RX_REGISTER) {
        uint32_t flags = le32_to_cpu(cmd->cdw12);
        if (flags & ~FEMU_CACHE_RX_QUEUED) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        ring=g_new0(FemuCacheRing,1);
        ring->queued = flags & FEMU_CACHE_RX_QUEUED;
        uint16_t status=dma_write_cmd(n,cmd,(void *)&ring->rx,sizeof(ring->rx));
        if(status) { g_free(ring); return status; }
        ring->rx.descriptor_dma=le64_to_cpu(ring->rx.descriptor_dma);
        if(!ring->rx.descriptor_dma || ring->rx.descriptor_dma%FEMU_CACHE_PAGE_BYTES) goto invalid;
        ring->rx.resident_dma=le64_to_cpu(ring->rx.resident_dma);
        ring->rx.resident_bytes=le64_to_cpu(ring->rx.resident_bytes);
        if(!ring->rx.resident_dma || !ring->rx.resident_bytes ||
           ring->rx.resident_bytes>DIV_ROUND_UP(n->namespaces[0].size,(uint64_t)FEMU_CACHE_PAGE_BYTES*8)) goto invalid;
        ring->resident=g_malloc(ring->rx.resident_bytes);
        if(femu_dma_read(n,ring->rx.resident_dma,ring->resident,ring->rx.resident_bytes)) goto invalid;
        for(unsigned i=0;i<FEMU_CACHE_TOTAL_PAGES;i++) {
                uint64_t *address=&ring->rx.pages[i];
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
        session->cache_ring=ring;
    }
    qemu_mutex_unlock(&csd->lock);
    return NVME_SUCCESS;
invalid:
    if(ring) g_free(ring->resident);
    g_free(ring); return NVME_INVALID_FIELD|NVME_DNR;
}

static void csd_cache_retire_completed(FemuCsdState *csd, FemuCacheItem *item)
{
    if (item->graph && (item->graph->flags & FEMU_CSD_GRAPH_PROFILE)) {
        csd->cache_sweep = true;
        csd_cache_wake_graph(csd, item);
    } else {
        csd_cache_finish(csd, item);
    }
}

/* FDM may be reused as soon as the link expires, independently of when
 * the worker publishes the descriptor or the Guest returns its credit. */
static void csd_cache_collect_publications(FemuCsdState *csd)
{
    unsigned first = find_first_bit(csd->cache_publishing, CSD_CACHE_ITEMS);
    uint64_t now = first < CSD_CACHE_ITEMS ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
    uint64_t deadline = UINT64_MAX;
    for (unsigned i = first;
         i < CSD_CACHE_ITEMS;
         i = find_next_bit(csd->cache_publishing, CSD_CACHE_ITEMS, i + 1)) {
        FemuCachePublication *pub = &csd->cache_publications[i];
        FemuCacheItem *item = &csd->cache[i];
        bool done = qatomic_load_acquire(&pub->done);
        if (done && pub->end_ns > now) now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        bool trace = item->graph && (item->graph->flags & FEMU_CSD_GRAPH_PROFILE);
        if (!item->data_done && (done || now >= item->dma_ns)) {
            item->data_done = true;
            if (trace) {
                csd_cache_profile_wake(item, now);
                csd_cache_wake_graph(csd, item);
            }
        }
        /* Block-level traces keep the graph long enough to correlate CU
         * timestamps. Normal completion and summary timing need only the
         * FDM lifetime, and can leave descriptor publication in the ring. */
        if (item->data_done && !trace) {
            csd_cache_release_input(csd, item);
        }
        if (!done) {
            if (!item->data_done) deadline = MIN(deadline, item->dma_ns);
            continue;
        }
        if (pub->profile && !pub->cancelled) {
            FemuCacheProfile *profile = pub->profile;
            profile->publish_count++;
            profile->publish_late_ns += pub->start_ns - pub->due_ns;
            profile->publish_collect_ns += now - pub->end_ns;
            profile->worker_ns[pub->worker] += pub->end_ns - pub->start_ns;
        }
        item->publish_start_ns = pub->start_ns;
        item->publish_end_ns = pub->end_ns;
        if (!pub->cancelled) item->dma_ns = pub->end_ns;
        item->published = !pub->failed && !pub->cancelled;
        item->dropped |= !item->published;
        item->slot = -1;
        item->ring->active--;
        clear_bit(i, csd->cache_publishing);
        qatomic_set(&item->dma_active, CACHE_IDLE);
        if (item->graph) csd_cache_retire_completed(csd, item);
        else csd_cache_finish(csd, item);
    }
    qatomic_set(&csd->cache_deadline, deadline);
}

/* Dispatch and completion use separate indices in the same per-worker lane.
 * Only the scheduler publishes submissions and reclaims completed entries. */
static void csd_cache_collect(FemuCsdState *csd, bool ftl)
{
    uint64_t begin = ftl ? csd_ftl_stamp(csd) : 0;
    for (unsigned i = 0; i < CSD_CACHE_WORKERS; i++) {
        FemuCacheWorker *worker = &csd->cache_workers[i];
        unsigned end = qatomic_load_acquire(&worker->completed);
        while (worker->collected != end) {
            FemuCacheDispatch *work = &worker->lane[worker->collected++ % CSD_CACHE_LANE];
            FemuCacheItem *item = work->item;
            FemuCacheRing *ring = item->ring;
            if (qatomic_read(&item->dma_active) != CACHE_CANCELLED) {
                /* Graph state can change while the worker owns the transfer. */
                work->result.waiters = qatomic_read(&item->waiters);
                work->result.consumers = item->consumers;
                work->result.cu_ns = item->cu_ns;
                work->result.wake_counted = item->wake_counted;
                work->result.dropped |= item->dropped;
                *item = work->result;
                if (work->summary) {
                    FemuCacheProfile *profile = item->summary_profile;
                    uint64_t collected = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                    uint64_t started = work->done_ns - work->busy_ns;
                    uint64_t queued = started - work->submit_ns;
                    uint64_t harvest = collected - work->done_ns;
                    profile->worker_ns[i] += work->busy_ns;
                    item->dma_report_ns = work->done_ns;
                    item->dma_collect_ns = collected;
                    profile->dispatch_count++;
                    profile->admission_ns += work->submit_ns - item->event.load_ns;
                    profile->lane_ns += queued;
                    profile->lane_max_ns = MAX(profile->lane_max_ns, queued);
                    profile->collect_ns += harvest;
                    if (item->dma_bytes) {
                        profile->copied_lane_ns += queued;
                        profile->inline_publishes += item->published;
                        if (item->link_end_ns > collected) {
                            profile->link_pending_count++;
                            profile->collect_to_link_ns += item->link_end_ns - collected;
                        }
                    }
                    if (work->credit_miss) {
                        profile->credit_count++;
                        profile->credit_lane_ns += queued;
                    }
                }
                /* A software copy with an outstanding link deadline owns
                 * its slot until the worker acknowledges publication. */
                if (item->slot >= 0) {
                    csd->cache_publications[item - csd->cache].profile =
                        item->summary_profile;
                    csd_cache_unpin(csd, item);
                    qatomic_set(&item->dma_active, CACHE_PUBLISHING);
                    set_bit(item - csd->cache, csd->cache_publishing);
                    continue;
                }
            }
            qatomic_set(&item->dma_active, CACHE_IDLE);
            ring->active--;
            csd_cache_retire_completed(csd, item);
        }
    }
    csd_cache_collect_publications(csd);
    if (begin) {
        csd->ftl_profile.collect_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - begin;
        }
}

static unsigned csd_cache_slot_owner(unsigned slot)
{
    return (slot % FEMU_CACHE_READY_QUEUES) % CSD_CACHE_WORKERS;
}

static void csd_cache_sample_credits(FemuCsdState *csd, FemuCacheRing *ring,
                                     uint64_t now)
{
    if (now < ring->credit_sample_ns) {
        return;
    }
    ring->credit_sample_ns = now + CSD_CACHE_CREDIT_SAMPLE_NS;
    memset(ring->credits, 0, sizeof(ring->credits));
    struct femu_cache_slot slots[FEMU_CACHE_RX_SLOTS];
    if (femu_dma_read(csd->n, ring->rx.descriptor_dma, slots, sizeof(slots))) {
        return;
    }
    for (unsigned i = 0; i < FEMU_CACHE_RX_SLOTS; i++) {
        if (!slots[i].ready && !qatomic_read(&ring->busy[i])) {
            ring->credits[i >= FEMU_CACHE_LARGE_SLOTS][csd_cache_slot_owner(i)]++;
        }
    }
}

static void csd_cache_dispatch(FemuCsdState *csd)
{
    if (csd->stopping) return;
    uint64_t begin = csd_ftl_stamp(csd);
    unsigned cursor = csd->cache_next_item;
    bool wake[CSD_CACHE_WORKERS] = {false};
    uint64_t now = 0;

    for (unsigned scanned = 0; scanned < CSD_CACHE_ITEMS;) {
        unsigned i = find_next_bit(csd->cache_runnable, CSD_CACHE_ITEMS, cursor);
        if (i == CSD_CACHE_ITEMS) {
            scanned += CSD_CACHE_ITEMS - cursor;
            cursor = 0;
            continue;
        }
        scanned += i - cursor + 1;
        if (scanned > CSD_CACHE_ITEMS) break;
        cursor = (i + 1) % CSD_CACHE_ITEMS;
        FemuCacheItem *item = &csd->cache[i];
        FemuCacheRing *ring = item->ring;
        bool small = item->event.bytes <= FEMU_CACHE_SLOT_BYTES(FEMU_CACHE_LARGE_SLOTS);
        unsigned workers = ring->queued ?
            MIN(CSD_CACHE_WORKERS, FEMU_CACHE_READY_QUEUES) : CSD_CACHE_WORKERS;
        if (ring->queued) {
            if (!now) now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            csd_cache_sample_credits(csd, ring, now);
        }
        FemuCacheWorker *worker = NULL;
        for (unsigned j = 0; j < workers; j++) {
            unsigned w = (csd->cache_next_worker + j) % workers;
            FemuCacheWorker *candidate = &csd->cache_workers[w];
            if (qatomic_read(&candidate->submitted) - candidate->collected >= CSD_CACHE_LANE) {
                continue;
            }
            if (!ring->queued || ring->credits[small][w]) {
                worker = candidate;
                break;
            }
            if (!worker) worker = candidate;
        }
        if (!worker) {
            if (workers == CSD_CACHE_WORKERS) break;
            continue;
        }

        /* Credits can change asynchronously. Keep dispatching on a miss and
         * let the DMA worker try the actual reservation once. */
        if (ring->queued && ring->credits[small][worker->index]) {
            ring->credits[small][worker->index]--;
        }
        unsigned submitted = qatomic_read(&worker->submitted);
        FemuCacheDispatch *work = &worker->lane[submitted % CSD_CACHE_LANE];
        *work = (FemuCacheDispatch){
            .item = item, .result = *item,
            .summary = item->summary_profile != NULL,
            .profiling = item->graph && (item->graph->flags & FEMU_CSD_GRAPH_PROFILE),
        };
        if (work->summary) {
            work->submit_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        }
        clear_bit(i, csd->cache_runnable);
        item->ring->active++;
        qatomic_set(&item->dma_active, CACHE_QUEUED);
        qatomic_store_release(&worker->submitted, submitted + 1);
        wake[worker->index] = true;
        csd->cache_next_worker = (worker->index + 1) % CSD_CACHE_WORKERS;
        csd->cache_next_item = cursor;
    }
    for (unsigned i = 0; i < CSD_CACHE_WORKERS; i++) {
        if (wake[i]) qemu_event_set(&csd->cache_workers[i].wake);
    }
    if (begin) {
        csd->ftl_profile.dispatch_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - begin;
        }
}

static void csd_cache_refill(FemuCsdState *csd)
{
    if (qatomic_read(&csd->cache_refill) && qatomic_xchg(&csd->cache_refill, 0)) {
        csd_cache_collect(csd, true);
        csd_cache_dispatch(csd);
    }
}

static int csd_cache_reserve_slot(FemuCacheWorker *worker, FemuCacheItem *item)
{
    FemuCacheRing *ring = item->ring;
    bool small = item->event.bytes <= FEMU_CACHE_SLOT_BYTES(FEMU_CACHE_LARGE_SLOTS);
    unsigned first = small ? FEMU_CACHE_LARGE_SLOTS : 0;
    unsigned count = small ? FEMU_CACHE_RX_SLOTS - first : FEMU_CACHE_LARGE_SLOTS;
    unsigned start = worker->next_slot[small];

    for (unsigned j = 0; j < count; j++) {
        unsigned index = (start + j) % count;
        unsigned slot = first + index;
        if (ring->queued && csd_cache_slot_owner(slot) != worker->index) {
            continue;
        }
        if (qatomic_read(&ring->busy[slot]) ||
            qatomic_cmpxchg(&ring->busy[slot], false, true)) {
            continue;
        }
        uint64_t ready;
        uint64_t address = ring->rx.descriptor_dma + slot * sizeof(struct femu_cache_slot);
        if (!femu_dma_read(worker->csd->n, address, &ready, sizeof(ready)) && !ready) {
            worker->next_slot[small] = (index + 1) % count;
            return slot;
        }
        qatomic_store_release(&ring->busy[slot], false);
    }
    return -1;
}

static void csd_cache_dma(FemuCacheWorker *worker, FemuCacheDispatch *work, uint64_t now)
{
    FemuCtrl *n = worker->csd->n;
    FemuCacheItem *item = &work->result;
    FemuCacheRing *ring = item->ring;
    bool publish = false, failed = false;

    if (qatomic_read(&ring->stopping) ||
        qatomic_read(&worker->csd->cache_cancel_publications)) {
        qatomic_fetch_add(&ring->stats.stop_dropped_bytes, item->event.bytes);
        item->dropped = true;
        return;
    }
    item->dma_start_ns = now;
    if (csd_cache_resident(ring, item->event.lba, item->event.bytes,
                           n->namespaces[item->event.nsid - 1].lbaf.lbads, true, n)) {
        qatomic_fetch_add(&ring->stats.resident_bytes, item->event.bytes);
        item->published = item->data_done = true;
        item->no_copy_done_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        return;
    }
    item->slot = csd_cache_reserve_slot(worker, item);
    if (item->slot < 0) {
        work->credit_miss = true;
        qatomic_fetch_add(&ring->stats.credit_dropped_bytes, item->event.bytes);
        item->dropped = true;
        item->no_copy_done_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        qatomic_inc(&worker->csd->cache_dropped);
        return;
    }
    struct timespec cpu_begin, cpu_end;
    bool timing = work->profiling || work->summary;
    if (timing) {
        item->copy_begin_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_begin);
    }
    uint64_t bytes = item->event.bytes;
    unsigned first = FEMU_CACHE_SLOT_OFFSET(item->slot) / FEMU_CACHE_PAGE_BYTES;
    for (uint64_t at = 0; at < bytes;) {
        unsigned page = at / FEMU_CACHE_PAGE_BYTES;
        uint64_t address = ring->rx.pages[first + page];
        uint64_t length = MIN(bytes - at, FEMU_CACHE_PAGE_BYTES);
        while (at + length < bytes && (at + length) % FEMU_CACHE_PAGE_BYTES == 0 &&
               ring->rx.pages[first + (at + length) / FEMU_CACHE_PAGE_BYTES] == address + length) {
            length += MIN(bytes - at - length, FEMU_CACHE_PAGE_BYTES);
        }
        item->segments++;
        if (femu_dma_write_population(n, address, item->data + at, length)) {
            failed = true;
            break;
        }
        at += length;
    }
    if (!failed) {
        if (timing) {
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end);
            item->copy_cpu_ns = (cpu_end.tv_sec - cpu_begin.tv_sec) * 1000000000LL +
                                cpu_end.tv_nsec - cpu_begin.tv_nsec;
        }
        item->copy_end_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        NvmeRequest transfer = {
            .ns = &n->namespaces[item->event.nsid - 1],
            .stime = now, .expire_time = now, .xfer_bytes = bytes,
        };
        nvme_charge_link(n, &transfer, true);
        item->dma_bytes = bytes;
        qatomic_fetch_add(&ring->stats.dma_bytes, bytes);
        item->link_begin_ns = transfer.link_timing.begin;
        item->link_end_ns = transfer.expire_time;
        item->dma_ns = MAX(item->copy_end_ns, item->link_end_ns);
        now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        publish = now >= item->dma_ns;
        if (!publish) {
            FemuCachePublication *pub =
                &worker->csd->cache_publications[work->item - worker->csd->cache];
            *pub = (FemuCachePublication){
                .ring = ring, .record = csd_cache_slot_record(item),
                .due_ns = item->dma_ns, .slot = item->slot, .worker = worker->index,
            };
            /* The shared link assigns successive deadlines in order.
             * Each worker's pending publications therefore form a FIFO. */
            QTAILQ_INSERT_TAIL(&worker->publications, pub, entry);
            return;
        }
    }

    if (publish) {
        struct femu_cache_slot record = csd_cache_slot_record(item);
        item->publish_start_ns = now;
        failed = csd_cache_publish_slot(n, ring, item->slot, &record, now);
        item->publish_end_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        item->dma_ns = item->publish_end_ns;
        item->data_done = true;
        item->published = !failed;
    }
    if (failed) {
        qatomic_fetch_add(&ring->stats.error_dropped_bytes, item->event.bytes);
        qatomic_inc(&worker->csd->cache_dropped);
        item->no_copy_done_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        item->dropped = true;
    }
    if (publish || failed) {
        qatomic_store_release(&ring->busy[item->slot], false);
        item->slot = -1;
    }
}

static void csd_cache_publish_pending(FemuCacheWorker *worker, FemuCachePublication *pub,
                                      uint64_t now, bool cancel)
{
    FemuCacheRing *ring = pub->ring;
    QTAILQ_REMOVE(&worker->publications, pub, entry);
    pub->cancelled = cancel;
    if (cancel) {
        qatomic_fetch_add(&ring->stats.stop_dropped_bytes, le64_to_cpu(pub->record.bytes));
    } else {
        pub->start_ns = now;
        pub->failed = csd_cache_publish_slot(worker->csd->n, ring, pub->slot, &pub->record, now);
        pub->end_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (pub->failed) {
            qatomic_fetch_add(&ring->stats.error_dropped_bytes, le64_to_cpu(pub->record.bytes));
            qatomic_inc(&worker->csd->cache_dropped);
        }
    }
    qatomic_store_release(&ring->busy[pub->slot], false);
    qatomic_store_release(&pub->done, true);
}

static uint64_t csd_cache_publish_due(FemuCacheWorker *worker)
{
    if (QTAILQ_EMPTY(&worker->publications)) return UINT64_MAX;
    FemuCsdState *csd = worker->csd;
    FemuCachePublication *pub, *next;
    bool changed = false;
    if (qatomic_read(&csd->cache_draining)) {
        QTAILQ_FOREACH_SAFE(pub, &worker->publications, entry, next) {
            if (qatomic_read(&csd->cache_cancel_publications) ||
                qatomic_read(&pub->ring->stopping)) {
                csd_cache_publish_pending(worker, pub, 0, true);
                changed = true;
            }
        }
    }
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    while ((pub = QTAILQ_FIRST(&worker->publications)) && pub->due_ns <= now) {
        csd_cache_publish_pending(worker, pub, now, false);
        changed = true;
        now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }
    if (changed) {
        qatomic_set(&csd->cache_dirty, 1);
        if (qatomic_read(&csd->cache_draining)) qemu_event_set(&csd->cache_completion);
    }
    return pub ? pub->due_ns : UINT64_MAX;
}

static void *csd_cache_worker(void *opaque)
{
    femu_bind_device_thread();
    FemuCacheWorker *worker = opaque;
    FemuCsdState *csd = worker->csd;
    unsigned completed = 0;
    rcu_register_thread();
    while (!qatomic_read(&csd->stopping)) {
        uint64_t due = csd_cache_publish_due(worker);
        if (completed == qatomic_load_acquire(&worker->submitted)) {
            if (due != UINT64_MAX) {
                uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                if (due > now + 15000) {
                    g_usleep(MIN((due - now) / 1000, 25));
                } else {
                    cpu_relax();
                }
                continue;
            }
            qemu_event_reset(&worker->wake);
            if (!qatomic_read(&csd->stopping) &&
                completed == qatomic_load_acquire(&worker->submitted)) {
                qemu_event_wait(&worker->wake);
            }
            continue;
        }
        FemuCacheDispatch *work = &worker->lane[completed % CSD_CACHE_LANE];
        FemuCacheItem *item = work->item;
        if (qatomic_cmpxchg(&item->dma_active, CACHE_QUEUED, CACHE_ACTIVE) == CACHE_QUEUED) {
            uint64_t begin = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            csd_cache_dma(worker, work, begin);
            if (work->summary) {
                work->done_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                work->busy_ns = work->done_ns - begin;
            }
        }
        bool urgent = qatomic_read(&item->waiters) != 0;
        qatomic_store_release(&worker->completed, ++completed);
        qatomic_set(&csd->cache_dirty, 1);
        unsigned queued = qatomic_read(&worker->submitted) - completed;
        if (queued == CSD_CACHE_LANE / 2 || queued == 0) {
            qatomic_set(&csd->cache_refill, 1);
        }
        if (urgent) qatomic_set(&csd->cache_urgent, 1);
        if (qatomic_read(&csd->cache_draining)) qemu_event_set(&csd->cache_completion);
    }
    rcu_unregister_thread();
    return NULL;
}

/* STOP/reset synchronize with writes to the Guest mapping, outside the FTL
 * hot path. The scheduler lock prevents dispatch while completions drain. */
static void csd_cache_wait(FemuCsdState *csd, FemuCacheRing *ring)
{
    qatomic_set(&csd->cache_cancel_publications, !ring);
    qatomic_set_mb(&csd->cache_draining, true);
    for (;;) {
        qemu_event_reset(&csd->cache_completion);
        csd_cache_collect(csd, false);
        bool pending = ring ? ring->active != 0 :
            find_first_bit(csd->cache_publishing, CSD_CACHE_ITEMS) < CSD_CACHE_ITEMS;
        if (!ring) {
            for (unsigned i = 0; i < CSD_CACHE_WORKERS; i++) {
                FemuCacheWorker *worker = &csd->cache_workers[i];
                pending |= qatomic_read(&worker->submitted) != worker->collected;
            }
        }
        if (!pending) break;
        qemu_event_wait(&csd->cache_completion);
    }
    qatomic_set(&csd->cache_draining, false);
    qatomic_set(&csd->cache_cancel_publications, false);
}

void femu_csd_cache_quiesce(FemuCtrl *n)
{
    FemuCsdState *csd = csd_state(n);
    if (!csd) return;
    qemu_mutex_lock(&csd->lock);
    csd_cache_wait(csd, NULL);
    qemu_mutex_unlock(&csd->lock);
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

/* Guest clears its ready bits asynchronously. Sample Guest ownership and
 * device reservations without taking a slot away from the workers. */
static void csd_cache_sample_slots(FemuCsdState *csd)
{
    if (!qatomic_read(&csd->cache_profile_sessions)) return;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (now < csd->cache_next_sample) return;
    csd->cache_next_sample = now + CSD_CACHE_SAMPLE_NS;
    if (qemu_mutex_trylock(&csd->lock)) return;
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, csd->sessions);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        FemuCsdSession *session = value;
        FemuCacheRing *ring = session->cache_ring;
        FemuCacheProfile *p = &session->cache_profile;
        if (!p->enabled || !ring) continue;
        struct femu_cache_slot slots[FEMU_CACHE_RX_SLOTS];
        if (!femu_dma_read(csd->n, ring->rx.descriptor_dma, slots, sizeof(slots))) {
            unsigned large = 0, small = 0, large_ready = 0, small_ready = 0;
            for (unsigned i = 0; i < FEMU_CACHE_RX_SLOTS; i++) {
                bool ready = le64_to_cpu(slots[i].ready) != 0;
                bool unavailable = ready || qatomic_read(&ring->busy[i]);
                if (i < FEMU_CACHE_LARGE_SLOTS) {
                    large += unavailable;
                    large_ready += ready;
                } else {
                    small += unavailable;
                    small_ready += ready;
                }
            }
            unsigned small_slots = FEMU_CACHE_RX_SLOTS - FEMU_CACHE_LARGE_SLOTS;
            p->slot_samples++;
            p->slot_large_full += large == FEMU_CACHE_LARGE_SLOTS;
            p->slot_small_full += small == small_slots;
            p->slot_all_full += large + small == FEMU_CACHE_RX_SLOTS;
            p->slot_large_ready += large_ready == FEMU_CACHE_LARGE_SLOTS;
            p->slot_small_ready += small_ready == small_slots;
        }
    }
    qemu_mutex_unlock(&csd->lock);
}

/* Jobs/graph nodes retain their own AFDM references while computing. Once
 * DMA is published or abandoned, its metadata no longer protects any input;
 * keeping it until CU completion needlessly exhausts the bounded queue. */
void femu_csd_cache_poll(FemuCtrl *n)
{
    FemuCsdState *csd=csd_state(n);
    if (!csd) return;
    csd_cache_sample_slots(csd);
    if (!qatomic_read(&csd->cache_work) &&
        !qatomic_read(&csd->cache_urgent)) return;
    uint64_t deadline = qatomic_read(&csd->cache_deadline);
    if (!qatomic_xchg(&csd->cache_dirty, 0) &&
        !qatomic_xchg(&csd->cache_urgent, 0) &&
        (deadline == UINT64_MAX || qemu_clock_get_ns(QEMU_CLOCK_REALTIME) < deadline)) return;
    FemuCacheItem logs[CSD_CACHE_ITEMS];
    unsigned count=0;
    csd_ftl_lock(csd, &csd->ftl_profile.cache_lock_ns);
    qatomic_xchg(&csd->cache_urgent, 0);
    csd_cache_collect(csd, true);
    uint64_t now=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (csd->cache_sweep) {
        uint64_t begin = csd_ftl_stamp(csd);
        csd->cache_sweep = false;
        for (unsigned i = 0; i < CSD_CACHE_ITEMS; i++) {
            FemuCacheItem *item = &csd->cache[i];
            if (item->event.token && qatomic_read(&item->dma_active) == CACHE_IDLE &&
                (item->published || item->dropped)) {
                if (item->graph && (item->graph->flags & FEMU_CSD_GRAPH_PROFILE)) logs[count++] = *item;
                csd_cache_finish(csd, item);
            }
        }
        if (begin) {
            csd->ftl_profile.sweep_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - begin;
        }
    }
    csd_cache_dispatch(csd);
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
        (g->flags & ~(FEMU_CSD_GRAPH_SERIAL | FEMU_CSD_GRAPH_PROFILE | FEMU_CSD_GRAPH_CACHE |
                      FEMU_CSD_GRAPH_CACHE_SUMMARY)) || !g->cookie ||
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
    if ((g->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY) && !g->session->cache_profile.enabled) {
        FemuCacheProfile *p = &g->session->cache_profile;
        p->begin_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        p->queue_full_start = csd_cache_full_time(csd, p->begin_ns);
        p->ftl_start = csd->ftl_profile;
        p->ftl_cpu_start = csd_ftl_cpu_ns(csd);
        p->enabled = true;
        pthread_spin_lock(&n->pcie_lock);
        if (!n->pcie_profile_users++) {
            n->pcie_tx_without_hpcp = n->pcie_tx_next_avail_time;
            n->pcie_profile = (struct FemuLinkProfile){0};
        }
        pthread_spin_unlock(&n->pcie_lock);
        qatomic_inc(&csd->cache_profile_sessions);
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

static bool csd_read_pool_valid(FemuCsdJob *job, const struct femu_graph_read *r,
                                uint64_t bytes)
{
    uint64_t offset = r->pool_offset;
    uint64_t length = job->args.mr_len[r->afdm_index];
    if (offset % _Alignof(struct femu_graph_read_pool) || offset > length ||
        length - offset < sizeof(struct femu_graph_read_pool)) {
        return false;
    }
    const struct femu_graph_read_pool *pool =
        (const void *)((uint8_t *)job->args.mr_addr[r->afdm_index] + offset);
    uint32_t width = le32_to_cpu(pool->slot_bytes);
    uint32_t slots = le32_to_cpu(pool->slots);
    uint32_t limit = le32_to_cpu(pool->pin_limit);
    uint64_t span = (uint64_t)width * slots;
    if (!width || width % FEMU_CACHE_PAGE_BYTES || !slots ||
        slots > FEMU_GRAPH_READ_POOL_MAX_SLOTS || limit >= slots ||
        pool->pinned > limit || span > offset ||
        slots * sizeof(pool->readers[0]) > length - offset - sizeof(*pool) ||
        r->offset < offset - span || r->offset > offset ||
        bytes > offset - r->offset) {
        return false;
    }
    uint64_t start = r->offset - (offset - span);
    for (unsigned i = start / width; i < DIV_ROUND_UP(start + bytes, width); i++) {
        if (qatomic_read(&pool->readers[i])) {
            return false;
        }
    }
    return true;
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
        r->pool_offset = le32_to_cpu(r->pool_offset);
        uint64_t bytes = (uint64_t)r->blocks << ns->lbaf.lbads;
        if (r->nsid != ns->id || !r->blocks ||
            r->afdm_index >= job->args.numr ||
            !job->afdms[r->afdm_index] ||
            r->lba > le64_to_cpu(ns->id_ns.nsze) ||
            r->blocks > le64_to_cpu(ns->id_ns.nsze) - r->lba ||
            r->offset > job->args.mr_len[r->afdm_index] ||
            bytes > job->args.mr_len[r->afdm_index] - r->offset ||
            bytes > UINT64_MAX - total ||
            (r->pool_offset && !csd_read_pool_valid(job, r, bytes))) {
            g_free(reads); return NVME_INVALID_FIELD | NVME_DNR;
        }
        total += bytes;
    }
    /* Aliased destinations keep descriptor order. They cannot be offered
     * to optional DMA because a later range may overwrite their bytes. */
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
    if (v->serial_reads) {
        for (unsigned i = 0; i < count; i++) {
            reads[i].pool_offset = 0;
        }
    }
    v->reads = reads; v->range = 0; v->range_blocks = 0;
    v->io_action = 4; v->io_token = count; v->io_bytes = total;
    v->transferred = 0; v->state = GRAPH_IO;
    return NVME_SUCCESS;
}

static bool csd_cache_check_reuse(FemuCsdState *csd, FemuCsdGraphNode *v,
                                  uintptr_t first, uint64_t bytes, bool can_wait)
{
    FemuCsdGraph *g = v->graph;
    bool busy = false;

    /* The worker claims queued source data with a CAS. If the FTL wins the
     * cancellation race, that worker will never touch the overwritten bytes. */
    for (unsigned i = find_first_bit(g->cache_items, CSD_CACHE_ITEMS);
         i < CSD_CACHE_ITEMS;
         i = find_next_bit(g->cache_items, CSD_CACHE_ITEMS, i + 1)) {
        FemuCacheItem *item = &csd->cache[i];
        uintptr_t at = (uintptr_t)item->data;
        if (first >= at + item->event.bytes || at >= first + bytes) {
            continue;
        }
        if (item->dma_start_ns && item->dma_bytes && !item->data_done &&
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= item->dma_ns) {
            item->data_done = true;
        }
        if (item->data_done || item->published || item->dropped) {
            if ((g->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY) && !item->wake_counted) {
                csd_cache_profile_wake(item, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
            }
        } else if (!item->dma_start_ns &&
                   (qatomic_read(&item->dma_active) == CACHE_IDLE ||
                    qatomic_cmpxchg(&item->dma_active, CACHE_QUEUED, CACHE_CANCELLED) == CACHE_QUEUED)) {
            qatomic_fetch_add(&item->ring->stats.overwrite_dropped_bytes,
                              item->event.bytes);
            item->dropped = true;
            clear_bit(i, csd->cache_runnable);
            qatomic_inc(&csd->cache_dropped);
            csd->cache_sweep = true;
            csd_cache_signal_done(csd, item);
        } else {
            busy = true;
            if (can_wait) {
                qatomic_or(&item->waiters, UINT64_C(1) << (v - g->nodes));
            }
        }
    }
    return busy;
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
    v->read_copy_ready_ns = 0;
    v->state = GRAPH_IO;
next_read:
    if (g->status) { goto complete; }
    csd_cache_refill(csd);
    uint64_t lat = 0, bytes = 0;
    uint64_t begin = (g->flags & FEMU_CSD_GRAPH_PROFILE) ?
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
    uint16_t status;
    FemuCsdIoTiming timing={0};
    FemuCsdReadCopy copy_work = {0};
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
        bool summary = g->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY;
        if (csd_cache_check_reuse(csd, v, first, bytes, !issued)) {
            if (issued) {
                goto complete;
            }
            if (!v->cache_wait_begin) {
                v->cache_wait_begin = summary ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : now;
                if (summary) {
                    csd_cache_wait_start(&g->session->cache_profile.reuse, v->cache_wait_begin);
                }
            }
            v->state=GRAPH_CACHE_WAIT;
            return;
        }
        if(v->cache_wait_begin) {
            if (summary) {
                csd_cache_profile_reuse_end(v, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
            }
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
        v->cache_pool = r->pool_offset ? (void *)(afdm->data + r->pool_offset) : NULL;
        status=csd_cache_prepare(csd,v,offset,bytes,le64_to_cpu(copy->slba));
        if (status) { goto fail; }
        status = csd_nvm_copy_locked(n, ns, &req, &lat, afdm, begin ? &timing : NULL,
                                     bytes >= CSD_COPY_MIN_BYTES &&
                                     csd->copy_count < CSD_COPY_QUEUE ? &copy_work : NULL);
        if (status) { goto fail; }
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
                status = req.status;
                goto fail;
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
        v->cache_pool = NULL;
        status = csd_cache_prepare(csd, v, le64_to_cpu(copy->offset), bytes,le64_to_cpu(copy->slba));
        if (status) { goto fail; }
        v->cache_bytes=bytes;
        status = csd_nvm_copy_locked(n, ns, &req, &lat, v->afdm, begin ? &timing : NULL,
                                     batch_read && bytes >= CSD_COPY_MIN_BYTES &&
                                     csd->copy_count < CSD_COPY_QUEUE ? &copy_work : NULL);
        if (status) { goto fail; }
        v->cache_lba=le64_to_cpu(copy->slba); v->cache_bytes=bytes;
        v->cache_offset=le64_to_cpu(copy->offset);
        v->transferred += bytes; v->range_blocks += blocks;
        if (v->range_blocks == le32_to_cpu(range.blocks)) { v->range++; v->range_blocks = 0; }
    }
    if (copy_work.bytes) {
        unsigned tail = (csd->copy_head + csd->copy_count) % CSD_COPY_QUEUE;
        copy_work.node = v;
        csd->copies[tail] = copy_work;
        csd->copy_count++;
        v->copies_pending++;
        qemu_cond_signal(&csd->copy_cond);
    }
    /* FTL calls still have one owner. Their common arrival lets NAND/channel
     * state schedule independent ranges concurrently; the batch completes
     * at its slowest deadline, rather than summing per-range waits. */
    v->deadline = now + lat;
    completed = MAX(completed, v->deadline);
    if (batch_read && v->cache_candidate) {
        if (!v->read_completions) {
            v->read_completions = g_new(FemuCsdReadCompletion, FEMU_CSD_GRAPH_MAX_NODES);
        }
        v->read_completions[v->read_completed++] = (FemuCsdReadCompletion){
            v->cache_afdm, v->cache_lba, v->cache_offset, v->cache_bytes, v->cache_pool };
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
        csd->copy_count < CSD_COPY_QUEUE &&
        (v->job ? v->transferred < v->io_bytes : v->range < v->wire.count)) {
        goto next_read;
    }
    goto complete;

fail:
    if (!g->status) {
        g->status = status;
        g->result = v - g->nodes + 1;
    }
complete:
    v->deadline = completed;
    v->state = GRAPH_IO_DONE;
    /* Retain the active node and its AFDM even if a later range fails. The
     * event is dispatched only after both the copies and NAND have finished. */
    if (!v->copies_pending) {
        csd_graph_event_locked(csd, v);
    }
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
    if (!(v->graph->flags & FEMU_CSD_GRAPH_PROFILE) || !v->graph->cache_pending) return;
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
    bool resume = qatomic_read(&csd->cache_urgent);
    if (!resume && !qatomic_read(&csd->ready_count) &&
        now < qatomic_read(&csd->graph_deadline)) { return; }
    if (resume) femu_csd_cache_poll(n);
    csd_ftl_lock(csd, &csd->ftl_profile.graph_lock_ns);
    now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    FemuCsdGraphNode *v;
    while (!resume && (v = pqueue_peek(csd->graph_events)) && v->deadline <= now) {
        if (qatomic_read(&csd->cache_urgent)) {
            qemu_mutex_unlock(&csd->lock);
            femu_csd_cache_poll(n);
            csd_ftl_lock(csd, &csd->ftl_profile.graph_lock_ns);
            now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            break;
        }
        FemuCsdGraph *g = v->graph;
        bool profile_resume = (g->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY) &&
            !g->status && v->state == GRAPH_IO_DONE && v->job &&
            v->io_action == 4 && v->transferred == v->io_bytes;
        uint64_t resume_ready = 0, resume_start = 0;
        uint64_t register_start = 0, collect_start = 0, dispatch_start = 0;
        if (profile_resume) {
            resume_ready = MAX(v->deadline, v->read_copy_ready_ns);
            resume_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            register_start = csd->ftl_profile.register_ns;
            collect_start = csd->ftl_profile.collect_ns;
            dispatch_start = csd->ftl_profile.dispatch_ns;
        }
        csd_cache_refill(csd);
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
                if (!g->status) {
                    if (v->io_action == 4 && (g->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY)) {
                        FemuCacheProfile *p = &g->session->cache_profile;
                        p->read_issue_count++;
                        p->read_issue_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - v->deadline;
                    }
                    csd_graph_io(csd, g, v, now);
                } else {
                    g->result = v - g->nodes + 1;
                    csd_graph_node_done(csd, v);
                }
            } else { csd_graph_node_done(csd, v); }
        } else {
            /* All ranges in this bounded window are complete. Keep every
             * LOAD's HPCP metadata; only then resume/continue the parent. */
            csd_cache_publish_reads(csd, v, now);
            bool done = v->job ? v->transferred == v->io_bytes : v->range == v->wire.count;
            if (!done) {
                csd_graph_io(csd, g, v, now);
            } else if (v->job) {
                g_free(v->reads); v->reads = NULL;
                if (v->spill) { v->spill->busy--; v->spill = NULL; }
                if (profile_resume) {
                    FemuCacheProfile *p = &g->session->cache_profile;
                    p->read_resume_count++;
                    p->read_resume_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - resume_ready;
                    p->resume_before_event_ns += resume_start - resume_ready;
                    p->resume_register_ns += csd->ftl_profile.register_ns - register_start;
                    p->resume_collect_ns += csd->ftl_profile.collect_ns - collect_start;
                    p->resume_dispatch_ns += csd->ftl_profile.dispatch_ns - dispatch_start;
                }
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
        csd_cache_refill(csd);
        if (!g->poller && g->req) { continue; } /* defer will enqueue it again */
        uint64_t begin = (g->flags & FEMU_CSD_GRAPH_PROFILE) ? now : 0;
        for(unsigned i=0;i<g->count;i++) {
            v=&g->nodes[i];
            if(v->state!=GRAPH_CACHE_WAIT) continue;
            if (!g->status) {
                csd_graph_io(csd, g, v, now);
            } else {
                csd_graph_node_done(csd, v);
            }
            now=qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        }
        while (!g->status && g->ready && (!(g->flags & FEMU_CSD_GRAPH_SERIAL) || !g->active)) {
            unsigned i = ctz64(g->ready); g->ready &= g->ready - 1;
            v = &g->nodes[i]; g->active++;
            if (v->job) { csd_graph_compute_ready(csd, v, now); }
            else {
                csd_graph_io(csd, g, v, now);
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
                for (unsigned i=0;i<ARRAY_SIZE(csd->cache);i++) {
                    FemuCacheItem *item=&csd->cache[i];
                    if (!item->event.token || item->graph!=g) { continue; }
                    item->consumers=0;
                    if (!item->cu_ns) { item->cu_ns=now; }
                    item->dropped=true;
                    clear_bit(i, csd->cache_runnable);
                    qatomic_cmpxchg(&item->dma_active, CACHE_QUEUED, CACHE_CANCELLED);
                }
                csd->cache_sweep = true;
                qatomic_set(&csd->cache_dirty,1);
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
        if (g->cache_pending) {
            if ((g->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY) && !g->cache_tail_begin) {
                g->cache_tail_begin = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                csd_cache_wait_start(&g->session->cache_profile.tail, g->cache_tail_begin);
            }
            continue;
        }
        if (g->cache_tail_begin) {
            csd_cache_wait_end(&g->session->cache_profile.tail, g->cache_tail_begin,
                               qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
            g->cache_tail_begin = 0;
        }
        if (g->req) {
            NvmeRequest *req = g->req;
            req->status = g->status; req->cqe.res64 = cpu_to_le64(g->result);
            req->expire_time = now; req->reqlat = now - req->stime;
            struct rte_ring *ring = n->to_poller[g->poller];
            if (femu_ring_enqueue(ring, (void *)&req, 1) != 1) {
                csd_graph_ready_locked(csd, g); break;
            }
            if ((g->flags & FEMU_CSD_GRAPH_CACHE) && !(g->flags & FEMU_CSD_GRAPH_CACHE_SUMMARY)) {
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
            csd_graph_session(csd, ns, req, lat) :
            csd_nvm_copy_locked(n, ns, req, lat, NULL, NULL, NULL);
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
