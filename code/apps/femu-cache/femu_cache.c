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
struct receiver {
    struct femu_cache_slot *slots;
    dma_addr_t descriptor_dma;
    void *data;
    dma_addr_t pages[FEMU_CACHE_TOTAL_PAGES];
    unsigned char *resident;
    dma_addr_t resident_dma;
    size_t resident_bytes;
    unsigned mapped,session;
    struct mutex lock;
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
        memset(r->slots,0,PAGE_SIZE);
        mutex_lock(&receiver_pool_lock);
        receiver_pool_busy=false;
        mutex_unlock(&receiver_pool_lock);
        r->slots=NULL; r->data=NULL; r->mapped=0; r->pooled=false;
        r->resident=NULL;
        return;
    }
    for(unsigned i=0;i<r->mapped;i++)
        dma_unmap_page(dma_device,r->pages[i],
                       PAGE_SIZE,DMA_FROM_DEVICE);
    vfree(r->data);
    if(r->slots) dma_free_coherent(dma_device,PAGE_SIZE,r->slots,r->descriptor_dma);
    if(r->resident) dma_free_coherent(dma_device,r->resident_bytes,r->resident,r->resident_dma);
    r->slots=NULL; r->data=NULL; r->mapped=0;
    r->resident=NULL;
}
static int receiver_allocate(struct receiver *r)
{
    int ret=-ENOMEM;
    if(r->slots) return -EBUSY;
    r->slots=dma_alloc_coherent(dma_device,PAGE_SIZE,&r->descriptor_dma,GFP_KERNEL);
    r->data=vmalloc_user(FEMU_CACHE_DATA_BYTES);
    r->resident_bytes=DIV_ROUND_UP_ULL(bdev_nr_bytes(bdev->bdev),PAGE_SIZE*8);
    r->resident=dma_alloc_coherent(dma_device,r->resident_bytes,&r->resident_dma,GFP_KERNEL);
    if(!r->slots || !r->data || !r->resident) goto fail;
    memset(r->slots,0,PAGE_SIZE);
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
        memcpy(r->pages,warm_receiver.pages,sizeof(r->pages)); r->pooled=true;
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
        info=kzalloc(sizeof(*info),GFP_KERNEL);
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
    kfree(info);
    if(!ret) r->session=session;
    return ret>0?-EIO:ret;
}
static int acquire_slot(struct receiver *r,unsigned index)
{
    if(!r->slots || index>=FEMU_CACHE_RX_SLOTS) return -EINVAL;
    struct femu_cache_slot *slot=&r->slots[index];
    if(le64_to_cpu(smp_load_acquire(&slot->ready))!=1) return -EAGAIN;
    dma_rmb();
    u64 bytes=le64_to_cpu(slot->bytes);
    if(le32_to_cpu(slot->error) || !bytes || bytes>FEMU_CACHE_SLOT_BYTES(index) || bytes%PAGE_SIZE) return -EINVAL;
    for(unsigned p=0;p<bytes/PAGE_SIZE;p++)
        dma_sync_single_for_cpu(dma_device,r->pages[FEMU_CACHE_SLOT_OFFSET(index)/PAGE_SIZE+p],PAGE_SIZE,DMA_FROM_DEVICE);
    return 0;
}
static int publish_slot(struct receiver *r,unsigned index,bool hpcp,bool acquired,bool shared_done)
{
    if(!r->slots || index>=FEMU_CACHE_RX_SLOTS) return -EINVAL;
    struct femu_cache_slot *slot=&r->slots[index];
    if(le64_to_cpu(smp_load_acquire(&slot->ready))!=1) return -EAGAIN;
    dma_rmb();
    u64 bytes=le64_to_cpu(slot->bytes),lba=le64_to_cpu(slot->lba);
    bool timing=READ_ONCE(profile),full=false;
    u64 begin=timing?ktime_get_ns():0,mark=begin;
    u64 lookup=0,folio_time=0,copy_time=0;
    if(le32_to_cpu(slot->error) || !bytes || bytes>FEMU_CACHE_SLOT_BYTES(index) || bytes%PAGE_SIZE ||
       lba>U64_MAX/bdev_logical_block_size(bdev->bdev)) {
        atomic64_inc(&errors); goto done;
    }
    unsigned count=bytes/PAGE_SIZE;
    if(!acquired) for(unsigned p=0;p<count;p++)
        dma_sync_single_for_cpu(dma_device,r->pages[FEMU_CACHE_SLOT_OFFSET(index)/PAGE_SIZE+p],PAGE_SIZE,DMA_FROM_DEVICE);
    if(timing) { u64 now=ktime_get_ns(); atomic64_add(now-mark,&sync_cpu_ns); mark=now; }
    u64 physical=lba*bdev_logical_block_size(bdev->bdev);
    /* MemAvailable includes reclaimable file cache and would allow population
     * to recycle its own earlier pages. Reserve one eighth of RAM as genuinely
     * free pages; optional allocation must never initiate direct reclaim. */
    bool admit=global_zone_page_state(NR_FREE_PAGES)>totalram_pages()/8;
    full=hpcp && !admit;
    struct mapping *binding=NULL;
    for(unsigned p=0;p<count;p++) {
        if(timing) mark=ktime_get_ns();
        struct file *file=NULL;
        u64 offset=0,at=physical+(u64)p*PAGE_SIZE;
        /* One LOAD normally remains inside one registered extent. Reuse
         * that mapping instead of searching every extent for every 4KiB. */
        if(!binding || at<binding->range.lba_bytes || at-binding->range.lba_bytes>=binding->range.bytes) {
            struct mapping *map;
            binding=NULL;
            list_for_each_entry(map,&r->bindings,list) {
                if(map->range.nsid==le32_to_cpu(slot->nsid) && at>=map->range.lba_bytes &&
                   at-map->range.lba_bytes<map->range.bytes) { binding=map; break; }
            }
        }
        if(binding) { file=binding->file; offset=binding->range.file_offset+at-binding->range.lba_bytes; }
        bool shared=shared_done || (binding && !(binding->range.flags&FEMU_CACHE_NEEDS_DCP));
        if(!hpcp) {
            if(shared) r->resident[at/PAGE_SIZE/8]|=1U<<(at/PAGE_SIZE%8);
            continue;
        }
        if(timing) { u64 now=ktime_get_ns(); lookup+=now-mark; mark=now; }
        if(!file) { atomic64_add(PAGE_SIZE,&skipped); continue; }
        struct folio *folio=__filemap_get_folio(file->f_mapping,offset>>PAGE_SHIFT,
                                               FGP_LOCK|FGP_NOWAIT|(admit?FGP_CREAT:0),GFP_NOWAIT);
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
                if(shared) r->resident[at/PAGE_SIZE/8]|=1U<<(at/PAGE_SIZE%8);
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
            if(shared) r->resident[at/PAGE_SIZE/8]|=1U<<(at/PAGE_SIZE%8);
        }
        folio_unlock(folio); folio_put(folio);
    }
    if(timing) mark=ktime_get_ns();
    for(unsigned p=0;p<count;p++)
        dma_sync_single_for_device(dma_device,r->pages[FEMU_CACHE_SLOT_OFFSET(index)/PAGE_SIZE+p],PAGE_SIZE,DMA_FROM_DEVICE);
    if(timing) {
        atomic64_add(ktime_get_ns()-mark,&sync_device_ns);
        atomic64_add(lookup,&lookup_ns); atomic64_add(folio_time,&folio_ns);
        atomic64_add(copy_time,&copy_ns);
    }
done:
    if(timing) atomic64_add(ktime_get_ns()-begin,&publish_ns);
    atomic64_inc(&blocks);
    dma_wmb();
    smp_store_release(&slot->ready,cpu_to_le64(0));
    return full?FEMU_CACHE_HPCP_FULL:0;
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
    if(cmd==FEMU_CACHE_RX_SETUP || cmd==FEMU_CACHE_RX_PUBLISH || cmd==FEMU_CACHE_RX_ACQUIRE || cmd==FEMU_CACHE_RX_RELEASE ||
       cmd==FEMU_CACHE_RX_BIND || cmd==FEMU_CACHE_RX_UNBIND) {
        mutex_lock(&r->lock);
        int ret=cmd==FEMU_CACHE_RX_SETUP?receiver_setup(r):
            cmd==FEMU_CACHE_RX_PUBLISH?publish_slot(r,arg,true,false,false):
            cmd==FEMU_CACHE_RX_ACQUIRE?acquire_slot(r,arg):
            cmd==FEMU_CACHE_RX_RELEASE?publish_slot(r,arg&~(FEMU_CACHE_RELEASE_HPCP|FEMU_CACHE_RELEASE_DCP),(arg&FEMU_CACHE_RELEASE_HPCP)!=0,true,(arg&FEMU_CACHE_RELEASE_DCP)!=0):
            receiver_bind(r,cmd==FEMU_CACHE_RX_BIND?arg:0);
        mutex_unlock(&r->lock); return ret;
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
    mutex_init(&r->lock); INIT_LIST_HEAD(&r->bindings); file->private_data=r; return 0;
}
static int cache_mmap(struct file *file,struct vm_area_struct *vma)
{
    struct receiver *r=file->private_data;
    if(!r->slots) return -EINVAL;
    if(!vma->vm_pgoff && vma->vm_end-vma->vm_start==PAGE_SIZE)
        return dma_mmap_coherent(dma_device,vma,r->slots,r->descriptor_dma,PAGE_SIZE);
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
