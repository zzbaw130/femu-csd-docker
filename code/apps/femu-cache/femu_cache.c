// SPDX-License-Identifier: GPL-2.0
/* Linux 6.8: AER-driven direct FDM DMA into regular-file page-cache folios.
 * Uses exported NVMe/block APIs; no private nvme_ctrl layout or user proxy. */
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/nvme.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/tracepoint.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include "csd-cache.h"
struct nvme_ctrl;
extern void nvme_init_request(struct request *, struct nvme_command *);
extern int nvme_execute_rq(struct request *, bool);
extern int nvme_submit_sync_cmd(struct request_queue *, struct nvme_command *, void *, unsigned);
static char *device="/dev/nvme0n1";
module_param(device,charp,0444);
struct mapping {
    struct list_head list;
    struct file *file, *owner;
    struct femu_cache_mapping range;
};
static LIST_HEAD(mappings);
static DEFINE_MUTEX(mapping_lock);
static struct bdev_handle *bdev;
static struct request_queue *queue;
static struct tracepoint *aer_trace;
static struct workqueue_struct *workers;
static void refill_work(struct work_struct *);
static DECLARE_WORK(refill,refill_work);
static atomic_t pending=ATOMIC_INIT(0);
static u64 blocks, filled, cached, skipped, errors;
/* Diagnostic records stay in memory; export only after the measured request.
 * All fields below use Guest ktime. Device timestamps are correlated offline
 * using the causal bounds of DMA and ACK request/response pairs. */
static bool profile;
module_param(profile,bool,0644);
#define PROFILE_MAX 4096
struct cache_profile {
    u64 token, lba, aer, worker, fetch_begin, fetch_end, begin, pages_ns;
    u64 dma_build, dma_submit, dma_done, published, ack_begin, ack_end;
    unsigned dma_calls;
};
static struct cache_profile profiles[PROFILE_MAX];
static unsigned profile_count;
static atomic64_t last_aer=ATOMIC64_INIT(0);
static struct dentry *profile_dir;
static int profile_show(struct seq_file *out,void *unused)
{
    flush_workqueue(workers);
    for(unsigned i=0;i<profile_count;i++) {
        const struct cache_profile *p=&profiles[i];
        seq_printf(out,"CACHE_HOST token=%llu lba=%llu aer=%llu worker=%llu fetch_begin=%llu fetch_end=%llu begin=%llu pages_ns=%llu dma_build=%llu dma_submit=%llu dma_done=%llu published=%llu ack_begin=%llu ack_end=%llu dma_calls=%u\n",
            p->token,p->lba,p->aer,p->worker,p->fetch_begin,p->fetch_end,p->begin,p->pages_ns,
            p->dma_build,p->dma_submit,p->dma_done,p->published,p->ack_begin,p->ack_end,p->dma_calls);
    }
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(profile);

static int control_bytes(unsigned action,u64 token,void *data,unsigned valid)
{
    struct nvme_command cmd={};
    cmd.common.opcode=FEMU_CACHE_CONTROL;
    cmd.common.nsid=cpu_to_le32(1);
    cmd.common.cdw10=cpu_to_le32(action);
    cmd.common.cdw12=cpu_to_le32(token);
    cmd.common.cdw13=cpu_to_le32(token>>32);
    cmd.common.cdw14=cpu_to_le32(valid);
    return nvme_submit_sync_cmd(queue,&cmd,data,data?FEMU_CACHE_LOG_BYTES:0);
}
static int control(unsigned action,u64 token,void *data)
{ return control_bytes(action,token,data,0); }
static int dma_folios(const struct femu_cache_event *event,unsigned offset,
                      struct folio **folios,unsigned count,struct cache_profile *p)
{
    struct nvme_command cmd={};
    struct request *req;
    struct bio *bio;
    int ret;
    if(p) { p->dma_build=ktime_get_ns(); p->dma_calls++; }
    cmd.common.opcode=FEMU_CACHE_READ;
    cmd.common.nsid=cpu_to_le32(event->nsid);
    cmd.common.cdw10=cpu_to_le32(event->token);
    cmd.common.cdw11=cpu_to_le32(event->token>>32);
    cmd.common.cdw12=cpu_to_le32(offset);
    cmd.common.cdw13=cpu_to_le32(count*PAGE_SIZE);
    req=blk_mq_alloc_request(queue,REQ_OP_DRV_IN,0);
    if(IS_ERR(req)) return PTR_ERR(req);
    nvme_init_request(req,&cmd);
    bio=bio_alloc(bdev->bdev,count,REQ_OP_DRV_IN,GFP_NOIO);
    if(!bio) { blk_mq_free_request(req); return -ENOMEM; }
    for(unsigned i=0;i<count;i++)
        if(bio_add_page(bio,folio_page(folios[i],0),PAGE_SIZE,0)!=PAGE_SIZE) {
            bio_put(bio); blk_mq_free_request(req); return -EIO;
        }
    ret=blk_rq_append_bio(req,bio);
    if(!ret) {
        if(p) p->dma_submit=ktime_get_ns();
        ret=nvme_execute_rq(req,false);
        if(p) p->dma_done=ktime_get_ns();
    }
    bio_put(bio);
    blk_mq_free_request(req);
    return ret;
}
static void fill_event(const struct femu_cache_event *event,struct cache_profile *p)
{
    struct mapping *map;
    u64 physical=event->lba*bdev_logical_block_size(bdev->bdev);
    u64 end=physical+event->bytes;
    u64 before=filled+cached;
    if(p) p->begin=ktime_get_ns();
    mutex_lock(&mapping_lock);
    list_for_each_entry(map,&mappings,list) {
        u64 start=max(physical,map->range.lba_bytes);
        u64 stop=min(end,map->range.lba_bytes+map->range.bytes);
        if(map->range.nsid!=event->nsid || start>=stop) continue;
        while(start<stop) {
            struct folio *folios[FEMU_CACHE_BLOCK_BYTES/PAGE_SIZE];
            unsigned count=0,offset=start-physical;
            u64 file_offset=map->range.file_offset+start-map->range.lba_bytes;
            while(start<stop && count<ARRAY_SIZE(folios)) {
                u64 page_begin=p?ktime_get_ns():0;
                struct folio *f=__filemap_get_folio(map->file->f_mapping,file_offset>>PAGE_SHIFT,
                    FGP_LOCK|FGP_CREAT|FGP_NOWAIT,GFP_NOIO);
                if(p) p->pages_ns+=ktime_get_ns()-page_begin;
                if(IS_ERR(f)) { if(!count) { skipped+=PAGE_SIZE; start+=PAGE_SIZE; } break; }
                if(folio_test_uptodate(f) || folio_test_dirty(f) || folio_order(f)) {
                    bool valid=folio_test_uptodate(f);
                    folio_unlock(f); folio_put(f);
                    if(!count) { if(valid) cached+=PAGE_SIZE; else skipped+=PAGE_SIZE; start+=PAGE_SIZE; }
                    break;
                }
                folios[count++]=f; start+=PAGE_SIZE; file_offset+=PAGE_SIZE;
            }
            if(count) {
                int ret=dma_folios(event,offset,folios,count,p);
                for(unsigned i=0;i<count;i++) {
                    if(!ret) folio_mark_uptodate(folios[i]);
                    folio_unlock(folios[i]); folio_put(folios[i]);
                }
                if(ret) errors++; else filled+=(u64)count*PAGE_SIZE;
            }
        }
    }
    mutex_unlock(&mapping_lock);
    blocks++;
    if(p) p->published=ktime_get_ns();
    /* All cache folios are now visible. Device timestamps ACK reception in
     * its own clock, an upper bound on page-cache publication, not pure DMA. */
    if(p) p->ack_begin=ktime_get_ns();
    if(control_bytes(FEMU_CACHE_ACK,event->token,NULL,filled+cached-before)) errors++;
    if(p) p->ack_end=ktime_get_ns();
}
static void refill_work(struct work_struct *work)
{
    u64 worker_ns=profile?ktime_get_ns():0;
    u64 aer_ns=profile?atomic64_read(&last_aer):0;
    void *data=kzalloc(FEMU_CACHE_LOG_BYTES,GFP_KERNEL);
    if(!data) return;
    do {
        struct femu_cache_log *log=data;
        atomic_set(&pending,0);
        memset(data,0,FEMU_CACHE_LOG_BYTES);
        u64 fetch_begin=profile?ktime_get_ns():0;
        if(control(FEMU_CACHE_FETCH,0,data)) { errors++; break; }
        u64 fetch_end=profile?ktime_get_ns():0;
        for(unsigned i=0;i<min_t(unsigned,log->count,FEMU_CACHE_LOG_COUNT);i++) {
            struct cache_profile *p=NULL;
            if(profile && profile_count<PROFILE_MAX) {
                p=&profiles[profile_count];
                *p=(struct cache_profile){.token=log->events[i].token,.lba=log->events[i].lba,.aer=aer_ns,
                    .worker=worker_ns,.fetch_begin=fetch_begin,.fetch_end=fetch_end};
            }
            fill_event(&log->events[i],p);
            if(p) profile_count++;
        }
        if(log->count) atomic_set(&pending,1);
    } while(atomic_xchg(&pending,0));
    kfree(data);
}
static void aer(void *unused,struct nvme_ctrl *ctrl,u32 result)
{
    if((result&0x00ffffff)!=FEMU_CACHE_AER) return;
    if(profile) atomic64_set(&last_aer,ktime_get_ns());
    atomic_set(&pending,1);
    queue_work(workers,&refill);
}
static void find_aer(struct tracepoint *tp,void *unused)
{ if(!strcmp(tp->name,"nvme_async_event")) aer_trace=tp; }
/* NVMe is a module on the Guest; the notifier also visits already loaded
 * modules, unlike for_each_kernel_tracepoint (built-in tracepoints only). */
static int module_trace(struct notifier_block *nb,unsigned long action,void *data)
{
    struct tp_module *tm=data;
    for(unsigned i=0;i<tm->mod->num_tracepoints;i++) {
        struct tracepoint *tp=tracepoint_ptr_deref(&tm->mod->tracepoints_ptrs[i]);
        if(strcmp(tp->name,"nvme_async_event")) continue;
        if(action==MODULE_STATE_COMING && !aer_trace) {
            if(!tracepoint_probe_register(tp,aer,NULL)) aer_trace=tp;
        } else if(action==MODULE_STATE_GOING && aer_trace==tp) {
            tracepoint_probe_unregister(tp,aer,NULL); aer_trace=NULL;
        }
    }
    return NOTIFY_OK;
}
static struct notifier_block module_events={.notifier_call=module_trace};
static void clear_mappings(struct file *owner)
{
    struct mapping *m,*next;
    list_for_each_entry_safe(m,next,&mappings,list) {
        if(!owner || m->owner==owner) {
            list_del(&m->list); fput(m->file); kfree(m);
        }
    }
}
static long cache_ioctl(struct file *file,unsigned cmd,unsigned long arg)
{
    struct femu_cache_mapping range;
    struct mapping *map;
    if(!capable(CAP_SYS_ADMIN)) return -EPERM;
    if(cmd==FEMU_CACHE_CLEAR) {
        mutex_lock(&mapping_lock);
        clear_mappings(file);
        if(list_empty(&mappings)) control(FEMU_CACHE_DISABLE,0,NULL);
        mutex_unlock(&mapping_lock);
        pr_info("femu_cache blocks=%llu filled=%llu cached=%llu skipped=%llu errors=%llu\n",
                blocks,filled,cached,skipped,errors);
        return 0;
    }
    if(cmd!=FEMU_CACHE_REGISTER) return -ENOTTY;
    if(copy_from_user(&range,(void __user *)arg,sizeof(range))) return -EFAULT;
    if(range.nsid!=1 || !range.bytes || range.lba_bytes>U64_MAX-range.bytes || (range.lba_bytes|range.file_offset|range.bytes)&(PAGE_SIZE-1))
        return -EINVAL;
    map=kzalloc(sizeof(*map),GFP_KERNEL);
    if(!map) return -ENOMEM;
    map->file=fget(range.fd);
    if(!map->file || !S_ISREG(file_inode(map->file)->i_mode) ||
       (!file_inode(map->file)->i_sb->s_bdev ||
        file_inode(map->file)->i_sb->s_bdev->bd_disk!=bdev->bdev->bd_disk) ||
       range.file_offset>i_size_read(file_inode(map->file)) ||
       range.bytes>i_size_read(file_inode(map->file))-range.file_offset) {
        if(map->file) fput(map->file);
        kfree(map); return -EINVAL;
    }
    map->range=range; map->owner=file;
    mutex_lock(&mapping_lock);
    list_add_tail(&map->list,&mappings);
    int ret=control(FEMU_CACHE_ENABLE,0,NULL)?-EIO:0;
    if(ret) { list_del(&map->list); fput(map->file); kfree(map); }
    mutex_unlock(&mapping_lock);
    return ret;
}
static int cache_release(struct inode *inode,struct file *file)
{
    mutex_lock(&mapping_lock);
    clear_mappings(file);
    if(list_empty(&mappings)) control(FEMU_CACHE_DISABLE,0,NULL);
    mutex_unlock(&mapping_lock);
    return 0;
}
static const struct file_operations ops={.owner=THIS_MODULE,.unlocked_ioctl=cache_ioctl,
    .release=cache_release};
static struct miscdevice misc={.minor=MISC_DYNAMIC_MINOR,.name="femu-cache",.fops=&ops,.mode=0600};
static int __init cache_init(void)
{
    int ret;
    if(PAGE_SIZE!=4096) return -EOPNOTSUPP;
    bdev=bdev_open_by_path(device,BLK_OPEN_READ,NULL,NULL);
    if(IS_ERR(bdev)) return PTR_ERR(bdev);
    queue=bdev_get_queue(bdev->bdev);
    workers=alloc_ordered_workqueue("femu-cache",WQ_MEM_RECLAIM);
    if(!workers) { ret=-ENOMEM; goto release; }
    for_each_kernel_tracepoint(find_aer,NULL);
    if(aer_trace) {
        ret=tracepoint_probe_register(aer_trace,aer,NULL); if(ret) goto destroy;
    }
    ret=register_tracepoint_module_notifier(&module_events);
    if(ret) goto destroy;
    if(!aer_trace) { ret=-ENOENT; unregister_tracepoint_module_notifier(&module_events); goto destroy; }
    ret=misc_register(&misc);
    if(ret) { unregister_tracepoint_module_notifier(&module_events); if(aer_trace) tracepoint_probe_unregister(aer_trace,aer,NULL); tracepoint_synchronize_unregister(); goto destroy; }
    profile_dir=debugfs_create_dir("femu-cache",NULL);
    debugfs_create_file("profile",0400,profile_dir,NULL,&profile_fops);
    return 0;
destroy: destroy_workqueue(workers);
release: bdev_release(bdev); return ret;
}
static void __exit cache_exit(void)
{
    debugfs_remove_recursive(profile_dir);
    control(FEMU_CACHE_DISABLE,0,NULL);
    unregister_tracepoint_module_notifier(&module_events);
    if(aer_trace) tracepoint_probe_unregister(aer_trace,aer,NULL);
    tracepoint_synchronize_unregister();
    flush_workqueue(workers); destroy_workqueue(workers);
    clear_mappings(NULL); misc_deregister(&misc); bdev_release(bdev);
}
module_init(cache_init); module_exit(cache_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FEMU direct FDM to file page-cache refill");
MODULE_IMPORT_NS(NVME_TARGET_PASSTHRU);
