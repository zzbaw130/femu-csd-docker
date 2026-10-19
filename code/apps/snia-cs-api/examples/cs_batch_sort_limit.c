/* Guest example: file extents -> internal LOAD -> external sort -> LIMIT 5. */
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
#include <unistd.h>

#define TRY(call) do { CS_STATUS status = (call); if (status != CS_SUCCESS) { \
    fprintf(stderr, "%s: status=%d\n", #call, status); goto cleanup; } } while (0)
struct program { const char *file, *symbol; unsigned index; CS_CSF_ID function; int loaded, active; };

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
static CS_STATUS copy_memory(CS_MEM_HANDLE memory, void *data, int bytes, CS_MEM_COPY_TYPE type)
{
    CsCopyMemRequest request = {.Type = type, .DevMem = {memory, 0}, .u.HostVAddress = data, .Bytes = bytes};
    return csQueueCopyMemRequest(&request, NULL, NULL, NULL, NULL, NULL);
}
static CsBatchRequest *compute(CS_CSF_ID function, u64 state, CS_MEM_HANDLE first, CS_MEM_HANDLE second)
{
    CsBatchRequest *request = calloc(1, sizeof(*request) + 2 * sizeof(CsComputeArg));
    if (!request) return NULL;
    request->ReqType = CS_QUEUE_COMPUTE;
    request->u.Compute.CSFId = function;
    unsigned n = 0;
    if (first) csHelperSetComputeArg(&request->u.Compute.Args[n++], CS_AFDM_TYPE, first, 0UL);
    if (second) csHelperSetComputeArg(&request->u.Compute.Args[n++], CS_AFDM_TYPE, second, 0UL);
    csHelperSetComputeArg(&request->u.Compute.Args[n++], CS_64BIT_VALUE_TYPE, state);
    request->u.Compute.NumArgs = n;
    return request;
}
static CS_STATUS single_batch(CS_CSF_ID function, u64 state, CS_MEM_HANDLE io)
{
    CsBatchRequest *request = compute(function, state, io, NULL);
    if (!request) return CS_OUT_OF_RESOURCES;
    CS_BATCH_HANDLE batch;
    CS_STATUS status = csAllocBatchRequest(CS_BATCH_SERIAL, 1, &batch);
    if (status == CS_SUCCESS) {
        CS_BATCH_INDEX entry;
        status = csConfigureBatchEntry(batch, CS_BATCH_ADD, request, 0, 0, &entry);
        if (status == CS_SUCCESS) status = csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, NULL);
        csFreeBatchRequest(batch);
    }
    free(request);
    return status;
}
static void remember_top_five(int32_t value, int32_t top[5])
{
    for (unsigned i = 0; i < 5; ++i) if (value > top[i]) {
        memmove(top + i + 1, top + i, (4 - i) * sizeof(*top));
        top[i] = value;
        break;
    }
}
static int generate_file(int fd, int32_t expected[5])
{
    int32_t *buffer = malloc(SORT_CHUNK_BYTES);
    if (!buffer) return -1;
    uint32_t random = 0x12345678;
    for (unsigned i = 0; i < 5; ++i) expected[i] = INT32_MIN;
    for (unsigned chunk = 0; chunk < SORT_MAX_RUNS; ++chunk) {
        for (unsigned i = 0; i < SORT_CHUNK_BYTES / sizeof(*buffer); ++i) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            buffer[i] = (int32_t)random;
            remember_top_five(buffer[i], expected);
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
static int read_number(const char *path, unsigned long long *value)
{
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    int result = fscanf(file, "%llu", value) == 1 ? 0 : -1;
    fclose(file);
    return result;
}
/* FIEMAP physical offsets are partition-relative; StartLba is namespace-relative. */
static int namespace_geometry(int file, const char *device, unsigned *lba, unsigned *nsid, u64 *start)
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
    if (read_number(path, &value)) return -1;
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
    if (read_number(path, &value) || value * 512 % *lba) return -1;
    *start = value * 512 / *lba;
    return 0;
}
static CsBatchRequest *load_extents(const struct fiemap *map, u64 logical, unsigned lba,
                                    unsigned nsid, u64 start, CS_DEV_HANDLE device, CS_MEM_HANDLE input)
{
    CsBatchRequest *request = calloc(1, sizeof(*request) + map->fm_mapped_extents * sizeof(CSBlockRange));
    if (!request) return NULL;
    request->ReqType = CS_STORAGE_IO;
    request->u.StorageIo.Mode = CS_STORAGE_BLOCK_IO;
    request->u.StorageIo.DevHandle = device;
    CsBlockIo *load = &request->u.StorageIo.u.BlockIo;
    load->Type = CS_STORAGE_LOAD_TYPE;
    load->DevMem.MemHandle = input;
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
    if (cursor != end) { free(request); return NULL; }
    return request;
}
int main(int argc, char **argv)
{
    const char *device_path = argc > 1 ? argv[1] : "/dev/nvme0n1";
    const char *file_path = argc > 2 ? argv[2] : "/data/cs-batch-input.bin";
    struct program programs[] = {
        {"cs-example-sort.so", "sort_create", 420, NULL, 0, 0},
        {"cs-example-sort.so", "sort_sink", 421, NULL, 0, 0},
        {"cs-example-sort.so", "sort_source", 422, NULL, 0, 0},
        {"cs-example-sort.so", "sort_destroy", 423, NULL, 0, 0},
        {"cs-example-sort.so", "sort_abort", 424, NULL, 0, 0},
        {"cs-example-limit.so", "limit_five", 425, NULL, 0, 0}};
    CS_DEV_HANDLE device = NULL;
    CS_MEM_HANDLE input = NULL, io = NULL, sorted = NULL, result = NULL;
    CS_BATCH_HANDLE batch = NULL;
    CsBatchRequest *load = NULL, *sink = NULL, *source = NULL, *limit = NULL;
    CsMemFlags flags = {.FDMId = 1};
    u64 sort_state = 0, partition_start;
    unsigned lba, nsid;
    int rc = 1, file = -1;
    struct fiemap *map = calloc(1, sizeof(*map) + 4096 * sizeof(struct fiemap_extent));
    if (!map) goto cleanup;
    file = open(file_path, O_CREAT | O_RDWR, 0644);
    if (file < 0 || flock(file, LOCK_EX | LOCK_NB) || namespace_geometry(file, device_path, &lba, &nsid, &partition_start) ||
        ftruncate(file, 0)) { fprintf(stderr, "数据文件必须位于指定 namespace 的普通数据分区\n"); goto cleanup; }
    int32_t expected[5];
    if (generate_file(file, expected)) { perror("generate_file"); goto cleanup; }
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
    for (unsigned i = 0; i < 6; ++i) TRY(load_program(device, &programs[i]));
    TRY(csAllocMem(device, SORT_CHUNK_BYTES, &flags, &input, NULL));
    TRY(csAllocMem(device, SORT_BLOCK_BYTES, &flags, &io, NULL));
    TRY(csAllocMem(device, sizeof(struct sorted_chunk), &flags, &sorted, NULL));
    TRY(csAllocMem(device, sizeof(struct top_five), &flags, &result, NULL));
    CsComputeRequest create = {.CSFId = programs[0].function};
    u64 created = 0;
    TRY(csQueueComputeRequest(&create, NULL, NULL, NULL, NULL, &created));
    sort_state = created;
    sink = compute(programs[1].function, sort_state, input, io);
    source = compute(programs[2].function, sort_state, sorted, io);
    limit = compute(programs[5].function, 0, sorted, result);
    if (!sink || !source || !limit) goto cleanup;
    // Reconfigure only LOAD; the sorter keeps private runs between successful batches.
    TRY(csAllocBatchRequest(CS_BATCH_SERIAL, 2, &batch));
    CS_BATCH_INDEX load_entry = 0, sort_entry;
    for (u64 offset = 0; offset < SORT_FILE_BYTES; offset += SORT_CHUNK_BYTES) {
        load = load_extents(map, offset, lba, nsid, partition_start, device, input);
        if (!load) { fprintf(stderr, "extent 存在空洞或不支持的标志/对齐\n"); goto cleanup; }
        if (!offset) {
            TRY(csConfigureBatchEntry(batch, CS_BATCH_ADD, load, 0, 0, &load_entry));
            TRY(csConfigureBatchEntry(batch, CS_BATCH_ADD, sink, 0, load_entry, &sort_entry));
        } else TRY(csConfigureBatchEntry(batch, CS_BATCH_RECONFIG, load, load_entry, 0, NULL));
        free(load); load = NULL;
        TRY(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, NULL));
        if ((offset + SORT_CHUNK_BYTES) % (64U << 20) == 0)
            printf("LOAD -> sort.sink: %llu / 256 MiB\n", (unsigned long long)((offset + SORT_CHUNK_BYTES) >> 20));
    }
    TRY(csFreeBatchRequest(batch)); batch = NULL;
    // Blocking boundary: all 64 runs now exist. Merge globally, then LIMIT 5.
    TRY(csAllocBatchRequest(CS_BATCH_SERIAL, 2, &batch));
    CS_BATCH_INDEX source_entry, limit_entry;
    TRY(csConfigureBatchEntry(batch, CS_BATCH_ADD, source, 0, 0, &source_entry));
    TRY(csConfigureBatchEntry(batch, CS_BATCH_ADD, limit, 0, source_entry, &limit_entry));
    TRY(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, NULL));
    struct top_five top;
    TRY(copy_memory(result, &top, sizeof(top), CS_COPY_FROM_DEVICE));
    if (top.count != 5 || memcmp(top.values, expected, sizeof(expected))) {
        fprintf(stderr, "FAIL: CSD result differs from host reference\n"); goto cleanup;
    }
    printf("Top 5: %d %d %d %d %d\n", top.values[0], top.values[1], top.values[2], top.values[3], top.values[4]);
    TRY(single_batch(programs[3].function, sort_state, io)); sort_state = 0;
    puts("PASS: 256 MiB, 64 runs, internal LOAD/STORE, reentry merge, LIMIT 5; spill freed");
    rc = 0;
cleanup:
    if (batch) csFreeBatchRequest(batch);
    if (sort_state && programs[4].function) single_batch(programs[4].function, sort_state, NULL);
    free(load); free(sink); free(source); free(limit); free(map);
    if (result) csFreeMem(result, &flags);
    if (sorted) csFreeMem(sorted, &flags);
    if (io) csFreeMem(io, &flags);
    if (input) csFreeMem(input, &flags);
    for (unsigned i = 0; i < 6; ++i) unload_program(device, &programs[i]);
    if (device) csCloseCSx(device);
    if (file >= 0) close(file);
    return rc;
}
