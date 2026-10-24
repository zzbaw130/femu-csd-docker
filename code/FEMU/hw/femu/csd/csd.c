#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include <gmodule.h>

#include "csd.h"
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
    NvmeNamespace *ns;
    GHashTable *cancelled; /* cancellation racing graph acceptance */
} FemuCsdSession;

typedef struct FemuCsdSpill {
    uint64_t id, page, pages, bytes, creator;
    FemuCsdSession *session;
    unsigned busy;
} FemuCsdSpill;

enum { GRAPH_READY, GRAPH_COMPUTING, GRAPH_COMPUTED, GRAPH_IO,
       GRAPH_IO_DONE, GRAPH_DONE };
typedef struct FemuCsdGraphNode {
    struct femu_graph_node wire;
    FemuCsdJob *job;
    FemuCsdAfdm *afdm;
    unsigned state, range;
    uint64_t deadline, result, transferred, range_blocks;
    uint64_t io_action, io_token, io_bytes;
    FemuCsdSpill *spill;
    uint16_t status;
} FemuCsdGraphNode;

struct FemuCsdGraph {
    NvmeRequest *req;
    int poller;
    FemuCsdSession *session;
    uint8_t *data;
    uint32_t bytes, count, flags;
    uint64_t cookie, done, result;
    uint16_t status;
    FemuCsdGraphNode nodes[FEMU_CSD_GRAPH_MAX_NODES];
    QTAILQ_ENTRY(FemuCsdGraph) entry;
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
    unsigned graph_count;
    GHashTable *sessions, *spills;
    uint32_t next_session;
    uint64_t next_spill, spill_pages, spill_base;
    uint8_t *spill_space, *spill_bitmap;
};

static void *csd_worker(void *opaque);
static void csd_job_free_locked(FemuCsdJob *job);
static void csd_graph_destroy_locked(FemuCsdState *csd, FemuCsdGraph *g);
static void csd_graph_computed_locked(FemuCsdState *csd, FemuCsdJob *job,
                                      uint16_t status, int64_t result,
                                      int64_t host_ns);
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

/* callers hold csd->lock, the tables included */
static void csd_afdm_unref(gpointer opaque)
{
    FemuCsdAfdm *afdm = opaque;

    if (!afdm || --afdm->refs) {
        return;
    }

    afdm->csd->fdm_used -= afdm->size;
    g_free(afdm->data);
    g_free(afdm);
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
    csd->n = n;
    qemu_cond_init(&csd->job_cond);
    QTAILQ_INIT(&csd->pending);
    QTAILQ_INIT(&csd->running);
    QTAILQ_INIT(&csd->graphs);
    QSLIST_INIT(&csd->dead);
    csd->workers = g_new0(QemuThread, csd->params.nr_cu);
    for (i = 0; i < csd->params.nr_cu; i++) {
        qemu_thread_create(&csd->workers[i], "femu-csd-cu", csd_worker, csd,
                           QEMU_THREAD_JOINABLE);
    }
    n->csd_ctrl_state = csd;

    femu_log("%s,CSD mode initialized: fdm=%" PRIu64 "MB, "
             "nr_cu=%u, nr_thread=%u\n",
             n->devname, csd->params.fdm_size_mb, csd->params.nr_cu,
             csd->params.nr_thread);
}

static void csd_exit(FemuCtrl *n)
{
    FemuCsdState *csd = csd_state(n);
    int i;

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

    /* a program still running holds up teardown until it returns */
    qemu_mutex_lock(&csd->lock);
    csd->stopping = true;
    qemu_cond_broadcast(&csd->job_cond);
    qemu_mutex_unlock(&csd->lock);
    for (i = 0; i < csd->params.nr_cu; i++) {
        qemu_thread_join(&csd->workers[i]);
    }
    qemu_mutex_lock(&csd->lock);
    while (!QTAILQ_EMPTY(&csd->pending)) {
        FemuCsdJob *job = QTAILQ_FIRST(&csd->pending);

        QTAILQ_REMOVE(&csd->pending, job, entry);
        if (!job->graph) {
            csd_job_free_locked(job);
        }
    }
    while (!QTAILQ_EMPTY(&csd->graphs)) {
        csd_graph_destroy_locked(csd, QTAILQ_FIRST(&csd->graphs));
    }
    qemu_mutex_unlock(&csd->lock);

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
    qemu_mutex_destroy(&csd->lock);
    qemu_cond_destroy(&csd->job_cond);
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
    FemuCsdState *csd = opaque;
    FemuCtrl *n = csd->n;
    FemuCsdProgram *dead;
    FemuCsdJob *job;
    struct rte_ring *ring;
    int64_t result, host_ns;
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
            continue;
        }

        QTAILQ_INSERT_TAIL(&csd->running, job, entry);
        qemu_mutex_unlock(&csd->lock);

        result = 0;
        host_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        status = csd_run_program(job->program, &job->args, &result);
        host_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - host_ns;

        qemu_mutex_lock(&csd->lock);
        if (job->graph) {
            csd_graph_computed_locked(csd, job, status, result, host_ns);
            QTAILQ_REMOVE(&csd->running, job, entry);
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
        status = dma_read_cmd(n, cmd, afdm->data + offset, size);
    }
    qemu_mutex_unlock(&csd->lock);

    if (status) {
        return status | NVME_DNR;
    }

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
        status = dma_write_cmd(n, cmd, afdm->data + offset, size);
    }
    qemu_mutex_unlock(&csd->lock);

    if (status) {
        return status | NVME_DNR;
    }

    req->cqe.n.result = size;
    return NVME_SUCCESS;
}

static uint16_t csd_nvm_copy_locked(FemuCtrl *n, NvmeNamespace *ns, NvmeRequest *req,
                             uint64_t *lat, FemuCsdAfdm *held)
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
        *lat = bb_ftl_process_req(n, ns, &internal);
        if (write) {
            ns->ssd->host_write_pages = host_pages;
        }
        status = internal.status;
        if (!status) {
            void *nvm = (uint8_t *)n->mbe->logical_space + nvm_offset;

            backend_lock_range(n->mbe, nvm_offset, size);
            if (write) {
                memcpy(nvm, afdm->data + offset, size);
            } else {
                memcpy(afdm->data + offset, nvm, size);
            }
            backend_unlock_range(n->mbe, nvm_offset, size);
            if (write) {
                nvme_mark_written(ns, slba, nlb);
                nvme_note_user_write(n);
            }
        }
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
    for (unsigned i = 0; i < g->count; i++) {
        if (g->nodes[i].job) {
            csd_job_free_locked(g->nodes[i].job);
        }
        if (g->nodes[i].afdm) {
            csd_afdm_unref(g->nodes[i].afdm);
        }
    }
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
    uint64_t lat = bb_ftl_process_req(csd->n, ns, &req);
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
    g_hash_table_remove(csd->sessions, GUINT_TO_POINTER(id));
    return NVME_SUCCESS;
}

static void csd_graph_defer(FemuCtrl *n, NvmeRequest *req, int poller)
{
    FemuCsdState *csd = csd_state(n);
    FemuCsdGraph *g = req->opaque;
    req->opaque = NULL;
    qemu_mutex_lock(&csd->lock);
    g->poller = poller; /* visible to the FTL thread only after insertion */
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
        (g->flags & ~FEMU_CSD_GRAPH_SERIAL) || !g->cookie ||
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

static void csd_graph_io(FemuCsdState *csd, FemuCsdGraph *g, FemuCsdGraphNode *v,
                          uint64_t now)
{
    FemuCtrl *n = csd->n;
    NvmeNamespace *ns = g->session->ns;
    uint64_t lat = 0, bytes;
    uint16_t status;
    NvmeRequest req = { .ns = ns, .stime = now, .expire_time = now };
    if (v->job) {
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
            uint64_t host_pages = ns->ssd->host_write_pages;
            lat = bb_ftl_process_req(n, ns, &req);
            ns->ssd->host_write_pages = host_pages;
            if (req.status) { g->status = req.status; return; }
            uint8_t *io = v->job->args.mr_addr[v->job->args.numr - 1];
            if (write) { memcpy(csd->spill_space + offset, io + v->transferred, bytes); }
            else { memcpy(io + v->transferred, csd->spill_space + offset, bytes); }
            v->transferred += bytes;
        }
    } else {
        struct femu_graph_range range;
        memcpy(&range, g->data + v->wire.payload + v->range * sizeof(range), sizeof(range));
        uint32_t blocks = MIN((uint64_t)65536, le32_to_cpu(range.blocks) - v->range_blocks);
        bytes = (uint64_t)blocks << ns->lbaf.lbads;
        NvmeCsdNvmToAfdmCmd *copy = (void *)&req.cmd;
        copy->opcode = v->wire.operation == FEMU_GRAPH_STORE ? NVME_CMD_CSD_AFDM_TO_NVM : NVME_CMD_CSD_NVM_TO_AFDM;
        copy->slba = cpu_to_le64(le64_to_cpu(range.lba) + v->range_blocks);
        copy->nlb = cpu_to_le16(blocks - 1); copy->id = cpu_to_le32(v->wire.afdm);
        copy->offset = cpu_to_le64(v->wire.afdm_offset + v->transferred);
        status = csd_nvm_copy_locked(n, ns, &req, &lat, v->afdm);
        if (status) { g->status = status; return; }
        v->transferred += bytes; v->range_blocks += blocks;
        if (v->range_blocks == le32_to_cpu(range.blocks)) { v->range++; v->range_blocks = 0; }
    }
    v->deadline = now + lat;
    v->state = GRAPH_IO_DONE;
}

void femu_csd_graph_poll(FemuCtrl *n)
{
    FemuCsdState *csd = csd_state(n);
    FemuCsdGraph *g, *next;
    if (!csd || !qatomic_read(&csd->graph_count)) { return; }
    qemu_mutex_lock(&csd->lock);
    QTAILQ_FOREACH_SAFE(g, &csd->graphs, entry, next) {
        if (!g->poller && g->req) { continue; }
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        bool running = false;
        for (unsigned i = 0; i < g->count; i++) {
            FemuCsdGraphNode *v = &g->nodes[i];
            if (v->state == GRAPH_COMPUTING || v->deadline > now) { running = true; continue; }
            if (v->state == GRAPH_COMPUTED) {
                if (!g->status && (v->status || (int64_t)v->result < 0)) {
                    g->status = v->status ? v->status : NVME_INTERNAL_DEV_ERROR;
                    g->result = i + 1;
                } else if (!g->status && (v->result >> 60) >= 1 && (v->result >> 60) <= 3) {
                    g->status = csd_graph_spill_begin(csd, g, v);
                    if (g->status) { g->result = i + 1; }
                } else { v->state = GRAPH_DONE; }
            }
            if (v->state == GRAPH_IO_DONE) {
                bool done = v->job ? v->transferred == v->io_bytes : v->range == v->wire.count;
                if (!done) { v->state = GRAPH_IO; }
                else if (v->job) {
                    if (v->spill) { v->spill->busy--; v->spill = NULL; }
                    v->job->args.cparam2 = v->io_token;
                    v->io_action = 0; v->state = GRAPH_READY;
                } else { v->result = v->transferred; v->state = GRAPH_DONE; }
            }
            if (v->state == GRAPH_DONE && !(g->done & (1ULL << i))) {
                g->done |= 1ULL << i;
                if (!g->status) { g->result = v->result; }
            }
        }
        if (!g->status) {
            bool serial_busy = false;
            for (unsigned i = 0; i < g->count; i++) {
                unsigned st = g->nodes[i].state;
                if (st != GRAPH_READY && st != GRAPH_DONE) { serial_busy = true; }
            }
            for (unsigned i = 0; i < g->count && !g->status; i++) {
                FemuCsdGraphNode *v = &g->nodes[i];
                if (v->state == GRAPH_IO) {
                    csd_graph_io(csd, g, v, now); running = true;
                    if (g->status) { g->result = i + 1; }
                }
                else if (v->state == GRAPH_READY && !(v->wire.dependencies & ~g->done) &&
                         (!(g->flags & FEMU_CSD_GRAPH_SERIAL) || !serial_busy)) {
                    serial_busy = true; running = true;
                    if (v->job) {
                        v->job->arrival = now;
                        v->job->reserved_end = v->job->runtime ? csd_hold_cu_locked(csd, now, v->job->runtime) : 0;
                        v->state = GRAPH_COMPUTING;
                        QTAILQ_INSERT_TAIL(&csd->pending, v->job, entry);
                        qemu_cond_signal(&csd->job_cond);
                    } else {
                        v->state = GRAPH_IO; csd_graph_io(csd, g, v, now);
                        if (g->status) { g->result = i + 1; }
                    }
                }
            }
        }
        uint64_t all = UINT64_MAX >> (64 - g->count);
        if (running || (!g->status && g->done != all)) { continue; }
        /* No CSF or modeled I/O is in flight before freeing references. */
        for (unsigned i = 0; i < g->count; i++) {
            if (g->nodes[i].spill) { g->nodes[i].spill->busy--; g->nodes[i].spill = NULL; }
        }
        if (g->status) {
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
        if (g->req) {
            NvmeRequest *req = g->req;
            req->status = g->status; req->cqe.res64 = cpu_to_le64(g->result);
            req->expire_time = now; req->reqlat = now - req->stime;
            struct rte_ring *ring = n->to_poller[g->poller];
            if (femu_ring_enqueue(ring, (void *)&req, 1) != 1) { continue; }
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
            csd_graph_session(csd, ns, req, lat) : csd_nvm_copy_locked(n, ns, req, lat, NULL);
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
