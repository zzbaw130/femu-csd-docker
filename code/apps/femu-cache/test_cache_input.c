/* Guest microbenchmark: same file extents, internal LOAD versus warm cache input. */
#include "cs.h"
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define FILE_BYTES (1UL << 30)
#define MAX_EXTENTS 4096
static size_t copy_bytes;
static uint64_t now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static void check(CS_STATUS status)
{
    if(status!=CS_SUCCESS) { fprintf(stderr,"SNIA status=%d\n",status); exit(1); }
}
struct lane {
    int fd,mode,error;
    unsigned number,lanes,lba,nsid;
    size_t chunk,last;
    uint64_t partition,submit_ns,read_ns;
    CS_DEV_HANDLE dev;
    CS_MEM_HANDLE fdm;
    struct fiemap *map;
    const char *mapped;
    char *buffer;
    CsStorageRequest *load;
    pthread_barrier_t *start;
};
static void *transfer(void *opaque)
{
    struct lane *l=opaque;
    l->submit_ns=l->read_ns=0; l->error=0;
    pthread_barrier_wait(l->start);
    for(size_t at=l->number*l->chunk;at<FILE_BYTES;at+=l->lanes*l->chunk) {
        CS_STATUS status;
        if(l->mode==0) {
            CsBlockIo *io=&l->load->u.BlockIo;
            io->NumRanges=0;
            size_t cursor=at;
            for(unsigned e=0;e<l->map->fm_mapped_extents && cursor<at+l->chunk;e++) {
                struct fiemap_extent *x=&l->map->fm_extents[e];
                if(cursor>=x->fe_logical+x->fe_length) continue;
                if(cursor<x->fe_logical) break;
                size_t n=x->fe_logical+x->fe_length-cursor;
                if(n>at+l->chunk-cursor) n=at+l->chunk-cursor;
                CSBlockRange *r=&io->Range[io->NumRanges++];
                *r=(CSBlockRange){.NamespaceId=l->nsid,
                    .StartLba=l->partition+(x->fe_physical+cursor-x->fe_logical)/l->lba,
                    .NumBlocks=n/l->lba};
                cursor+=n;
            }
            if(cursor!=at+l->chunk) { l->error=1; break; }
            uint64_t begin=now();
            status=csQueueStorageRequest(l->load,NULL,NULL,NULL,NULL,NULL);
            l->submit_ns+=now()-begin;
        } else {
            void *input=(void *)(l->mapped+at);
            if(l->mode==2) {
                uint64_t begin=now();
                ssize_t n=pread(l->fd,l->buffer,l->chunk,at);
                l->read_ns+=now()-begin;
                if(n!=(ssize_t)l->chunk) { l->error=1; break; }
                input=l->buffer;
            }
            uint64_t begin=now();
            status=CS_SUCCESS;
            for(size_t off=0;off<l->chunk;off+=copy_bytes) {
                size_t n=l->chunk-off; if(n>copy_bytes) n=copy_bytes;
                CsCopyMemRequest copy={.Type=CS_COPY_TO_DEVICE,
                    .u.HostVAddress=(char *)input+off,.DevMem={l->fdm,off},.Bytes=n};
                status=csQueueCopyMemRequest(&copy,NULL,NULL,NULL,NULL,NULL);
                if(status!=CS_SUCCESS) break;
            }
            l->submit_ns+=now()-begin;
        }
        if(status!=CS_SUCCESS) { l->error=1; fprintf(stderr,"transfer mode=%d status=%d errno=%d (%s)\n",l->mode,status,errno,strerror(errno)); break; }
        l->last=at;
    }
    return NULL;
}
static unsigned long long number(const char *path)
{
    unsigned long long value;
    FILE *f=fopen(path,"r");
    if(!f || fscanf(f,"%llu",&value)!=1) { perror(path); exit(1); }
    fclose(f); return value;
}
int main(int argc,char **argv)
{
    if(argc!=5) { fprintf(stderr,"usage: %s file chunk-KiB lanes rounds\n",argv[0]); return 2; }
    size_t chunk=strtoul(argv[2],NULL,10)*1024;
    unsigned count=strtoul(argv[3],NULL,10),rounds=strtoul(argv[4],NULL,10);
    if(!chunk || chunk>4*1024*1024 || chunk%4096 || FILE_BYTES%chunk ||
       !count || count>8 || !rounds) return 2;
    setenv("SNIA_FEMU_CACHE_REFILL","0",1);
    setenv("SNIA_FEMU_PROFILE","0",1);
    int fd=open(argv[1],O_RDWR|O_CREAT,0600);
    struct stat st;
    if(fd<0 || fstat(fd,&st)) { perror("file"); return 1; }
    if(!st.st_size) {
        uint64_t *data=malloc(4*1024*1024);
        if(!data) return 1;
        for(size_t at=0;at<FILE_BYTES;at+=4*1024*1024) {
            for(size_t i=0;i<4*1024*1024/8;i++) data[i]=(at/8+i)*UINT64_C(0x9e3779b97f4a7c15);
            if(pwrite(fd,data,4*1024*1024,at)!=4*1024*1024) { perror("create"); return 1; }
        }
        free(data);
    } else if(st.st_size!=FILE_BYTES) { fprintf(stderr,"file must be 1GiB\n"); return 1; }
    if(fsync(fd)) { perror("fsync"); return 1; }
    struct fiemap *map=calloc(1,sizeof(*map)+MAX_EXTENTS*sizeof(struct fiemap_extent));
    if(!map) return 1;
    map->fm_length=FILE_BYTES; map->fm_flags=FIEMAP_FLAG_SYNC; map->fm_extent_count=MAX_EXTENTS;
    if(ioctl(fd,FS_IOC_FIEMAP,map) || !map->fm_mapped_extents ||
       !(map->fm_extents[map->fm_mapped_extents-1].fe_flags&FIEMAP_EXTENT_LAST)) return 1;
    for(unsigned e=0;e<map->fm_mapped_extents;e++)
        if(map->fm_extents[e].fe_flags&~(FIEMAP_EXTENT_LAST|FIEMAP_EXTENT_MERGED)) {
            fprintf(stderr,"unsupported extent flags=%x\n",map->fm_extents[e].fe_flags); return 1;
        }
    unsigned lba;
    int raw=open("/dev/nvme0n1",O_RDONLY);
    if(raw<0 || ioctl(raw,BLKSSZGET,&lba)) return 1;
    close(raw);
    char path[128];
    snprintf(path,sizeof(path),"/sys/dev/block/%u:%u/start",major(st.st_dev),minor(st.st_dev));
    uint64_t partition=number(path)*512/lba;
    unsigned nsid=number("/sys/class/block/nvme0n1/nsid");
    /* Page-aligned, even maximally fragmented file pages must fit the queue.
     * The current plugin's fixed 1MiB chunks can exceed max_segments=128. */
    copy_bytes=number("/sys/class/block/nvme0n1/queue/max_segments")*4096;
    if(copy_bytes>1024*1024) copy_bytes=1024*1024;
    if(posix_fadvise(fd,0,FILE_BYTES,POSIX_FADV_DONTNEED)) return 1;
    char *mapped=mmap(NULL,FILE_BYTES,PROT_READ,MAP_SHARED,fd,0);
    if(mapped==MAP_FAILED) return 1;
    volatile unsigned checksum=0;
    uint64_t begin=now();
    for(size_t at=0;at<FILE_BYTES;at+=4096) checksum+=(unsigned char)mapped[at];
    double warm_ms=(now()-begin)/1e6;
    unsigned char *resident=malloc(FILE_BYTES/4096);
    if(!resident || mincore(mapped,FILE_BYTES,resident)) return 1;
    unsigned pages=0;
    for(size_t i=0;i<FILE_BYTES/4096;i++) pages+=!!(resident[i]&1);
    free(resident);
    if(pages!=FILE_BYTES/4096) { fprintf(stderr,"file not fully cached\n"); return 1; }
    CS_DEV_HANDLE dev; check(csOpenCSx("/dev/nvme0n1",NULL,&dev));
    CsMemFlags flags={.FDMId=1};
    struct lane lanes[8]={0}; pthread_t threads[8]; pthread_barrier_t start;
    pthread_barrier_init(&start,NULL,count+1);
    for(unsigned i=0;i<count;i++) {
        struct lane *l=&lanes[i];
        *l=(struct lane){.fd=fd,.number=i,.lanes=count,.lba=lba,.nsid=nsid,
            .chunk=chunk,.partition=partition,.dev=dev,.map=map,.mapped=mapped,.start=&start};
        check(csAllocMem(dev,chunk,&flags,&l->fdm,NULL));
        if(posix_memalign((void **)&l->buffer,4096,chunk)) return 1;
        memset(l->buffer,0,chunk);
        l->load=calloc(1,sizeof(*l->load)+map->fm_mapped_extents*sizeof(CSBlockRange));
        if(!l->load) return 1;
        *l->load=(CsStorageRequest){.Mode=CS_STORAGE_BLOCK_IO,.DevHandle=dev,
            .u.BlockIo={.Type=CS_STORAGE_LOAD_TYPE,.DevMem={l->fdm,0}}};
    }
    printf("# bytes=%lu chunk=%zu copy_request_bytes=%zu lanes=%u resident_pages=%u warm_ms=%.3f checksum=%u extents=%u\n",
        FILE_BYTES,chunk,copy_bytes,count,pages,warm_ms,checksum,map->fm_mapped_extents);
    puts("round,mode,wall_ms,MiB_s,submit_sum_ms,pread_sum_ms,verified");
    const char *names[]={"nvm_load","cache_mmap","cache_pread"};
    for(unsigned round=0;round<rounds;round++) for(unsigned step=0;step<3;step++) {
        unsigned mode=(step+round)%3;
        for(unsigned i=0;i<count;i++) { lanes[i].mode=mode; if(pthread_create(&threads[i],NULL,transfer,&lanes[i])) return 1; }
        begin=now(); pthread_barrier_wait(&start);
        uint64_t submit=0,read=0;
        for(unsigned i=0;i<count;i++) {
            pthread_join(threads[i],NULL);
            if(lanes[i].error) return 1;
            submit+=lanes[i].submit_ns; read+=lanes[i].read_ns;
        }
        double ms=(now()-begin)/1e6;
        /* Untimed full last-chunk readback for every independent stream. */
        for(unsigned i=0;i<count;i++) {
            struct lane *l=&lanes[i];
            for(size_t off=0;off<chunk;off+=copy_bytes) {
                size_t n=chunk-off; if(n>copy_bytes) n=copy_bytes;
                CsCopyMemRequest copy={.Type=CS_COPY_FROM_DEVICE,.u.HostVAddress=l->buffer+off,
                    .DevMem={l->fdm,off},.Bytes=n};
                check(csQueueCopyMemRequest(&copy,NULL,NULL,NULL,NULL,NULL));
            }
            if(memcmp(l->buffer,mapped+l->last,chunk)) { fprintf(stderr,"data mismatch\n"); return 1; }
        }
        printf("%u,%s,%.3f,%.3f,%.3f,%.3f,yes\n",round+1,names[mode],ms,1024*1000/ms,submit/1e6,read/1e6);
        fflush(stdout);
    }
    for(unsigned i=0;i<count;i++) { check(csFreeMem(lanes[i].fdm,&flags)); free(lanes[i].buffer); free(lanes[i].load); }
    check(csCloseCSx(dev)); pthread_barrier_destroy(&start);
    munmap(mapped,FILE_BYTES); close(fd); free(map);
    return 0;
}
