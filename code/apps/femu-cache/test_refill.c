/* Guest-only test: extent LOAD -> read-only Filter, with optional cache refill. */
#include "cs.h"
#include "csd-cache.h"
#include <linux/fs.h>
#include <linux/fiemap.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <time.h>
#define LANES 8
#define CHUNK (64U*1024)
#define FILE_BYTES (16U*1024*1024)
struct program { const char *file,*symbol; unsigned index; CS_CSF_ID function; int loaded,active; };
static CS_STATUS load_program(CS_DEV_HANDLE device, struct program *p)
{
    char descriptor[128];
    size_t first = strlen(p->file) + 1;
    memcpy(descriptor, p->file, first);
    strcpy(descriptor + first, p->symbol);
    CsCSFDownloadInfo download = {.Type = CS_CSF_VENDOR_SPECIFIC,
        .SubType = CS_SUBTYPE_VENDOR_SPECIFIC, .CSEEId = 1, .Index = p->index};
    snprintf(download.common.UniqueName, sizeof(download.common.UniqueName), "example-%s", p->symbol);
    download.common.DataBuffer = descriptor;
    download.common.Length = first + strlen(p->symbol) + 1;
    u32 id;
    CS_STATUS status = csCSFDownload(device, &download, &id);
    if (status != CS_SUCCESS) return status;
    p->loaded = 1;
    CsConfigInfo config = {.Type = CS_CSF_ACTIVATE,
        .u.CSFActivateInfo = {.CSFId = id, .EEIId = 1, .State = CS_ACTIVE_STATE}};
    CsConfigData data;
    int length = sizeof(data);
    status = csConfig(device, &length, &config, &data);
    if (status != CS_SUCCESS) return status;
    p->active = 1;
    int count;
    length = 0;
    status = csGetCSFId(device, download.common.UniqueName, 0, &length, &count, NULL);
    if (status != CS_LENGTH_RETURNED) return status;
    CSFIdInfo *functions = malloc(length);
    if (!functions) return CS_OUT_OF_RESOURCES;
    status = csGetCSFId(device, download.common.UniqueName, 0, &length, &count, functions);
    if (status == CS_SUCCESS) p->function = functions[0].CSFId;
    free(functions);
    return status;
}
static void unload_program(CS_DEV_HANDLE device, const struct program *p)
{
    if (p->active) {
        CsConfigInfo config = {.Type = CS_CSF_ACTIVATE,
            .u.CSFActivateInfo = {.CSFId = p->index, .EEIId = 1, .State = CS_INACTIVE_STATE}};
        CsConfigData data;
        int length = sizeof(data);
        csConfig(device, &length, &config, &data);
    }
    if (p->loaded) {
        CsCSFDownloadInfo download = {.CSEEId = 1, .Index = p->index, .common.Unload = 1};
        u32 id;
        csCSFDownload(device, &download, &id);
    }
}
static int read_sysfs_number(const char *path, unsigned long long *value)
{
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    int result = fscanf(file, "%llu", value) == 1 ? 0 : -1;
    fclose(file);
    return result;
}
/* FIEMAP physical offsets are partition-relative; StartLba is namespace-relative. */
static int read_namespace_geometry(int file, const char *device, unsigned *lba, unsigned *nsid, u64 *start)
{
    struct stat data, raw;
    int fd = open(device, O_RDONLY);
    if (fd < 0) return -1;
    int valid = !fstat(file, &data) && !fstat(fd, &raw) && S_ISBLK(raw.st_mode) && !ioctl(fd, BLKSSZGET, lba);
    close(fd);
    if (!valid) return -1;
    char path[128];
    unsigned long long value;
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/nsid", major(raw.st_rdev), minor(raw.st_rdev));
    if (read_sysfs_number(path, &value)) return -1;
    *nsid = value;
    *start = 0;
    if (data.st_dev == raw.st_rdev) return 0;
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/../dev", major(data.st_dev), minor(data.st_dev));
    FILE *parent = fopen(path, "r");
    if (!parent) return -1;
    unsigned maj, min;
    valid = fscanf(parent, "%u:%u", &maj, &min) == 2 && makedev(maj, min) == raw.st_rdev;
    fclose(parent);
    if (!valid) return -1;
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/start", major(data.st_dev), minor(data.st_dev));
    if (read_sysfs_number(path, &value) || value * 512 % *lba) return -1;
    *start = value * 512 / *lba;
    return 0;
}

struct lane {
    CS_DEV_HANDLE dev; CS_CSF_ID csf; struct fiemap *map;
    unsigned number,lba,repetitions; uint64_t partition,sum,count;
    int error;
};
#define CHECK(call) do { CS_STATUS s=(call); if(s!=CS_SUCCESS) { fprintf(stderr,"%s: %d\n",#call,s); lane->error=1; goto done; } } while(0)
static void *run_lane(void *opaque)
{
    struct lane *lane=opaque;
    CS_MEM_HANDLE input=NULL,output=NULL;
    CS_BATCH_HANDLE batch=NULL;
    CsBatchRequest *load=NULL,*compute=NULL;
    CsMemFlags flags={.FDMId=1};
    CHECK(csAllocMem(lane->dev,CHUNK,&flags,&input,NULL));
    CHECK(csAllocMem(lane->dev,16,&flags,&output,NULL));
    load=calloc(1,sizeof(*load)+lane->map->fm_mapped_extents*sizeof(CSBlockRange));
    compute=calloc(1,sizeof(*compute)+3*sizeof(CsComputeArg));
    if(!load || !compute) { lane->error=1; goto done; }
    load->ReqType=CS_STORAGE_IO; load->u.StorageIo.Mode=CS_STORAGE_BLOCK_IO;
    load->u.StorageIo.DevHandle=lane->dev;
    load->u.StorageIo.u.BlockIo.Type=CS_STORAGE_LOAD_TYPE;
    load->u.StorageIo.u.BlockIo.DevMem.MemHandle=input;
    compute->ReqType=CS_QUEUE_COMPUTE; compute->u.Compute.CSFId=lane->csf;
    compute->u.Compute.NumArgs=3;
    csHelperSetComputeArg(&compute->u.Compute.Args[0],CS_AFDM_TYPE,input,0UL);
    csHelperSetComputeArg(&compute->u.Compute.Args[1],CS_AFDM_TYPE,output,0UL);
    csHelperSetComputeArg(&compute->u.Compute.Args[2],CS_64BIT_VALUE_TYPE,(u64)lane->repetitions);
    CHECK(csAllocBatchRequest(CS_BATCH_SERIAL,2,&batch));
    CS_BATCH_INDEX first,second;
    for(uint64_t at=(uint64_t)lane->number*CHUNK;at<FILE_BYTES;at+=(uint64_t)LANES*CHUNK) {
        CsBlockIo *io=&load->u.StorageIo.u.BlockIo; io->NumRanges=0;
        uint64_t cursor=at;
        for(unsigned e=0;e<lane->map->fm_mapped_extents && cursor<at+CHUNK;e++) {
            const struct fiemap_extent *extent=&lane->map->fm_extents[e];
            if(cursor>=extent->fe_logical+extent->fe_length) continue;
            if(cursor<extent->fe_logical) { lane->error=1; goto done; }
            uint64_t bytes=extent->fe_logical+extent->fe_length-cursor;
            if(bytes>at+CHUNK-cursor) bytes=at+CHUNK-cursor;
            CSBlockRange *range=&io->Range[io->NumRanges++];
            range->NamespaceId=1; range->StartLba=lane->partition+(extent->fe_physical+cursor-extent->fe_logical)/lane->lba;
            range->NumBlocks=bytes/lane->lba; cursor+=bytes;
        }
        if(cursor!=at+CHUNK) { lane->error=1; goto done; }
        if(at==(uint64_t)lane->number*CHUNK) {
            CHECK(csConfigureBatchEntry(batch,CS_BATCH_ADD,load,0,0,&first));
            CHECK(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,first,&second));
        } else CHECK(csConfigureBatchEntry(batch,CS_BATCH_RECONFIG,load,first,0,NULL));
        CHECK(csQueueBatchRequest(batch,NULL,NULL,NULL,NULL,NULL));
        uint64_t values[2];
        CsCopyMemRequest copy={.Type=CS_COPY_FROM_DEVICE,.DevMem={output,0},.u.HostVAddress=values,.Bytes=16};
        CHECK(csQueueCopyMemRequest(&copy,NULL,NULL,NULL,NULL,NULL));
        lane->sum+=values[0]; lane->count+=values[1];
    }
done:
    free(load); free(compute);
    if(batch) csFreeBatchRequest(batch);
    if(input) csFreeMem(input,&flags);
    if(output) csFreeMem(output,&flags);
    return NULL;
}
int main(int argc,char **argv)
{
    if(argc!=4) { fprintf(stderr,"usage: %s data-file repetitions refill[0|1]\n",argv[0]); return 2; }
    unsigned repetitions=strtoul(argv[2],NULL,10); int refill=atoi(argv[3]);
    if(!repetitions) return 2;
    setenv("SNIA_FEMU_CACHE_REFILL",refill?"1":"0",1);
    int fd=open(argv[1],O_RDWR|O_CREAT,0600); if(fd<0) { perror("open"); return 1; }
    uint32_t *buffer=malloc(CHUNK); uint64_t expected_sum=0,expected_count=0;
    for(unsigned b=0;b<FILE_BYTES/CHUNK;b++) {
        for(unsigned i=0;i<CHUNK/4;i++) {
            uint32_t v=b*(CHUNK/4)+i; buffer[i]=v;
            if(v&1) { expected_sum+=v; expected_count++; }
        }
        if(pwrite(fd,buffer,CHUNK,(off_t)b*CHUNK)!=CHUNK) { perror("pwrite"); return 1; }
    }
    free(buffer); if(ftruncate(fd,FILE_BYTES)||fsync(fd)) return 1;
    struct fiemap *map=calloc(1,sizeof(*map)+1024*sizeof(struct fiemap_extent));
    map->fm_length=FILE_BYTES; map->fm_flags=FIEMAP_FLAG_SYNC; map->fm_extent_count=1024;
    if(ioctl(fd,FS_IOC_FIEMAP,map) || !map->fm_mapped_extents) { perror("FIEMAP"); return 1; }
    unsigned lba,nsid; u64 partition;
    if(read_namespace_geometry(fd,"/dev/nvme0n1",&lba,&nsid,&partition)) return 1;
    int module=open("/dev/femu-cache",O_RDWR);
    if(module<0) { perror("femu-cache"); return 1; }
    if(ioctl(module,FEMU_CACHE_CLEAR)) { perror("CLEAR"); return 1; }
    if(refill) for(unsigned i=0;i<map->fm_mapped_extents;i++) {
        struct fiemap_extent *extent=&map->fm_extents[i];
        struct femu_cache_mapping range={.fd=fd,.nsid=nsid,.lba_bytes=partition*lba+extent->fe_physical,
            .file_offset=extent->fe_logical,.bytes=extent->fe_length};
        if(ioctl(module,FEMU_CACHE_REGISTER,&range)) { perror("REGISTER"); return 1; }
    }
    /* Registration never reads file contents. Both modes start with cold pages. */
    if(posix_fadvise(fd,0,FILE_BYTES,POSIX_FADV_DONTNEED)) return 1;
    void *mapping=mmap(NULL,FILE_BYTES,PROT_NONE,MAP_SHARED,fd,0);
    unsigned char residency[FILE_BYTES/4096]; unsigned cold=0,resident=0;
    if(mapping==MAP_FAILED || mincore(mapping,FILE_BYTES,residency)) return 1;
    for(unsigned i=0;i<sizeof(residency);i++) cold+=!!(residency[i]&1);
    CS_DEV_HANDLE dev; if(csOpenCSx("/dev/nvme0n1",NULL,&dev)!=CS_SUCCESS) return 1;
    struct program program={.file="cache-filter.so",.symbol="cache_filter",.index=60000};
    if(load_program(dev,&program)!=CS_SUCCESS) return 1;
    struct lane lanes[LANES]; pthread_t threads[LANES];
    struct timespec begin,end; clock_gettime(CLOCK_MONOTONIC,&begin);
    for(unsigned i=0;i<LANES;i++) {
        lanes[i]=(struct lane){.dev=dev,.csf=program.function,.map=map,.number=i,.lba=lba,.partition=partition,.repetitions=repetitions};
        if(pthread_create(&threads[i],NULL,run_lane,&lanes[i])) return 1;
    }
    uint64_t sum=0,count=0; int error=0;
    for(unsigned i=0;i<LANES;i++) { pthread_join(threads[i],NULL); sum+=lanes[i].sum; count+=lanes[i].count; error|=lanes[i].error; }
    clock_gettime(CLOCK_MONOTONIC,&end);
    if(mincore(mapping,FILE_BYTES,residency)) error=1;
    for(unsigned i=0;i<sizeof(residency);i++) resident+=!!(residency[i]&1);
    if(sum!=expected_sum || count!=expected_count || cold || (refill && resident!=sizeof(residency)) || (!refill && resident)) error=1;
    printf("refill=%d repetitions=%u bytes=%u lanes=%u chunk=%u cold_pages=%u resident_pages=%u sum=%llu count=%llu correct=%s wall_ms=%.3f\n",
        refill,repetitions,FILE_BYTES,LANES,CHUNK,cold,resident,(unsigned long long)sum,(unsigned long long)count,error?"no":"yes",
        (end.tv_sec-begin.tv_sec)*1000.+(end.tv_nsec-begin.tv_nsec)/1e6);
    /* Read the populated cache and compare every byte, outside the timed region. */
    buffer=malloc(CHUNK);
    for(unsigned b=0;b<FILE_BYTES/CHUNK;b++) {
        if(pread(fd,buffer,CHUNK,(off_t)b*CHUNK)!=CHUNK) { error=1; break; }
        for(unsigned i=0;i<CHUNK/4;i++) if(buffer[i]!=b*(CHUNK/4)+i) { error=1; break; }
    }
    printf("cache_bytes_match=%s\n",error?"no":"yes");
    free(buffer); unload_program(dev,&program); csCloseCSx(dev);
    ioctl(module,FEMU_CACHE_CLEAR); close(module); munmap(mapping,FILE_BYTES); close(fd); free(map);
    return error?1:0;
}
