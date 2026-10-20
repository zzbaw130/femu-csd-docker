/* Guest example: file extents -> internal LOAD -> external sort -> average. */
#include "cs.h"
#include "batch_sort.h"
#include <fcntl.h>
#include <limits.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define TRY(call) do { CS_STATUS status = (call); if (status != CS_SUCCESS) { \
    fprintf(stderr, "%s: status=%d\n", #call, status); goto cleanup; } } while (0)
enum { SORT_SINK, SORT_SOURCE, AVERAGE, PROGRAM_COUNT };
struct program {
    const char *file, *symbol;
    unsigned index;
    CS_CSF_ID function;
    int loaded, active;
};

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
/* CsComputeRequest describes computation; its caller supplies the batch wrapper. */
static void build_compute_request(CsComputeRequest *request, CS_CSF_ID function,
                                  CS_MEM_HANDLE input, CS_MEM_HANDLE output, u64 state, CS_MEM_HANDLE control)
{
    request->CSFId = function;
    request->NumArgs = 0;
    if (input) csHelperSetComputeArg(&request->Args[request->NumArgs++], CS_AFDM_TYPE, input, 0UL);
    if (control) csHelperSetComputeArg(&request->Args[request->NumArgs++], CS_AFDM_TYPE, control, 0UL);
    if (output) csHelperSetComputeArg(&request->Args[request->NumArgs++], CS_AFDM_TYPE, output, 0UL);
    if (state) csHelperSetComputeArg(&request->Args[request->NumArgs++], CS_64BIT_VALUE_TYPE, state);
}

static int generate_file(int fd, int64_t *expected_sum)
{
    int32_t *buffer = malloc(SORT_CHUNK_BYTES);
    if (!buffer) return -1;
    uint32_t random = 0x12345678;
    *expected_sum = 0;
    for (unsigned chunk = 0; chunk < SORT_MAX_RUNS; ++chunk) {
        for (unsigned i = 0; i < SORT_CHUNK_BYTES / sizeof(*buffer); ++i) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            buffer[i] = (int32_t)random;
            *expected_sum += buffer[i];
        }
        size_t written = 0;
        while (written < SORT_CHUNK_BYTES) {
            ssize_t n = write(fd, (char *)buffer + written, SORT_CHUNK_BYTES - written);
            if (n <= 0) { free(buffer); return -1; }
            written += n;
        }
    }
    free(buffer);
    return fsync(fd);
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
/* The caller reuses one storage request; only its extent ranges change per chunk. */
static int build_load_request(CsStorageRequest *request, const struct fiemap *map, u64 logical,
                              unsigned lba, unsigned nsid, u64 start,
                              CS_DEV_HANDLE device, CS_MEM_HANDLE input)
{
    request->Mode = CS_STORAGE_BLOCK_IO;
    request->DevHandle = device;
    CsBlockIo *load = &request->u.BlockIo;
    load->Type = CS_STORAGE_LOAD_TYPE;
    load->DevMem.MemHandle = input;
    load->NumRanges = 0;
    u64 end = logical + SORT_CHUNK_BYTES, cursor = logical;
    for (unsigned i = 0; i < map->fm_mapped_extents && cursor < end; ++i) {
        const struct fiemap_extent *extent = &map->fm_extents[i];
        u64 last = extent->fe_logical + extent->fe_length;
        if (last <= cursor) continue;
        u64 bytes = (last < end ? last : end) - cursor;
        u64 physical = extent->fe_physical + cursor - extent->fe_logical;
        if (extent->fe_logical > cursor || physical % lba || bytes % lba ||
            extent->fe_flags & ~(FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_MERGED)) break;
        load->Range[load->NumRanges++] = (CSBlockRange){nsid, start + physical / lba, bytes / lba};
        cursor += bytes;
    }
    return cursor == end ? 0 : -1;
}
int main(int argc, char **argv)
{
    if (argc > 4 || (argc == 4 && strcmp(argv[3], "--existing"))) {
        fprintf(stderr, "usage: %s [device] [file] [--existing]\n", argv[0]);
        return 2;
    }
    int existing = argc == 4;
    const char *device_path = argc > 1 ? argv[1] : "/dev/nvme0n1";
    const char *file_path = argc > 2 ? argv[2] : "/data/cs-batch-input.bin";
    struct program programs[] = {
        [SORT_SINK] = {.file = "merge-sort.so", .symbol = "sort_sink", .index = 420},
        [SORT_SOURCE] = {.file = "merge-sort.so", .symbol = "sort_source", .index = 421},
        [AVERAGE] = {.file = "cs-example-avg.so", .symbol = "avg_int32", .index = 422}};
    CS_DEV_HANDLE device = NULL;
    CS_MEM_HANDLE input[SORT_WORKERS] = {0}, io[SORT_WORKERS] = {0}, task[SORT_WORKERS] = {0};
    CS_MEM_HANDLE sorted = NULL, result = NULL, group_mem = NULL;
    struct sort_run_group group = {.count = SORT_WORKERS};
    int group_ready = 0;
    CS_BATCH_HANDLE batch = NULL;
    CsBatchRequest *load_entry = NULL, *compute_entry = NULL;
    CsMemFlags flags = {.FDMId = 1};
    u64 partition_start;
    unsigned lba, nsid;
    int rc = 1, file = -1;
    struct fiemap *map = calloc(1, sizeof(*map) + 4096 * sizeof(struct fiemap_extent));
    if (!map) goto cleanup;
    file = open(file_path, existing ? O_RDONLY : O_CREAT | O_RDWR, 0644);
    if (file < 0 || flock(file, LOCK_EX | LOCK_NB) || read_namespace_geometry(file, device_path, &lba, &nsid, &partition_start)) {
        fprintf(stderr, "数据文件必须位于指定 namespace 的普通数据分区\n"); goto cleanup;
    }
    int64_t expected_sum = 0;
    if (existing) {
        struct stat info;
        if (fstat(file, &info) || info.st_size != SORT_FILE_BYTES) {
            fprintf(stderr, "existing file must contain exactly 256 MiB\n"); goto cleanup;
        }
    } else if (ftruncate(file, 0) || generate_file(file, &expected_sum)) {
        perror("generate_file"); goto cleanup;
    }
    map->fm_length = SORT_FILE_BYTES;
    map->fm_flags = FIEMAP_FLAG_SYNC;
    map->fm_extent_count = 4096;
    if (ioctl(file, FS_IOC_FIEMAP, map) || !map->fm_mapped_extents ||
        !(map->fm_extents[map->fm_mapped_extents - 1].fe_flags & FIEMAP_EXTENT_LAST)) {
        fprintf(stderr, "FIEMAP 未返回完整数据文件映射\n"); goto cleanup;
    }
    printf("256 MiB random int32 file, %u extents; partition start LBA=%llu\n",
           map->fm_mapped_extents, (unsigned long long)partition_start);
    TRY(csOpenCSx(device_path, NULL, &device));
    for (unsigned i = 0; i < PROGRAM_COUNT; ++i) TRY(load_program(device, &programs[i]));
    for (unsigned i = 0; i < SORT_WORKERS; ++i) {
        TRY(csAllocMem(device, SORT_CHUNK_BYTES, &flags, &input[i], NULL));
        TRY(csAllocMem(device, SORT_BLOCK_BYTES, &flags, &io[i], NULL));
        flags.Flags = CS_FDM_CLEAR;
        TRY(csAllocMem(device, sizeof(struct sort_sink_task), &flags, &task[i], NULL));
        struct sort_sink_task control = {.workers = SORT_WORKERS};
        CsCopyMemRequest write = {.Type = CS_COPY_TO_DEVICE, .DevMem = {task[i], 0},
            .u.HostVAddress = &control, .Bytes = sizeof(control)};
        TRY(csQueueCopyMemRequest(&write, NULL, NULL, NULL, NULL, NULL));
        flags.Flags = 0;
    }
    TRY(csAllocMem(device, sizeof(group), &flags, &group_mem, NULL));
    TRY(csAllocMem(device, sizeof(struct sorted_chunk), &flags, &sorted, NULL));
    flags.Flags = CS_FDM_CLEAR; // AVG accumulates across chunks; only its small output needs zeroing.
    TRY(csAllocMem(device, sizeof(struct average_result), &flags, &result, NULL));
    // Allocate two batch wrappers once; the API copies each configured request.
    load_entry = calloc(1, sizeof(*load_entry) + (map->fm_mapped_extents - 1) * sizeof(CSBlockRange));
    compute_entry = calloc(1, sizeof(*compute_entry) + 4 * sizeof(CsComputeArg));
    if (!load_entry || !compute_entry) goto cleanup;
    load_entry->ReqType = CS_STORAGE_IO;
    compute_entry->ReqType = CS_QUEUE_COMPUTE;

    struct timespec start, sink_done, end;
    printf("CONFIG CSD sink_workers=%u merge_workers=1 work_bytes=%zu buffer_bytes=%zu\n",
           SORT_WORKERS, sort_work_bytes(SORT_WORKERS), sort_buffer_bytes(SORT_WORKERS));
    clock_gettime(CLOCK_MONOTONIC, &start);
    // Four independent LOAD -> sink branches. Each sink uses the same CSF ID.
    TRY(csAllocBatchRequest(CS_BATCH_HYBRID, 2 * SORT_WORKERS, &batch));
    CS_BATCH_INDEX load_nodes[SORT_WORKERS], sink_node;
    for (unsigned round = 0; round < SORT_MAX_RUNS / SORT_WORKERS; ++round) {
        for (unsigned i = 0; i < SORT_WORKERS; ++i) {
            u64 offset = (u64)(round * SORT_WORKERS + i) * SORT_CHUNK_BYTES;
            if (build_load_request(&load_entry->u.StorageIo, map, offset, lba, nsid, partition_start, device, input[i])) {
                fprintf(stderr, "extent 存在空洞或不支持的标志/对齐\n"); goto cleanup;
            }
            if (!round) {
                TRY(csConfigureBatchEntry(batch, CS_BATCH_ADD, load_entry, 0, 0, &load_nodes[i]));
                build_compute_request(&compute_entry->u.Compute, programs[SORT_SINK].function, input[i], io[i], 0, task[i]);
                TRY(csConfigureBatchEntry(batch, CS_BATCH_ADD, compute_entry, 0, load_nodes[i], &sink_node));
            } else {
                TRY(csConfigureBatchEntry(batch, CS_BATCH_RECONFIG, load_entry, load_nodes[i], 0, NULL));
            }
        }
        TRY(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, NULL));
    }
    for (unsigned i = 0; i < SORT_WORKERS; ++i) {
        struct sort_sink_task control;
        CsCopyMemRequest read = {.Type = CS_COPY_FROM_DEVICE, .DevMem = {task[i], 0},
            .u.HostVAddress = &control, .Bytes = sizeof(control)};
        TRY(csQueueCopyMemRequest(&read, NULL, NULL, NULL, NULL, NULL));
        group.states[i] = control.state;
    }
    CsCopyMemRequest write_group = {.Type = CS_COPY_TO_DEVICE, .DevMem = {group_mem, 0},
        .u.HostVAddress = &group, .Bytes = sizeof(group)};
    TRY(csQueueCopyMemRequest(&write_group, NULL, NULL, NULL, NULL, NULL));
    group_ready = 1;
    TRY(csFreeBatchRequest(batch));
    batch = NULL;
    clock_gettime(CLOCK_MONOTONIC, &sink_done);

    // Phase 2: source -> avg. The same wrapper can configure both nodes.
    TRY(csAllocBatchRequest(CS_BATCH_SERIAL, 2, &batch));
    CS_BATCH_INDEX source_node, average_node;
    build_compute_request(&compute_entry->u.Compute, programs[SORT_SOURCE].function, sorted, io[0], group.states[0], group_mem);
    TRY(csConfigureBatchEntry(batch, CS_BATCH_ADD, compute_entry, 0, 0, &source_node));
    build_compute_request(&compute_entry->u.Compute, programs[AVERAGE].function, sorted, result, 0, NULL);
    TRY(csConfigureBatchEntry(batch, CS_BATCH_ADD, compute_entry, 0, source_node, &average_node));

    struct average_result average;
    CsCopyMemRequest read_result = {.Type = CS_COPY_FROM_DEVICE, .DevMem = {result, 0},
        .u.HostVAddress = &average, .Bytes = sizeof(average)};
    do {
        TRY(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, NULL));
        TRY(csQueueCopyMemRequest(&read_result, NULL, NULL, NULL, NULL, NULL));
    } while (!average.finished);
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (average.count != SORT_FILE_BYTES / sizeof(int32_t) ||
        average.average != (double)average.sum / (double)average.count ||
        (!existing && average.sum != expected_sum)) {
        fprintf(stderr, "FAIL: CSD result differs from host reference\n"); goto cleanup;
    }
    printf("AVERAGE count=%llu sum=%lld value=%.17g\n", (unsigned long long)average.count,
           (long long)average.sum, average.average);
    double sink_seconds = sink_done.tv_sec - start.tv_sec + (sink_done.tv_nsec - start.tv_nsec) / 1e9;
    double source_seconds = end.tv_sec - sink_done.tv_sec + (end.tv_nsec - sink_done.tv_nsec) / 1e9;
    printf("TIMING CSD %.6f %.6f %.6f\n", sink_seconds, source_seconds, sink_seconds + source_seconds);
    printf("WORKLOAD CSD runs=%u spilled=%u write=%llu read=%llu peak=%llu limit=%u\n",
           average.metrics.runs, average.metrics.spilled_runs,
           (unsigned long long)average.metrics.spill_write_bytes, (unsigned long long)average.metrics.spill_read_bytes,
           (unsigned long long)average.metrics.peak_bytes, SORT_MEMORY_BYTES);
    printf("PROFILE CSD sink_work wall_s=%.9f cpu_s=%.9f run_sort_s=%.9f\n",
           average.metrics.sink.wall_ns / 1e9, average.metrics.sink.cpu_ns / 1e9, average.metrics.run_sort_ns / 1e9);
    printf("PROFILE CSD source_work wall_s=%.9f cpu_s=%.9f\n",
           average.metrics.source.wall_ns / 1e9, average.metrics.source.cpu_ns / 1e9);
    printf("PROFILE CSD average_work wall_s=%.9f cpu_s=%.9f\n",
           average.aggregate.wall_ns / 1e9, average.aggregate.cpu_ns / 1e9);
    puts("PASS: 4 parallel sinks / 1 source; 256 MiB, memory-first runs, spill on allocation failure, full merge, AVG; source released all state");
    rc = 0;
cleanup:
    if (batch) csFreeBatchRequest(batch);
    if (group_ready) {
        CsCopyMemRequest read = {.Type = CS_COPY_FROM_DEVICE, .DevMem = {group_mem, 0},
            .u.HostVAddress = &group, .Bytes = sizeof(group)};
        csQueueCopyMemRequest(&read, NULL, NULL, NULL, NULL, NULL);
    } else {
        for (unsigned i = 0; i < SORT_WORKERS; ++i) if (task[i]) {
            struct sort_sink_task control = {0};
            CsCopyMemRequest read = {.Type = CS_COPY_FROM_DEVICE, .DevMem = {task[i], 0},
                .u.HostVAddress = &control, .Bytes = sizeof(control)};
            csQueueCopyMemRequest(&read, NULL, NULL, NULL, NULL, NULL);
            group.states[i] = control.state;
        }
    }
    for (unsigned i = 0; i < SORT_WORKERS; ++i) if (group.states[i] && compute_entry) {
        build_compute_request(&compute_entry->u.Compute, programs[SORT_SOURCE].function, io[0], NULL, group.states[i], NULL);
        csQueueComputeRequest(&compute_entry->u.Compute, NULL, NULL, NULL, NULL, NULL);
    }
    free(load_entry);
    free(compute_entry);
    free(map);
    flags.Flags = 0;
    if (result) csFreeMem(result, &flags);
    if (sorted) csFreeMem(sorted, &flags);
    if (group_mem) csFreeMem(group_mem, &flags);
    for (unsigned i = 0; i < SORT_WORKERS; ++i) {
        if (task[i]) csFreeMem(task[i], &flags);
        if (io[i]) csFreeMem(io[i], &flags);
        if (input[i]) csFreeMem(input[i], &flags);
    }
    for (unsigned i = 0; i < PROGRAM_COUNT; ++i) unload_program(device, &programs[i]);
    if (device) csCloseCSx(device);
    if (file >= 0) close(file);
    return rc;
}
