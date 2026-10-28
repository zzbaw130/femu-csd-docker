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
#include <linux/wait.h>
#include <linux/semaphore.h>
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
static struct workqueue_struct *workers, *fill_workers;
#define REFILL_WORKERS 4
/* Unbound workqueue max_active is per pool, not a global I/O limit. */
static struct semaphore fill_slots;
static void refill_work(struct work_struct *);
static DECLARE_WORK(refill,refill_work);
static atomic_t pending=ATOMIC_INIT(0);
static atomic_t ack_inflight=ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(ack_idle);
static atomic64_t blocks, filled, cached, skipped, errors, ack_sync;
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
    flush_workqueue(fill_workers);
    wait_event(ack_idle,!atomic_read(&ack_inflight));
    for(unsigned i=0;i<profile_count;i++) {
        const struct cache_profile *p=&profiles[i];
        seq_printf(out,"CACHE_HOST token=%llu lba=%llu aer=%llu worker=%llu fetch_begin=%llu fetch_end=%llu begin=%llu pages_ns=%llu dma_build=%llu dma_submit=%llu dma_done=%llu published=%llu ack_begin=%llu ack_end=%llu dma_calls=%u parallel=1\n",
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
struct cache_job {
    struct work_struct work;
    struct femu_cache_event event;
    struct cache_profile *profile;
    struct nvme_command ack_cmd;
};
/* Completion only records status and releases request-owned state. The
 * worker never waits for ACK CQ delivery. Limit requests independently of
 * the DMA worker pool so background metadata cannot grow without bound. */
static enum rq_end_io_ret ack_done(struct request *req,blk_status_t status)
{
    struct cache_job *job=req->end_io_data;
    if(job->profile) job->profile->ack_end=ktime_get_ns();
    if(status) atomic64_inc(&errors);
    kfree(job);
    if(atomic_dec_and_test(&ack_inflight)) wake_up_all(&ack_idle);
    module_put(THIS_MODULE);
    return RQ_END_IO_FREE;
}
static int submit_ack(struct cache_job *job,u64 valid)
{
    struct request *req;
    struct nvme_command *cmd=&job->ack_cmd;
    if(!try_module_get(THIS_MODULE)) return -EBUSY;
    if(atomic_inc_return(&ack_inflight)>REFILL_WORKERS*2) {
        atomic_dec(&ack_inflight);
        module_put(THIS_MODULE);
        return -EAGAIN;
    }
    cmd->common.opcode=FEMU_CACHE_CONTROL;
    cmd->common.nsid=cpu_to_le32(job->event.nsid);
    cmd->common.cdw10=cpu_to_le32(FEMU_CACHE_ACK);
    cmd->common.cdw12=cpu_to_le32(job->event.token);
    cmd->common.cdw13=cpu_to_le32(job->event.token>>32);
    cmd->common.cdw14=cpu_to_le32(valid);
    req=blk_mq_alloc_request(queue,REQ_OP_DRV_IN,0);
    if(IS_ERR(req)) {
        if(atomic_dec_and_test(&ack_inflight)) wake_up_all(&ack_idle);
        module_put(THIS_MODULE);
        return PTR_ERR(req);
    }
    nvme_init_request(req,cmd);
    req->end_io=ack_done; req->end_io_data=job;
    blk_execute_rq_nowait(req,false);
    return 0;
}
struct cache_page {
    struct file *file;
    u64 offset;
    struct folio *folio;
};
static void fill_event(struct work_struct *work)
{
    struct cache_job *job=container_of(work,struct cache_job,work);
    const struct femu_cache_event *event=&job->event;
    struct cache_profile *p=job->profile;
    struct cache_page pages[FEMU_CACHE_BLOCK_BYTES/PAGE_SIZE]={};
    struct mapping *map;
    u64 physical=event->lba*bdev_logical_block_size(bdev->bdev);
    u64 end=physical+event->bytes,valid=0,transferred=0;
    unsigned nr=event->bytes/PAGE_SIZE;
    down(&fill_slots);
    if(p) p->begin=ktime_get_ns();
    /* A partial file page cannot be made uptodate from partial input. */
    if((physical|event->bytes)&(PAGE_SIZE-1) || nr>ARRAY_SIZE(pages)) {
        atomic64_add(event->bytes,&skipped);
        goto acknowledge;
    }
    /* Snapshot file references only. Page allocation and NVMe waits must not
     * hold the mapping lock or serialize unrelated blocks. References keep
     * the inode/mapping alive if the registering application closes its fd. */
    mutex_lock(&mapping_lock);
    list_for_each_entry(map,&mappings,list) {
        u64 start=max(physical,map->range.lba_bytes);
        u64 stop=min(end,map->range.lba_bytes+map->range.bytes);
        if(map->range.nsid!=event->nsid) continue;
        for(u64 at=start;at<stop;at+=PAGE_SIZE) {
            unsigned i=(at-physical)/PAGE_SIZE;
            if(!pages[i].file) {
                pages[i].file=get_file(map->file);
                pages[i].offset=map->range.file_offset+at-map->range.lba_bytes;
            }
        }
    }
    mutex_unlock(&mapping_lock);
    for(unsigned i=0;i<nr;i++) {
        struct folio *f;
        u64 begin=p?ktime_get_ns():0;
        if(!pages[i].file) { atomic64_add(PAGE_SIZE,&skipped); continue; }
        f=__filemap_get_folio(pages[i].file->f_mapping,pages[i].offset>>PAGE_SHIFT,
                            FGP_LOCK|FGP_CREAT|FGP_NOWAIT,GFP_NOIO);
        if(p) p->pages_ns+=ktime_get_ns()-begin;
        if(IS_ERR(f)) { atomic64_add(PAGE_SIZE,&skipped); continue; }
        if(folio_test_uptodate(f) || folio_test_dirty(f) || folio_order(f)) {
            if(folio_test_uptodate(f) && !folio_test_dirty(f)) {
                valid+=PAGE_SIZE; atomic64_add(PAGE_SIZE,&cached);
            } else atomic64_add(PAGE_SIZE,&skipped);
            folio_unlock(f); folio_put(f);
        } else pages[i].folio=f;
    }
    for(unsigned i=0;i<nr;) {
        struct folio *folios[ARRAY_SIZE(pages)];
        unsigned first=i,count=0;
        int ret;
        if(!pages[i].folio) { i++; continue; }
        while(i<nr && pages[i].folio) folios[count++]=pages[i++].folio;
        ret=dma_folios(event,first*PAGE_SIZE,folios,count,p);
        for(unsigned j=0;j<count;j++) {
            if(!ret) folio_mark_uptodate(folios[j]);
            folio_unlock(folios[j]); folio_put(folios[j]);
        }
        if(ret) atomic64_inc(&errors);
        else { transferred+=(u64)count*PAGE_SIZE; valid+=(u64)count*PAGE_SIZE; atomic64_add((u64)count*PAGE_SIZE,&filled); }
    }
    for(unsigned i=0;i<nr;i++) if(pages[i].file) fput(pages[i].file);
acknowledge:
    atomic64_inc(&blocks);
    if(p) p->published=ktime_get_ns();
    /* The device can release fully transferred input once its CU is done.
     * Only skipped/cached/failed ranges need an explicit end-of-refill ACK. */
    if(transferred==event->bytes) {
        kfree(job);
        up(&fill_slots);
        return;
    }
    if(p) p->ack_begin=ktime_get_ns();
    if(!submit_ack(job,valid)) { up(&fill_slots); return; }
    /* At the bound, retain the original synchronous path. */
    atomic64_inc(&ack_sync);
    if(control_bytes(FEMU_CACHE_ACK,event->token,NULL,valid)) atomic64_inc(&errors);
    if(p) p->ack_end=ktime_get_ns();
    kfree(job);
    up(&fill_slots);
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
        if(control(FEMU_CACHE_FETCH,0,data)) { atomic64_inc(&errors); break; }
        u64 fetch_end=profile?ktime_get_ns():0;
        for(unsigned i=0;i<min_t(unsigned,log->count,FEMU_CACHE_LOG_COUNT);i++) {
            struct cache_profile *p=NULL;
            struct cache_job *job=kzalloc(sizeof(*job),GFP_NOIO);
            if(!job) {
                atomic64_inc(&errors);
                control_bytes(FEMU_CACHE_ACK,log->events[i].token,NULL,0);
                continue;
            }
            if(profile && profile_count<PROFILE_MAX) {
                p=&profiles[profile_count];
                *p=(struct cache_profile){.token=log->events[i].token,.lba=log->events[i].lba,.aer=aer_ns,
                    .worker=worker_ns,.fetch_begin=fetch_begin,.fetch_end=fetch_end};
            }
            job->event=log->events[i]; job->profile=p;
            if(p) profile_count++;
            INIT_WORK(&job->work,fill_event);
            queue_work(fill_workers,&job->work);
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
        pr_info("femu_cache blocks=%llu filled=%llu cached=%llu skipped=%llu errors=%llu ack_sync=%llu\n",
                atomic64_read(&blocks),atomic64_read(&filled),atomic64_read(&cached),
                atomic64_read(&skipped),atomic64_read(&errors),atomic64_read(&ack_sync));
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
    sema_init(&fill_slots,REFILL_WORKERS);
    bdev=bdev_open_by_path(device,BLK_OPEN_READ,NULL,NULL);
    if(IS_ERR(bdev)) return PTR_ERR(bdev);
    queue=bdev_get_queue(bdev->bdev);
    workers=alloc_ordered_workqueue("femu-cache",WQ_MEM_RECLAIM);
    if(!workers) { ret=-ENOMEM; goto release; }
    fill_workers=alloc_workqueue("femu-cache-dma",WQ_UNBOUND|WQ_MEM_RECLAIM,REFILL_WORKERS);
    if(!fill_workers) { ret=-ENOMEM; goto destroy; }
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
destroy:
    if(fill_workers) destroy_workqueue(fill_workers);
    destroy_workqueue(workers);
release: bdev_release(bdev); return ret;
}
static void __exit cache_exit(void)
{
    debugfs_remove_recursive(profile_dir);
    control(FEMU_CACHE_DISABLE,0,NULL);
    unregister_tracepoint_module_notifier(&module_events);
    if(aer_trace) tracepoint_probe_unregister(aer_trace,aer,NULL);
    tracepoint_synchronize_unregister();
    flush_workqueue(workers);
    flush_workqueue(fill_workers);
    wait_event(ack_idle,!atomic_read(&ack_inflight));
    destroy_workqueue(fill_workers);
    destroy_workqueue(workers);
    clear_mappings(NULL); misc_deregister(&misc); bdev_release(bdev);
}
module_init(cache_init); module_exit(cache_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FEMU direct FDM to file page-cache refill");
MODULE_IMPORT_NS(NVME_TARGET_PASSTHRU);
