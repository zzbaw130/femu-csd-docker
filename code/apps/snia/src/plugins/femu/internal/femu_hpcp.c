/* FEMU receive-ring lifecycle. Library thread, no helper process or AER. */
#include "femu_internal.h"
#include "csd-cache.h"
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <endian.h>
#include <time.h>
#include <stdio.h>
struct cache_worker {
    struct cs_cache_receiver *receiver;
    pthread_t thread;
    unsigned group;
    uint64_t calls,ioctl_ns,sleep_ns,worker_ns,worker_cpu_ns;
};
struct cs_cache_receiver {
    int fd;
    struct femu_cache_slot *slots;
    struct cache_worker workers[FEMU_CACHE_READY_QUEUES];
    unsigned started;
    bool stopping,profile,external;
    uint64_t setup_ns,bind_ns,stop_ns,join_ns;
};
static uint64_t cache_clock(clockid_t id)
{
    struct timespec t; clock_gettime(id,&t);
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static void *receive_cache(void *opaque)
{
    struct cache_worker *worker = opaque;
    struct cs_cache_receiver *r = worker->receiver;
    const struct femu_cache_ready_queue *queues =
        (const void *)((const unsigned char *)r->slots + FEMU_CACHE_READY_OFFSET);
    const struct femu_cache_ready_queue *queue = &queues[worker->group];
    unsigned head = 0;
    uint64_t begin = r->profile ? cache_clock(CLOCK_MONOTONIC) : 0;
    uint64_t cpu = r->profile ? cache_clock(CLOCK_THREAD_CPUTIME_ID) : 0;

    for (;;) {
        bool found = false;
        while (head != le32toh(__atomic_load_n(&queue->tail, __ATOMIC_ACQUIRE))) {
            unsigned slot = le32toh(__atomic_load_n(&queue->slots[head], __ATOMIC_RELAXED));
            uint64_t start = r->profile ? cache_clock(CLOCK_MONOTONIC) : 0;
            int result = ioctl(r->fd, FEMU_CACHE_RX_PUBLISH, (unsigned long)slot);
            if (r->profile) {
                worker->calls++;
                worker->ioctl_ns += cache_clock(CLOCK_MONOTONIC)-start;
            }
            if (result < 0 && (errno == EAGAIN || errno == EINTR))
                break;
            head = (head + 1) % FEMU_CACHE_READY_CAPACITY;
            found = true;
            if (result == FEMU_CACHE_HPCP_FULL &&
                ioctl(r->fd, FEMU_CACHE_RX_MODE, 0UL) == FEMU_CACHE_TARGET_HPCP)
                ioctl(r->fd, FEMU_CACHE_RX_UNBIND);
        }
        if (!found) {
            uint64_t start;

            if (__atomic_load_n(&r->stopping, __ATOMIC_ACQUIRE))
                break;
            start = r->profile ? cache_clock(CLOCK_MONOTONIC) : 0;
            usleep(50);
            if (r->profile)
                worker->sleep_ns += cache_clock(CLOCK_MONOTONIC)-start;
        }
    }
    if (r->profile) {
        worker->worker_ns = cache_clock(CLOCK_MONOTONIC)-begin;
        worker->worker_cpu_ns = cache_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    }
    return NULL;
}

static void join_cache_workers(struct cs_cache_receiver *r)
{
    __atomic_store_n(&r->stopping, true, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < r->started; i++)
        pthread_join(r->workers[i].thread, NULL);
}

CS_STATUS femu_hpcp_start(struct cs_device *d)
{
    const char *enabled=getenv("SNIA_FEMU_HPCP");
    if(__atomic_load_n(&d->cache_disabled,__ATOMIC_ACQUIRE) || d->cache || (!d->cache_external && (!enabled || strcmp(enabled,"1")))) return CS_SUCCESS;
    struct cs_cache_receiver *r=calloc(1,sizeof(*r));
    if(!r) return CS_OUT_OF_RESOURCES;
    r->profile=d->profile || d->cache_summary;
    uint64_t start=r->profile?cache_clock(CLOCK_MONOTONIC):0;
    r->external=d->cache_external;
    r->fd=r->external?d->cache_fd:open("/dev/femu-cache",O_RDWR|O_CLOEXEC);
    if(r->external) d->cache_fd=-1;
    r->slots=MAP_FAILED;
    if(r->fd<0) goto fail;
    if(!r->external) {
        if(ioctl(r->fd,FEMU_CACHE_RX_ADMISSION)==FEMU_CACHE_HPCP_FULL) {
            __atomic_store_n(&d->cache_disabled,true,__ATOMIC_RELEASE);
            close(r->fd); free(r); return CS_SUCCESS;
        }
        if(ioctl(r->fd,FEMU_CACHE_RX_SETUP)<0) goto fail;
    }
    int targets = ioctl(r->fd, FEMU_CACHE_RX_MODE, 0UL);
    if (targets < 0) goto fail;
    if (targets & FEMU_CACHE_TARGET_HPCP) {
        r->slots=mmap(NULL,FEMU_CACHE_DESCRIPTOR_BYTES,PROT_READ,MAP_SHARED,r->fd,0);
        if (r->slots==MAP_FAILED) goto fail;
    }
    if(r->profile) { r->setup_ns=cache_clock(CLOCK_MONOTONIC)-start; start=cache_clock(CLOCK_MONOTONIC); }
    if(ioctl(r->fd,FEMU_CACHE_RX_BIND,(unsigned long)d->graph_session)<0) goto fail;
    if(r->profile) r->bind_ns=cache_clock(CLOCK_MONOTONIC)-start;
    if (targets & FEMU_CACHE_TARGET_HPCP) {
        for (unsigned i = 0; i < FEMU_CACHE_READY_QUEUES; i++) {
            struct cache_worker *worker = &r->workers[i];
            worker->receiver = r;
            worker->group = i;
            if (pthread_create(&worker->thread, NULL, receive_cache, worker))
                goto fail;
            r->started++;
        }
    }
    d->cache=r;
    return CS_SUCCESS;
fail:
    if(r->fd>=0) ioctl(r->fd,FEMU_CACHE_RX_UNBIND);
    join_cache_workers(r);
    if(r->slots!=MAP_FAILED) munmap(r->slots,FEMU_CACHE_DESCRIPTOR_BYTES);
    if(r->fd>=0) close(r->fd);
    free(r); return CS_DEVICE_NOT_AVAILABLE;
}
CS_STATUS femu_hpcp_configure(struct cs_device *d,int fd)
{
    if(fd<FEMU_CACHE_HPCP_WORKER) return CS_INVALID_ARG;
    pthread_mutex_lock(&d->graph_lock);
    CS_STATUS status=CS_SUCCESS;
    if(fd==FEMU_CACHE_HPCP_WORKER) {
        struct cs_cache_receiver *r=d->cache;
        if(!r || !r->external) status=CS_INVALID_ARG;
        else if (ioctl(r->fd, FEMU_CACHE_RX_MODE, (unsigned long)FEMU_CACHE_TARGET_HPCP) < 0)
            status=CS_DEVICE_ERROR;
        else { r->external=false; d->cache_external=false; }
    } else if(fd==-1) {
        __atomic_store_n(&d->cache_disabled,true,__ATOMIC_RELEASE);
        if(d->cache && ioctl(d->cache->fd,FEMU_CACHE_RX_UNBIND)<0) status=CS_DEVICE_ERROR;
    } else if(d->graph_session || d->cache || d->cache_external) status=CS_HANDLE_IN_USE;
    else {
        int copy=fcntl(fd,F_DUPFD_CLOEXEC,0);
        if(copy<0) status=CS_INVALID_ARG;
        else { d->cache_fd=copy; d->cache_external=true; }
    }
    pthread_mutex_unlock(&d->graph_lock);
    return status;
}
void femu_hpcp_stop(struct cs_device *d)
{
    struct cs_cache_receiver *r=d->cache;
    __atomic_store_n(&d->cache_disabled,false,__ATOMIC_RELEASE);
    if(!r) {
        if(d->cache_external && d->cache_fd>=0) close(d->cache_fd);
        d->cache_external=false; d->cache_fd=-1;
        return;
    }
    /* STOP waits out software DMA and cancels pending publications. The
     * module also performs STOP on fd release, including abnormal exit. */
    uint64_t start=r->profile?cache_clock(CLOCK_MONOTONIC):0;
    ioctl(r->fd,FEMU_CACHE_RX_UNBIND);
    if(r->profile) { r->stop_ns=cache_clock(CLOCK_MONOTONIC)-start; start=cache_clock(CLOCK_MONOTONIC); }
    join_cache_workers(r);
    if(r->profile) {
        r->join_ns=cache_clock(CLOCK_MONOTONIC)-start;
        uint64_t calls=0, ioctl_ns=0, sleep_ns=0, worker_ns=0, worker_cpu_ns=0;
        for (unsigned i=0; i<r->started; i++) {
            const struct cache_worker *worker = &r->workers[i];
            calls += worker->calls; ioctl_ns += worker->ioctl_ns;
            sleep_ns += worker->sleep_ns; worker_ns += worker->worker_ns;
            worker_cpu_ns += worker->worker_cpu_ns;
            if (d->cache_summary)
                fprintf(stderr, "CACHE_RX_WORKER worker=%u calls=%llu ioctl_ns=%llu sleep_ns=%llu window_ns=%llu cpu_ns=%llu\n",
                        i, (unsigned long long)worker->calls, (unsigned long long)worker->ioctl_ns,
                        (unsigned long long)worker->sleep_ns, (unsigned long long)worker->worker_ns,
                        (unsigned long long)worker->worker_cpu_ns);
        }
        fprintf(stderr,"PROFILE CACHE_RX setup_ns=%llu bind_ns=%llu stop_ns=%llu join_ns=%llu calls=%llu ioctl_ns=%llu sleep_ns=%llu worker_ns=%llu worker_cpu_ns=%llu\n",
            (unsigned long long)r->setup_ns,(unsigned long long)r->bind_ns,(unsigned long long)r->stop_ns,
            (unsigned long long)r->join_ns,(unsigned long long)calls,(unsigned long long)ioctl_ns,
            (unsigned long long)sleep_ns,(unsigned long long)worker_ns,(unsigned long long)worker_cpu_ns);
    }
    if(r->slots!=MAP_FAILED) munmap(r->slots,FEMU_CACHE_DESCRIPTOR_BYTES);
    close(r->fd);
    free(r); d->cache=NULL;
    d->cache_external=false; d->cache_fd=-1;
}
