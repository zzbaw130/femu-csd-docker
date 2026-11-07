// SPDX-License-Identifier: GPL-2.0
/* Guest receive memory and clean page-cache publication; no AER or block I/O. */
#include <linux/module.h>
#include <linux/swap.h>
#include <linux/blkdev.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/nvme.h>
#include <linux/mm.h>
#include <linux/vmstat.h>
#include <linux/bitmap.h>
#include <linux/rwsem.h>
#include "csd-cache.h"
static char *device="/dev/nvme0n1";
module_param(device,charp,0444);
static bool profile;
module_param(profile,bool,0644);
static struct bdev_handle *bdev;
static struct device *dma_device;
extern int nvme_submit_sync_cmd(struct request_queue *,struct nvme_command *,void *,unsigned);
struct mapping {
    struct list_head list;
    struct file *file,*owner;
    struct femu_cache_mapping range;
    unsigned char *resident;
};
static LIST_HEAD(mappings);
static DEFINE_MUTEX(mapping_lock);
struct receive_slot {
    struct mutex lock;
    bool synced, dcp_acquired, dcp_done, shared_done, hpcp_busy, hpcp_done;
    DECLARE_BITMAP(file_pages, FEMU_CACHE_RX_PAGES);
};
struct receiver {
    struct femu_cache_slot *slots;
    dma_addr_t descriptor_dma;
    void *data;
    dma_addr_t *pages;
    unsigned char *resident;
    dma_addr_t resident_dma;
    size_t resident_bytes;
    unsigned mapped,session;
    struct rw_semaphore lifecycle;
    unsigned targets;
    struct receive_slot state[FEMU_CACHE_RX_SLOTS];
    struct list_head bindings;
    bool pooled;
};
/* One receiver is allocated and DMA-mapped at module/device initialization.
 * Concurrent sessions use independent receivers, never a query execution lock. */
static struct receiver warm_receiver;
static DEFINE_MUTEX(receiver_pool_lock);
static bool receiver_pool_busy;
static atomic64_t blocks,filled,cached,skipped,errors;
static atomic64_t capacity_skipped;
static atomic64_t device_candidate,device_resident,device_dma,device_queue_drop;
static atomic64_t device_credit_drop,device_overwrite_drop,device_error_drop,device_stop_drop;
/* Wall time within publication; local page counters avoid per-page atomics. */
static atomic64_t publish_ns,sync_cpu_ns,lookup_ns,folio_ns,copy_ns,sync_device_ns;
static struct dentry *profile_dir;
static int profile_show(struct seq_file *out,void *unused)
{
    seq_printf(out,"CACHE_STATS blocks=%llu filled=%llu cached=%llu skipped=%llu errors=%llu\n",
        atomic64_read(&blocks),atomic64_read(&filled),atomic64_read(&cached),
        atomic64_read(&skipped),atomic64_read(&errors));
    seq_printf(out,"CACHE_HOST publish_ns=%llu sync_cpu_ns=%llu lookup_ns=%llu folio_ns=%llu copy_ns=%llu sync_device_ns=%llu\n",
        atomic64_read(&publish_ns),atomic64_read(&sync_cpu_ns),atomic64_read(&lookup_ns),
        atomic64_read(&folio_ns),atomic64_read(&copy_ns),atomic64_read(&sync_device_ns));
    seq_printf(out,"CACHE_ADMISSION capacity_skipped_bytes=%llu reserve_bytes=%llu\n",
        atomic64_read(&capacity_skipped),(u64)(totalram_pages()/8)*PAGE_SIZE);
    seq_printf(out,"CACHE_DEVICE candidate_bytes=%llu resident_bytes=%llu dma_bytes=%llu queue_dropped_bytes=%llu credit_dropped_bytes=%llu overwrite_dropped_bytes=%llu error_dropped_bytes=%llu stop_dropped_bytes=%llu\n",
        atomic64_read(&device_candidate),atomic64_read(&device_resident),atomic64_read(&device_dma),atomic64_read(&device_queue_drop),
        atomic64_read(&device_credit_drop),atomic64_read(&device_overwrite_drop),atomic64_read(&device_error_drop),atomic64_read(&device_stop_drop));
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(profile);
static void receiver_free(struct receiver *r)
{
    struct mapping *m,*next;
    list_for_each_entry_safe(m,next,&r->bindings,list) { list_del(&m->list); fput(m->file); kfree(m->resident); kfree(m); }
    if(r->pooled) {
        memset(r->slots,0,FEMU_CACHE_DESCRIPTOR_BYTES);
        mutex_lock(&receiver_pool_lock);
        receiver_pool_busy=false;
        mutex_unlock(&receiver_pool_lock);
        r->slots=NULL; r->data=NULL; r->mapped=0; r->pooled=false;
        r->pages=NULL;
        r->resident=NULL;
        return;
    }
    for(unsigned i=0;i<r->mapped;i++)
        dma_unmap_page(dma_device,r->pages[i],
                       PAGE_SIZE,DMA_FROM_DEVICE);
    vfree(r->data);
    if(r->slots) dma_free_coherent(dma_device,FEMU_CACHE_DESCRIPTOR_BYTES,r->slots,r->descriptor_dma);
    if(r->resident) dma_free_coherent(dma_device,r->resident_bytes,r->resident,r->resident_dma);
    kvfree(r->pages);
    r->slots=NULL; r->data=NULL; r->mapped=0;
    r->pages=NULL;
    r->resident=NULL;
}
static int receiver_allocate(struct receiver *r)
{
    int ret=-ENOMEM;
    if(r->slots) return -EBUSY;
    r->slots=dma_alloc_coherent(dma_device,FEMU_CACHE_DESCRIPTOR_BYTES,&r->descriptor_dma,GFP_KERNEL);
    r->data=vmalloc_user(FEMU_CACHE_DATA_BYTES);
    r->pages=kvcalloc(FEMU_CACHE_TOTAL_PAGES,sizeof(*r->pages),GFP_KERNEL);
    r->resident_bytes=DIV_ROUND_UP_ULL(bdev_nr_bytes(bdev->bdev),PAGE_SIZE*8);
    r->resident=dma_alloc_coherent(dma_device,r->resident_bytes,&r->resident_dma,GFP_KERNEL);
    if(!r->slots || !r->data || !r->pages || !r->resident) goto fail;
    memset(r->slots,0,FEMU_CACHE_DESCRIPTOR_BYTES);
    for(unsigned i=0;i<FEMU_CACHE_TOTAL_PAGES;i++) {
        dma_addr_t address=dma_map_page(dma_device,vmalloc_to_page(r->data+(size_t)i*PAGE_SIZE),
                                        0,PAGE_SIZE,DMA_FROM_DEVICE);
        if(dma_mapping_error(dma_device,address)) goto fail;
        r->pages[i]=address;
        r->mapped++;
    }
    return 0;
fail:
    receiver_free(r); return ret;
}
static int receiver_setup(struct receiver *r)
{
    if(r->slots) return -EBUSY;
    mutex_lock(&receiver_pool_lock);
    if(!receiver_pool_busy) {
        receiver_pool_busy=true;
        r->slots=warm_receiver.slots; r->descriptor_dma=warm_receiver.descriptor_dma;
        r->data=warm_receiver.data; r->mapped=warm_receiver.mapped;
        r->pages=warm_receiver.pages; r->pooled=true;
        r->resident=warm_receiver.resident; r->resident_dma=warm_receiver.resident_dma;
        r->resident_bytes=warm_receiver.resident_bytes;
    }
    mutex_unlock(&receiver_pool_lock);
    return r->pooled?0:receiver_allocate(r);
}
/* The module owns STOP even on process exit, before DMA mappings disappear. */
static int receiver_bind(struct receiver *r,unsigned session)
{
    struct nvme_command command={};
    struct femu_cache_rx *info=NULL;
    if(session && (!r->slots || r->session)) return -EINVAL;
    if(!session && !r->session) return 0;
    command.common.opcode=FEMU_CACHE_CONTROL;
    command.common.nsid=cpu_to_le32(1);
    command.common.cdw10=cpu_to_le32(session?FEMU_CACHE_RX_REGISTER:FEMU_CACHE_RX_STOP);
    command.common.cdw11=cpu_to_le32(session?session:r->session);
    if(session) {
        struct mapping *m;
        if (r->targets & FEMU_CACHE_TARGET_HPCP)
            command.common.cdw12 = cpu_to_le32(FEMU_CACHE_RX_QUEUED);
        memset((void *)r->slots+FEMU_CACHE_READY_OFFSET,0,
               FEMU_CACHE_READY_QUEUES*sizeof(struct femu_cache_ready_queue));
        memset(r->resident,0,r->resident_bytes);
        mutex_lock(&mapping_lock);
        list_for_each_entry(m,&mappings,list) {
            struct mapping *copy=kmalloc(sizeof(*copy),GFP_KERNEL);
            if(!copy) { mutex_unlock(&mapping_lock); return -ENOMEM; }
            *copy=*m; copy->resident=NULL; copy->file=get_file(m->file); list_add_tail(&copy->list,&r->bindings);
            if(m->resident) for(u64 page=0;page<m->range.bytes/PAGE_SIZE;page++) {
                u64 physical=m->range.lba_bytes/PAGE_SIZE+page;
                if(m->resident[page/8]&(1U<<(page%8))) r->resident[physical/8]|=1U<<(physical%8);
            }
        }
        mutex_unlock(&mapping_lock);
        info=kvzalloc(sizeof(*info),GFP_KERNEL);
        if(!info) return -ENOMEM;
        info->descriptor_dma=cpu_to_le64(r->descriptor_dma);
        info->resident_dma=cpu_to_le64(r->resident_dma);
        info->resident_bytes=cpu_to_le64(r->resident_bytes);
        for(unsigned i=0;i<FEMU_CACHE_TOTAL_PAGES;i++) info->pages[i]=cpu_to_le64(r->pages[i]);
        dma_wmb();
    }
    if(session) r->session=session; /* retain cleanup ownership even on timeout */
    int ret=nvme_submit_sync_cmd(bdev_get_queue(bdev->bdev),&command,info,info?sizeof(*info):0);
    if(!session && !ret) {
        const struct femu_cache_stats *s=(void *)r->slots+FEMU_CACHE_STATS_OFFSET;
        dma_rmb();
        atomic64_add(le64_to_cpu(s->candidate_bytes),&device_candidate);
        atomic64_add(le64_to_cpu(s->resident_bytes),&device_resident);
        atomic64_add(le64_to_cpu(s->dma_bytes),&device_dma);
        atomic64_add(le64_to_cpu(s->queue_dropped_bytes),&device_queue_drop);
        atomic64_add(le64_to_cpu(s->credit_dropped_bytes),&device_credit_drop);
        atomic64_add(le64_to_cpu(s->overwrite_dropped_bytes),&device_overwrite_drop);
        atomic64_add(le64_to_cpu(s->error_dropped_bytes),&device_error_drop);
        atomic64_add(le64_to_cpu(s->stop_dropped_bytes),&device_stop_drop);
    }
    kvfree(info);
    if(!ret) r->session=session;
    return ret>0?-EIO:ret;
}
static void sync_slot(struct receiver *r, unsigned index, bool for_cpu)
{
    unsigned first = FEMU_CACHE_SLOT_OFFSET(index) / PAGE_SIZE;
    unsigned count = le64_to_cpu(r->slots[index].bytes) / PAGE_SIZE;
    u64 begin = READ_ONCE(profile) ? ktime_get_ns() : 0;

    for (unsigned p = 0; p < count; p++) {
        if (for_cpu)
            dma_sync_single_for_cpu(dma_device, r->pages[first+p], PAGE_SIZE, DMA_FROM_DEVICE);
        else
            dma_sync_single_for_device(dma_device, r->pages[first+p], PAGE_SIZE, DMA_FROM_DEVICE);
    }
    if (begin)
        atomic64_add(ktime_get_ns()-begin, for_cpu ? &sync_cpu_ns : &sync_device_ns);
}

static int slot_ready(struct receiver *r, unsigned index)
{
    struct femu_cache_slot *slot = &r->slots[index];
    u64 bytes, lba, disk_bytes = bdev_nr_bytes(bdev->bdev);
    unsigned sector = bdev_logical_block_size(bdev->bdev);

    if (le64_to_cpu(smp_load_acquire(&slot->ready)) != 1)
        return -EAGAIN;
    dma_rmb();
    bytes = le64_to_cpu(slot->bytes);
    lba = le64_to_cpu(slot->lba);
    if (le32_to_cpu(slot->error) || !bytes || bytes > FEMU_CACHE_SLOT_BYTES(index) ||
        bytes % PAGE_SIZE || lba > disk_bytes / sector || bytes > disk_bytes - lba * sector ||
        (lba * sector) % PAGE_SIZE) {
        atomic64_inc(&errors);
        smp_store_release(&slot->ready, cpu_to_le64(0));
        return -EIO;
    }
    return 0;
}

static void mark_resident(struct receiver *r, u64 page)
{
    unsigned char *byte = &r->resident[page/8];
    unsigned char mask = 1U << (page%8);
    unsigned char old;

    /* Different slots may contribute bits to the same DMA-visible byte. */
    do {
        old = READ_ONCE(*byte);
        if (old & mask)
            return;
    } while (cmpxchg(byte, old, old | mask) != old);
}

/* Both consumers have stopped reading before this returns the slot to FEMU. */
static void finish_slot(struct receiver *r, unsigned index)
{
    struct receive_slot *state = &r->state[index];
    struct femu_cache_slot *slot = &r->slots[index];
    bool dcp = r->targets & FEMU_CACHE_TARGET_DCP;
    bool hpcp = r->targets & FEMU_CACHE_TARGET_HPCP;
    u64 first;
    unsigned count, page;

    if ((dcp && !state->dcp_done) || (hpcp && !state->hpcp_done))
        return;
    first = le64_to_cpu(slot->lba) * bdev_logical_block_size(bdev->bdev) / PAGE_SIZE;
    count = le64_to_cpu(slot->bytes) / PAGE_SIZE;
    if (!dcp || state->shared_done) {
        if (hpcp) {
            for_each_set_bit(page, state->file_pages, count)
                mark_resident(r, first+page);
        } else {
            for (page = 0; page < count; page++)
                mark_resident(r, first+page);
        }
    }
    sync_slot(r, index, false);
    state->synced = state->dcp_acquired = state->dcp_done = state->shared_done = false;
    state->hpcp_busy = state->hpcp_done = false;
    atomic64_inc(&blocks);
    dma_wmb();
    smp_store_release(&slot->ready, cpu_to_le64(0));
}

static int acquire_slot(struct receiver *r, unsigned index)
{
    struct receive_slot *state = &r->state[index];
    int ret;

    mutex_lock(&state->lock);
    if (!(r->targets & FEMU_CACHE_TARGET_DCP) || state->dcp_acquired || state->dcp_done) {
        ret = -EALREADY;
    } else {
        ret = state->synced ? 0 : slot_ready(r, index);
        if (!ret) {
            if (!state->synced)
                sync_slot(r, index, true);
            state->synced = state->dcp_acquired = true;
        }
    }
    mutex_unlock(&state->lock);
    return ret;
}

static int release_slot(struct receiver *r, unsigned index, bool shared_done)
{
    struct receive_slot *state = &r->state[index];

    mutex_lock(&state->lock);
    if (!state->dcp_acquired) {
        mutex_unlock(&state->lock);
        return -EINVAL;
    }
    state->dcp_acquired = false;
    state->dcp_done = true;
    state->shared_done = shared_done;
    finish_slot(r, index);
    mutex_unlock(&state->lock);
    return 0;
}

static struct mapping *find_mapping(struct list_head *head, u32 nsid, u64 at,
                                    u64 *next)
{
    struct mapping *map;

    list_for_each_entry(map, head, list) {
        if (map->range.nsid != nsid)
            continue;
        if (at < map->range.lba_bytes)
            *next = min(*next, map->range.lba_bytes);
        else if (at - map->range.lba_bytes < map->range.bytes)
            return map;
    }
    return NULL;
}

static int publish_slot(struct receiver *r,unsigned index)
{
    struct receive_slot *state = &r->state[index];
    struct femu_cache_slot *slot = &r->slots[index];
    bool timing = READ_ONCE(profile), full = false;
    u64 begin = timing ? ktime_get_ns() : 0, mark;
    u64 lookup = 0, folio_time = 0, copy_time = 0;
    u64 bytes, lba;
    unsigned count;
    int ret;

    mutex_lock(&state->lock);
    if (!(r->targets & FEMU_CACHE_TARGET_HPCP) || state->hpcp_busy || state->hpcp_done) {
        mutex_unlock(&state->lock);
        return -EALREADY;
    }
    ret = state->synced ? 0 : slot_ready(r, index);
    if (ret) {
        mutex_unlock(&state->lock);
        return ret;
    }
    if (!state->synced)
        sync_slot(r, index, true);
    state->synced = state->hpcp_busy = true;
    mutex_unlock(&state->lock);

    bytes = le64_to_cpu(slot->bytes);
    lba = le64_to_cpu(slot->lba);
    count = bytes / PAGE_SIZE;
    bitmap_zero(state->file_pages, count);
    u64 physical=lba*bdev_logical_block_size(bdev->bdev);
    /* MemAvailable includes reclaimable file cache and would allow population
     * to recycle its own earlier pages. Reserve one eighth of RAM as genuinely
     * free pages; optional allocation must never initiate direct reclaim. */
    bool admit=global_zone_page_state(NR_FREE_PAGES)>totalram_pages()/8;
    full=!admit;
    struct mapping *binding=NULL;
    struct mapping late={};
    u32 nsid=le32_to_cpu(slot->nsid);
    u64 gap_end=0;
    for(unsigned p=0;p<count;p++) {
        if(timing) mark=ktime_get_ns();
        struct file *file=NULL;
        u64 offset=0,at=physical+(u64)p*PAGE_SIZE;
        /* One LOAD normally remains inside one registered extent. Reuse
         * that mapping instead of searching every extent for every 4KiB. */
        if(at>=gap_end && (!binding || at-binding->range.lba_bytes>=binding->range.bytes)) {
            if(late.file) {
                fput(late.file);
                late.file=NULL;
            }
            u64 next=U64_MAX;
            binding=find_mapping(&r->bindings,nsid,at,&next);
            if(!binding) {
                /* An InitPlan can bind the receiver before the main plan
                 * opens its relations. Retain a file reference across page
                 * publication, even if registration closes meanwhile. */
                mutex_lock(&mapping_lock);
                struct mapping *map=find_mapping(&mappings,nsid,at,&next);
                if(map) {
                    late.range=map->range;
                    late.file=get_file(map->file);
                    binding=&late;
                }
                mutex_unlock(&mapping_lock);
                if(!binding) gap_end=next;
            }
        }
        if(binding) { file=binding->file; offset=binding->range.file_offset+at-binding->range.lba_bytes; }
        if(timing) { u64 now=ktime_get_ns(); lookup+=now-mark; mark=now; }
        if(!file) { atomic64_add(PAGE_SIZE,&skipped); continue; }
        struct address_space *mapping = file->f_mapping;
        pgoff_t page = offset >> PAGE_SHIFT;
        struct folio *folio = __filemap_get_folio(mapping, page, 0, 0);
        if (!IS_ERR(folio)) {
            /* A referenced, valid hit needs no content lock: no bytes change. */
            if (folio_test_uptodate(folio) && !folio_test_dirty(folio) &&
                READ_ONCE(folio->mapping) == mapping) {
                if (timing) folio_time += ktime_get_ns()-mark;
                atomic64_add(PAGE_SIZE,&cached);
                __set_bit(p, state->file_pages);
                folio_put(folio);
                continue;
            }
            if (!folio_trylock(folio)) {
                folio_put(folio);
                folio = ERR_PTR(-EAGAIN);
            } else if (folio->mapping != mapping) {
                folio_unlock(folio);
                folio_put(folio);
                folio = ERR_PTR(-EAGAIN);
            }
        } else if (PTR_ERR(folio) == -ENOENT && admit) {
            folio = __filemap_get_folio(mapping, page, FGP_LOCK|FGP_NOWAIT|FGP_CREAT, GFP_NOWAIT);
        }
        if(timing) { u64 now=ktime_get_ns(); folio_time+=now-mark; mark=now; }
        if(IS_ERR(folio)) {
            atomic64_add(PAGE_SIZE,&skipped);
            if(!admit || PTR_ERR(folio)==-ENOMEM) {
                atomic64_add(PAGE_SIZE,&capacity_skipped); full=true;
            }
            continue;
        }
        if(folio_test_uptodate(folio) || folio_test_dirty(folio) || folio_order(folio)) {
            if(folio_test_uptodate(folio) && !folio_test_dirty(folio)) {
                atomic64_add(PAGE_SIZE,&cached);
                __set_bit(p, state->file_pages);
            }
            else atomic64_add(PAGE_SIZE,&skipped);
        } else {
            void *target=kmap_local_folio(folio,0);
            memcpy(target,r->data+FEMU_CACHE_SLOT_OFFSET(index)+(size_t)p*PAGE_SIZE,PAGE_SIZE);
            kunmap_local(target); flush_dcache_folio(folio); folio_mark_uptodate(folio);
            /* Population serves a real device read, like buffered file input.
             * Give the new page its first access; duplicate DMA must not keep
             * promoting already cached pages over the HOST working set. */
            folio_mark_accessed(folio);
            if(timing) copy_time+=ktime_get_ns()-mark;
            atomic64_add(PAGE_SIZE,&filled);
            __set_bit(p, state->file_pages);
        }
        folio_unlock(folio); folio_put(folio);
    }
    if(late.file) fput(late.file);
    mutex_lock(&state->lock);
    state->hpcp_busy = false;
    state->hpcp_done = true;
    finish_slot(r, index);
    mutex_unlock(&state->lock);
    if (timing) {
        atomic64_add(lookup, &lookup_ns);
        atomic64_add(folio_time, &folio_ns);
        atomic64_add(copy_time, &copy_ns);
        atomic64_add(ktime_get_ns()-begin, &publish_ns);
    }
    return full ? FEMU_CACHE_HPCP_FULL : 0;
}

static int receiver_mode(struct receiver *r, unsigned targets)
{
    if (!targets)
        return r->targets;
    if (targets == r->targets)
        return 0;
    if (targets & ~(FEMU_CACHE_TARGET_DCP|FEMU_CACHE_TARGET_HPCP))
        return -EINVAL;
    if (r->session &&
        (targets != FEMU_CACHE_TARGET_HPCP ||
         r->targets != (FEMU_CACHE_TARGET_DCP|FEMU_CACHE_TARGET_HPCP)))
        return -EINVAL;
    for (unsigned i = 0; i < FEMU_CACHE_RX_SLOTS; i++)
        if (r->state[i].dcp_acquired)
            return -EBUSY;
    r->targets = targets;
    /* The PG consumer has relinquished DCP. Release slots whose file
     * publication already completed while it was still a required target. */
    for (unsigned i = 0; i < FEMU_CACHE_RX_SLOTS; i++)
        if (r->state[i].hpcp_done)
            finish_slot(r, i);
    return 0;
}
static void clear_mappings(struct file *owner)
{
    struct mapping *m,*next;
    list_for_each_entry_safe(m,next,&mappings,list) if(m->owner==owner) {
        list_del(&m->list); fput(m->file); kfree(m->resident); kfree(m);
    }
}
static long cache_ioctl(struct file *file,unsigned cmd,unsigned long arg)
{
    struct receiver *r=file->private_data;
    struct femu_cache_mapping range;
    struct mapping *map;
    if(!capable(CAP_SYS_ADMIN)) return -EPERM;
    if(cmd==FEMU_CACHE_RX_ADMISSION)
        return global_zone_page_state(NR_FREE_PAGES)>totalram_pages()/8?0:FEMU_CACHE_HPCP_FULL;
    if (cmd == FEMU_CACHE_RX_PUBLISH || cmd == FEMU_CACHE_RX_ACQUIRE ||
        cmd == FEMU_CACHE_RX_RELEASE) {
        unsigned long index = arg;
        int ret = -EINVAL;

        if (cmd == FEMU_CACHE_RX_RELEASE)
            index &= ~FEMU_CACHE_RELEASE_DCP;
        down_read(&r->lifecycle);
        if (r->slots && index < FEMU_CACHE_RX_SLOTS) {
            if (cmd == FEMU_CACHE_RX_PUBLISH)
                ret = publish_slot(r, index);
            else if (cmd == FEMU_CACHE_RX_ACQUIRE)
                ret = acquire_slot(r, index);
            else
                ret = release_slot(r, index, arg & FEMU_CACHE_RELEASE_DCP);
        }
        up_read(&r->lifecycle);
        return ret;
    }
    if (cmd == FEMU_CACHE_RX_SETUP || cmd == FEMU_CACHE_RX_MODE ||
        cmd == FEMU_CACHE_RX_BIND || cmd == FEMU_CACHE_RX_UNBIND) {
        int ret;

        down_write(&r->lifecycle);
        if (cmd == FEMU_CACHE_RX_SETUP)
            ret = receiver_setup(r);
        else if (cmd == FEMU_CACHE_RX_MODE)
            ret = receiver_mode(r, arg);
        else {
            ret = receiver_bind(r, cmd == FEMU_CACHE_RX_BIND ? arg : 0);
        }
        up_write(&r->lifecycle);
        return ret;
    }
    if(cmd==FEMU_CACHE_CLEAR) {
        mutex_lock(&mapping_lock); clear_mappings(file); mutex_unlock(&mapping_lock); return 0;
    }
    if(cmd!=FEMU_CACHE_REGISTER) return -ENOTTY;
    if(copy_from_user(&range,(void __user *)arg,sizeof(range))) return -EFAULT;
    if(range.nsid!=1 || !range.bytes || range.lba_bytes>U64_MAX-range.bytes ||
       range.lba_bytes>bdev_nr_bytes(bdev->bdev) || range.bytes>bdev_nr_bytes(bdev->bdev)-range.lba_bytes ||
       range.flags&~FEMU_CACHE_NEEDS_DCP || (range.lba_bytes|range.file_offset|range.bytes)&(PAGE_SIZE-1)) return -EINVAL;
    map=kzalloc(sizeof(*map),GFP_KERNEL);
    if(!map) return -ENOMEM;
    map->file=fget(range.fd);
    if(!map->file || !S_ISREG(file_inode(map->file)->i_mode) ||
       !file_inode(map->file)->i_sb->s_bdev ||
       file_inode(map->file)->i_sb->s_bdev->bd_disk!=bdev->bdev->bd_disk ||
       range.file_offset>i_size_read(file_inode(map->file)) ||
       range.bytes>i_size_read(file_inode(map->file))-range.file_offset) {
        if(map->file) fput(map->file);
        kfree(map); return -EINVAL;
    }
    map->range=range; map->owner=file;
    if(range.resident_bits) {
        map->resident=memdup_user((void __user *)(unsigned long)range.resident_bits,DIV_ROUND_UP_ULL(range.bytes,PAGE_SIZE*8));
        if(IS_ERR(map->resident)) { int ret=PTR_ERR(map->resident); fput(map->file); kfree(map); return ret; }
    }
    mutex_lock(&mapping_lock); list_add_tail(&map->list,&mappings); mutex_unlock(&mapping_lock);
    return 0;
}
static int cache_open(struct inode *inode,struct file *file)
{
    struct receiver *r=kzalloc(sizeof(*r),GFP_KERNEL);
    if(!r) return -ENOMEM;
    init_rwsem(&r->lifecycle);
    r->targets = FEMU_CACHE_TARGET_HPCP;
    for (unsigned i = 0; i < FEMU_CACHE_RX_SLOTS; i++)
        mutex_init(&r->state[i].lock);
    INIT_LIST_HEAD(&r->bindings); file->private_data=r; return 0;
}
static int cache_mmap(struct file *file,struct vm_area_struct *vma)
{
    struct receiver *r=file->private_data;
    if(!r->slots) return -EINVAL;
    if(!vma->vm_pgoff && vma->vm_end-vma->vm_start==FEMU_CACHE_DESCRIPTOR_BYTES)
        return dma_mmap_coherent(dma_device,vma,r->slots,r->descriptor_dma,FEMU_CACHE_DESCRIPTOR_BYTES);
    if(vma->vm_pgoff!=FEMU_CACHE_DATA_OFFSET/PAGE_SIZE ||
       vma->vm_end-vma->vm_start!=FEMU_CACHE_DATA_BYTES || (vma->vm_flags&VM_WRITE)) return -EINVAL;
    vm_flags_set(vma,VM_DONTEXPAND|VM_DONTDUMP);
    return remap_vmalloc_range(vma,r->data,0);
}
static int cache_release(struct inode *inode,struct file *file)
{
    struct receiver *r=file->private_data;
    mutex_lock(&mapping_lock); clear_mappings(file); mutex_unlock(&mapping_lock);
    if(receiver_bind(r,0)) {
        /* Failed STOP must not turn live DMA mappings into freed memory. */
        pr_err("femu_cache: cannot stop DMA; receive pages remain pinned\n");
        return 0;
    }
    receiver_free(r); kfree(r); return 0;
}
static const struct file_operations ops={.owner=THIS_MODULE,.open=cache_open,.unlocked_ioctl=cache_ioctl,
    .mmap=cache_mmap,.release=cache_release};
static struct miscdevice misc={.minor=MISC_DYNAMIC_MINOR,.name="femu-cache",.fops=&ops,.mode=0600};
static int __init cache_init(void)
{
    int ret;
    if(PAGE_SIZE!=FEMU_CACHE_PAGE_BYTES) return -EOPNOTSUPP;
    bdev=bdev_open_by_path(device,BLK_OPEN_READ,NULL,NULL);
    if(IS_ERR(bdev)) return PTR_ERR(bdev);
    dma_device=disk_to_dev(bdev->bdev->bd_disk);
    while(dma_device && !dev_is_pci(dma_device)) dma_device=dma_device->parent;
    if(!dma_device) { bdev_release(bdev); return -ENODEV; }
    get_device(dma_device);
    INIT_LIST_HEAD(&warm_receiver.bindings);
    ret=receiver_allocate(&warm_receiver);
    if(ret) { put_device(dma_device); bdev_release(bdev); return ret; }
    ret=misc_register(&misc);
    if(ret) { receiver_free(&warm_receiver); put_device(dma_device); bdev_release(bdev); return ret; }
    profile_dir=debugfs_create_dir("femu-cache",NULL);
    debugfs_create_file("profile",0400,profile_dir,NULL,&profile_fops);
    return 0;
}
static void __exit cache_exit(void)
{
    debugfs_remove_recursive(profile_dir); misc_deregister(&misc);
    if(!receiver_pool_busy) receiver_free(&warm_receiver);
    put_device(dma_device); bdev_release(bdev);
}
module_init(cache_init); module_exit(cache_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FEMU receive ring and clean file page-cache publication");

MODULE_IMPORT_NS(NVME_TARGET_PASSTHRU);
