/* FEMU receive-ring lifecycle. Library thread, no helper process or AER. */
#include "femu_internal.h"
#include "csd-cache.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <endian.h>
#include <time.h>
#include <stdio.h>
struct cs_cache_receiver {
    int fd;
    struct femu_cache_slot *slots;
    pthread_t worker;
    bool stopping,profile;
    uint64_t calls,ioctl_ns,sleep_ns,worker_ns,worker_cpu_ns;
    uint64_t setup_ns,bind_ns,stop_ns,join_ns;
};
static uint64_t cache_clock(clockid_t id)
{
    struct timespec t; clock_gettime(id,&t);
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static void *receive_cache(void *opaque)
{
    struct cs_cache_receiver *r=opaque;
    uint64_t begin=r->profile?cache_clock(CLOCK_MONOTONIC):0;
    uint64_t cpu=r->profile?cache_clock(CLOCK_THREAD_CPUTIME_ID):0;
    for(;;) {
        bool found=false;
        for(unsigned i=0;i<FEMU_CACHE_RX_SLOTS;i++) {
            if(le64toh(__atomic_load_n(&r->slots[i].ready,__ATOMIC_ACQUIRE))!=1) continue;
            found=true;
            uint64_t start=r->profile?cache_clock(CLOCK_MONOTONIC):0;
            if(ioctl(r->fd,FEMU_CACHE_RX_PUBLISH,(unsigned long)i)<0) {
                /* The module owns page validation and always returns the slot. */
                __atomic_store_n(&r->slots[i].ready,htole64(0),__ATOMIC_RELEASE);
            }
            if(r->profile) { r->calls++; r->ioctl_ns+=cache_clock(CLOCK_MONOTONIC)-start; }
        }
        if(!found) {
            if(__atomic_load_n(&r->stopping,__ATOMIC_ACQUIRE)) break;
            uint64_t start=r->profile?cache_clock(CLOCK_MONOTONIC):0;
            usleep(50);
            if(r->profile) r->sleep_ns+=cache_clock(CLOCK_MONOTONIC)-start;
        }
    }
    if(r->profile) {
        r->worker_ns=cache_clock(CLOCK_MONOTONIC)-begin;
        r->worker_cpu_ns=cache_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    }
    return NULL;
}
CS_STATUS femu_cache_start(struct cs_device *d)
{
    const char *enabled=getenv("SNIA_FEMU_CACHE_REFILL");
    if(d->cache || !enabled || strcmp(enabled,"1")) return CS_SUCCESS;
    struct cs_cache_receiver *r=calloc(1,sizeof(*r));
    if(!r) return CS_OUT_OF_RESOURCES;
    r->profile=d->profile;
    uint64_t start=r->profile?cache_clock(CLOCK_MONOTONIC):0;
    r->fd=open("/dev/femu-cache",O_RDWR|O_CLOEXEC);
    r->slots=MAP_FAILED;
    if(r->fd<0 || ioctl(r->fd,FEMU_CACHE_RX_SETUP)<0) goto fail;
    r->slots=mmap(NULL,FEMU_CACHE_PAGE_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,r->fd,0);
    if(r->slots==MAP_FAILED) goto fail;
    if(r->profile) { r->setup_ns=cache_clock(CLOCK_MONOTONIC)-start; start=cache_clock(CLOCK_MONOTONIC); }
    if(ioctl(r->fd,FEMU_CACHE_RX_BIND,(unsigned long)d->graph_session)<0) goto fail;
    if(r->profile) r->bind_ns=cache_clock(CLOCK_MONOTONIC)-start;
    if(pthread_create(&r->worker,NULL,receive_cache,r)) goto fail;
    d->cache=r;
    return CS_SUCCESS;
fail:
    if(r->fd>=0) ioctl(r->fd,FEMU_CACHE_RX_UNBIND);
    if(r->slots!=MAP_FAILED) munmap(r->slots,FEMU_CACHE_PAGE_BYTES);
    if(r->fd>=0) close(r->fd);
    free(r); return CS_DEVICE_NOT_AVAILABLE;
}
void femu_cache_stop(struct cs_device *d)
{
    struct cs_cache_receiver *r=d->cache;
    if(!r) return;
    /* STOP waits out software DMA and cancels pending publications. The
     * module also performs STOP on fd release, including abnormal exit. */
    uint64_t start=r->profile?cache_clock(CLOCK_MONOTONIC):0;
    ioctl(r->fd,FEMU_CACHE_RX_UNBIND);
    if(r->profile) { r->stop_ns=cache_clock(CLOCK_MONOTONIC)-start; start=cache_clock(CLOCK_MONOTONIC); }
    __atomic_store_n(&r->stopping,true,__ATOMIC_RELEASE);
    pthread_join(r->worker,NULL);
    if(r->profile) {
        r->join_ns=cache_clock(CLOCK_MONOTONIC)-start;
        fprintf(stderr,"PROFILE CACHE_RX setup_ns=%llu bind_ns=%llu stop_ns=%llu join_ns=%llu calls=%llu ioctl_ns=%llu sleep_ns=%llu worker_ns=%llu worker_cpu_ns=%llu\n",
            (unsigned long long)r->setup_ns,(unsigned long long)r->bind_ns,(unsigned long long)r->stop_ns,
            (unsigned long long)r->join_ns,(unsigned long long)r->calls,(unsigned long long)r->ioctl_ns,
            (unsigned long long)r->sleep_ns,(unsigned long long)r->worker_ns,(unsigned long long)r->worker_cpu_ns);
    }
    munmap(r->slots,FEMU_CACHE_PAGE_BYTES); close(r->fd);
    free(r); d->cache=NULL;
}
