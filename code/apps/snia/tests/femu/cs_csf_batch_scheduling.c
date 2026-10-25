#include "cs.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "batch_sort.h"
static atomic_uint checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
#define OK(x) CHECK((x) == CS_SUCCESS)
static uint64_t (*mock_used_bytes)(void);
static uint64_t (*mock_graph_commands)(void),(*mock_hidden_used)(void);
static CS_CSF_ID load(CS_DEV_HANDLE d, const char *file, const char *symbol, const char *name, unsigned index)
{
    char descriptor[128];
    strcpy(descriptor, file);
    strcpy(descriptor + strlen(descriptor) + 1, symbol);
    CsCSFDownloadInfo info = {.Type = CS_CSF_VENDOR_SPECIFIC,
                              .SubType = CS_SUBTYPE_VENDOR_SPECIFIC,
                              .CSEEId = 1,
                              .Index = index};
    strcpy(info.common.UniqueName, name);
    info.common.DataBuffer = descriptor;
    info.common.Length = strlen(file) + strlen(symbol) + 2;
    u32 id;
    OK(csCSFDownload(d, &info, &id));
    CHECK(id == index);
    CsConfigInfo config = {
        .Type = CS_CSF_ACTIVATE,
        .u.CSFActivateInfo = {.State = CS_ACTIVE_STATE, .CSFId = index, .EEIId = 1}};
    int n = sizeof(CsConfigData);
    CsConfigData data;
    OK(csConfig(d, &n, &config, &data));
    n = 0;
    int count = 0;
    CHECK(csGetCSFId(d, name, 0, &n, &count, NULL) == CS_LENGTH_RETURNED);
    CHECK(count == 1);
    CSFIdInfo *out = calloc(1, n);
    OK(csGetCSFId(d, name, 0, &n, &count, out));
    CS_CSF_ID result = out->CSFId;
    CHECK(out->FDMList->FDMId == 1);
    free(out);
    return result;
}
static void wait_event(CS_EVT_HANDLE event)
{
    CS_STATUS s;
    for (unsigned i = 0; i < 3000; i++) {
        s = csPollEvent(event, NULL, NULL);
        if (s != CS_NOT_DONE) {
            CHECK(s == CS_SUCCESS);
            return;
        }
        usleep(1000);
    }
    CHECK(0);
}
static CS_STATUS copy(CS_MEM_HANDLE mem, void *host, int bytes, int type, unsigned long off)
{
    CsCopyMemRequest r = {
        .Type = type, .u.HostVAddress = host, .DevMem = {mem, off}, .Bytes = bytes};
    return csQueueCopyMemRequest(&r, NULL, NULL, NULL, NULL, NULL);
}
static atomic_int callback_done;
static void callback(void *ctx, CS_STATUS status, u64 value)
{
    CHECK(ctx == (void *)123);
    CHECK(status == CS_SUCCESS);
    CHECK(value == 4);
    atomic_store(&callback_done, 1);
}
static atomic_uint requeue_entered;
static atomic_bool requeue_release;
struct callback_batch {
    CS_BATCH_HANDLE batch;
    CsBatchRequest entry;
    CS_BATCH_INDEX index;
    atomic_bool done;
    unsigned resumed;
    int result;
};
static void requeue_batch(void *context, CS_STATUS status, u64 value)
{
    struct callback_batch *b = context;
    CHECK(status == CS_SUCCESS && value == sizeof(b->result) && b->result == 9);
    if (!b->resumed) {
        b->resumed = 1;
        atomic_fetch_add(&requeue_entered, 1);
        while (!atomic_load(&requeue_release)) usleep(100);
        /* All workers can be inside callbacks. Both a synchronous request and
         * an asynchronous child waited here must still be able to progress. */
        OK(csQueueCopyMemRequest(&b->entry.u.CopyMem,NULL,NULL,NULL,NULL,NULL));
        CS_EVT_HANDLE event; OK(csCreateEvent(&event));
        CHECK(csQueueCopyMemRequest(&b->entry.u.CopyMem,NULL,NULL,event,NULL,NULL)==CS_QUEUED);
        CS_STATUS child=CS_NOT_DONE;
        for(unsigned i=0;i<5000 && child==CS_NOT_DONE;++i) {
            child=csPollEvent(event,NULL,NULL); if(child==CS_NOT_DONE) usleep(100);
        }
        CHECK(child==CS_SUCCESS); OK(csDeleteEvent(event));
        OK(csConfigureBatchEntry(b->batch, CS_BATCH_RECONFIG, &b->entry, b->index, 0, NULL));
        CHECK(csQueueBatchRequest(b->batch, b, requeue_batch, NULL, NULL, NULL) == CS_QUEUED);
        return;
    }
    atomic_store(&b->done, true);
}
static void test_batch_callback_requeue(CS_MEM_HANDLE memory)
{
    /* All 64 callbacks are still running when they submit replacements. */
    struct callback_batch slots[64] = {0};
    for (unsigned i = 0; i < 64; ++i) {
        atomic_init(&slots[i].done, false);
        OK(csAllocBatchRequest(CS_BATCH_SERIAL, 1, &slots[i].batch));
        slots[i].entry = (CsBatchRequest){.ReqType = CS_COPY_AFDM,
            .u.CopyMem = {.Type = CS_COPY_FROM_DEVICE, .DevMem = {memory, 0},
                .u.HostVAddress = &slots[i].result, .Bytes = sizeof(slots[i].result)}};
        OK(csConfigureBatchEntry(slots[i].batch, CS_BATCH_ADD, &slots[i].entry, 0, 0, &slots[i].index));
        CHECK(csQueueBatchRequest(slots[i].batch, &slots[i], requeue_batch, NULL, NULL, NULL) == CS_QUEUED);
    }
    for (unsigned i = 0; i < 5000 && atomic_load(&requeue_entered) != 64; ++i) usleep(1000);
    CHECK(atomic_load(&requeue_entered) == 64);
    atomic_store(&requeue_release, true);
    for (unsigned i = 0; i < 64; ++i) {
        for (unsigned j = 0; j < 5000 && !atomic_load(&slots[i].done); ++j) usleep(1000);
        CHECK(atomic_load(&slots[i].done));
        OK(csFreeBatchRequest(slots[i].batch));
    }
}
static atomic_uint sequence_threads;
static _Thread_local bool sequence_thread_seen;
struct callback_sequence {
    CsCopyMemRequest request;
    atomic_bool done;
    unsigned calls;
    int result;
};
static void continue_copy(void *context,CS_STATUS status,u64 value)
{
    struct callback_sequence *sequence=context;
    CHECK(status==CS_SUCCESS && value==sizeof(sequence->result) && sequence->result==9);
    if(!sequence_thread_seen) { sequence_thread_seen=true; atomic_fetch_add(&sequence_threads,1); }
    if(++sequence->calls<512) {
        CHECK(csQueueCopyMemRequest(&sequence->request,sequence,continue_copy,NULL,NULL,NULL)==CS_QUEUED);
    } else atomic_store(&sequence->done,true);
}
static void test_callback_sequence(CS_MEM_HANDLE memory)
{
    struct callback_sequence sequence={0}; atomic_init(&sequence.done,false);
    sequence.request=(CsCopyMemRequest){.Type=CS_COPY_FROM_DEVICE,.DevMem={memory,0},
        .u.HostVAddress=&sequence.result,.Bytes=sizeof(sequence.result)};
    struct timespec begin,end; clock_gettime(CLOCK_MONOTONIC,&begin);
    CHECK(csQueueCopyMemRequest(&sequence.request,&sequence,continue_copy,NULL,NULL,NULL)==CS_QUEUED);
    for(unsigned i=0;i<50000 && !atomic_load(&sequence.done);++i) usleep(100);
    CHECK(atomic_load(&sequence.done) && sequence.calls==512);
    /* TLS is initialized for each new thread lifetime, even if its OS thread
     * ID is recycled. One continuation chain must reuse execution threads. */
    CHECK(atomic_load(&sequence_threads)<64);
    clock_gettime(CLOCK_MONOTONIC,&end);
    printf("callback sequence: calls=%u thread_lifetimes=%u wall_ms=%.3f\n",sequence.calls,
        atomic_load(&sequence_threads),(end.tv_sec-begin.tv_sec)*1000.0+(end.tv_nsec-begin.tv_nsec)/1000000.0);
}
int main(void)
{
    CHECK(!setenv("CSD_WORKMEM_SIZE_MB", "12", 1));
    void *mock = dlopen(CS_MOCK_PLUGIN, RTLD_NOW | RTLD_LOCAL);
    assert(mock);
    mock_used_bytes = dlsym(mock, "mock_used_bytes");
    assert(mock_used_bytes);
    mock_graph_commands=dlsym(mock,"mock_graph_commands");
    mock_hidden_used=dlsym(mock,"mock_hidden_used");
    CHECK(mock_graph_commands && mock_hidden_used);
    /* Keep this test-only observation handle until process exit. */

    CS_DEV_HANDLE d;
    CsMemFlags f = {.FDMId = 1, .Flags = CS_FDM_CLEAR};
    CS_MEM_HANDLE a, b;
    CHECK(csCloseCSx((void *)1) == CS_INVALID_HANDLE);
    CHECK(csCreateEvent(NULL) == CS_INVALID_ARG);
    OK(csOpenCSx("/dev/nvme0n1", NULL, &d));
    CHECK(csAllocMem(d, 0, &f, &a, NULL) == CS_INVALID_ARG);
    CS_MEM_PTR va = (void *)1;
    CHECK(csAllocMem(d, 8, &f, &a, &va) == CS_COULD_NOT_MAP_MEMORY);
    CHECK(va == NULL);
    OK(csAllocMem(d, 7, &f, &a, NULL));
    OK(csAllocMem(d, 4096, &f, &b, NULL));
    CHECK(csCloseCSx(d) == CS_HANDLE_IN_USE);
    uint8_t buf[32] = {0};
    OK(copy(a, buf, 32, CS_COPY_FROM_DEVICE, 0));
    CHECK(!memcmp(buf, (uint8_t[32]){0}, 32));
    CsMemFlags fill = {.Flags = CS_FDM_FILL, .FillValue = 0x11223344};
    OK(csInitMem(a, 3, 7, &fill));
    OK(copy(a, buf, 7, CS_COPY_FROM_DEVICE, 3));
    CHECK(!memcmp(buf, (uint8_t[]){0x44, 0x33, 0x22, 0x11, 0x44, 0x33, 0x22}, 7));
    CHECK(copy(a, buf, 20, CS_COPY_FROM_DEVICE, 4090) == CS_INVALID_LENGTH);
    CsCopyMemRequest within = {
        .Type = CS_COPY_WITHIN_DEVICE, .u.SrcDevMem = {a, 3}, .DevMem = {b, 5}, .Bytes = 7};
    OK(csQueueCopyMemRequest(&within, NULL, NULL, NULL, NULL, NULL));
    OK(copy(b, buf, 7, CS_COPY_FROM_DEVICE, 5));
    CHECK(buf[0] == 0x44 && buf[6] == 0x22);
    CS_CSF_ID sink = load(d, "merge-sort.so", "sort_sink", "sink", 201),
              source = load(d, "merge-sort.so", "sort_source", "source", 202),
              average = load(d, "cs-example-avg.so", "avg_int32", "avg", 203);
    CS_CSF_ID reader=load(d,"cs-nvm-read-test-csf.so","cs_test_nvm_read","nvm-reader",204);
    CS_MEM_HANDLE read_mem,read_io;
    OK(csAllocMem(d,4096,&f,&read_mem,NULL)); OK(csAllocMem(d,4096,&f,&read_io,NULL));
    CsBatchRequest *read=calloc(1,sizeof(*read)+3*sizeof(CsComputeArg)); CHECK(read);
    read->ReqType=CS_QUEUE_COMPUTE;
    read->u.Compute.CSFId=reader; read->u.Compute.NumArgs=3;
    csHelperSetComputeArg(&read->u.Compute.Args[0],CS_AFDM_TYPE,read_mem,0UL);
    csHelperSetComputeArg(&read->u.Compute.Args[1],CS_AFDM_TYPE,read_io,0UL);
    csHelperSetComputeArg(&read->u.Compute.Args[2],CS_64BIT_VALUE_TYPE,(u64)0);
    CS_BATCH_HANDLE read_batch; CS_BATCH_INDEX read_index;
    OK(csAllocBatchRequest(CS_BATCH_SERIAL,1,&read_batch));
    OK(csConfigureBatchEntry(read_batch,CS_BATCH_ADD,read,0,0,&read_index));
    u64 read_value=0,graphs=mock_graph_commands();
    OK(csQueueBatchRequest(read_batch,NULL,NULL,NULL,NULL,&read_value));
    CHECK(read_value==0xbeef && mock_graph_commands()==graphs+1);
    OK(copy(read_mem,buf,sizeof(buf),CS_COPY_FROM_DEVICE,512));
    for(unsigned i=0;i<sizeof(buf);++i) CHECK(buf[i]==0xa5);
    for(unsigned invalid=1;invalid<=6;++invalid) {
        csHelperSetComputeArg(&read->u.Compute.Args[2],CS_64BIT_VALUE_TYPE,(u64)invalid);
        OK(csConfigureBatchEntry(read_batch,CS_BATCH_RECONFIG,read,read_index,0,NULL));
        CHECK(csQueueBatchRequest(read_batch,NULL,NULL,NULL,NULL,NULL)!=CS_SUCCESS);
        OK(copy(read_io,buf,sizeof(buf),CS_COPY_FROM_DEVICE,0));
        CHECK(buf[0]==1 && buf[4]==1); /* no read issued before all ranges validate */
    }
    OK(csFreeBatchRequest(read_batch)); free(read);
    OK(csFreeMem(read_mem,&f)); OK(csFreeMem(read_io,&f));
    int n = 0;
    CHECK(csQueryDeviceProperties(d, CS_FDM_TYPE, &n, NULL) == CS_INVALID_LENGTH);
    CsProperties *props = calloc(1, n);
    OK(csQueryDeviceProperties(d, CS_FDM_TYPE, &n, props));
    CHECK(props->FDMDetails[0].FDM[0].FDMSize == (64U << 20));
    CHECK(!props->FDMDetails[0].FDM[0].Flags.HostVisible);
    free(props);
    n = 0;
    CHECK(csQueryDeviceProperties(d, CS_CSE_TYPE, &n, NULL) == CS_INVALID_LENGTH);
    props = calloc(1, n);
    OK(csQueryDeviceProperties(d, CS_CSE_TYPE, &n, props));
    CHECK(props->CSEDetails[0].CSE[0].NumCRs == 4);
    CHECK(props->CSEDetails[0].CSE[0].NumAvailableCRs == 0);
    free(props);
    n = 0;
    CHECK(csQueryDeviceProperties(d, CS_CSx_TYPE, &n, NULL) == CS_INVALID_LENGTH);
    props = calloc(1, n);
    OK(csQueryDeviceProperties(d, CS_CSx_TYPE, &n, props));
    CHECK(!props->CSxDetails.Flags.BatchRequestsSupported);
    free(props);
    n = 0;
    CHECK(csQueryDeviceProperties(d, CS_CSF_TYPE, &n, NULL) == CS_INVALID_LENGTH);
    props = calloc(1, n);
    OK(csQueryDeviceProperties(d, CS_CSF_TYPE, &n, props));
    CHECK(props->CSFDetails[0].NumCSFs == 4);
    free(props);
    // A standard COPY batch exercises request snapshots and dependency edits without toy CSFs.
    uint32_t values[] = {9, 17}, copied_value = 0;
    CsBatchRequest entry = {.ReqType = CS_COPY_AFDM, .u.CopyMem = {
        .Type = CS_COPY_TO_DEVICE, .DevMem = {a, 0}, .u.HostVAddress = &values[0], .Bytes = 4}};
    CS_BATCH_HANDLE batch;
    CS_BATCH_INDEX first, second;
    OK(csAllocBatchRequest(CS_BATCH_HYBRID, 4, &batch));
    OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, &entry, 0, 0, &first));
    entry.u.CopyMem.u.HostVAddress = &values[1];
    OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, &entry, 0, first, &second));
    entry.u.CopyMem.Bytes = 0;
    CHECK(csConfigureBatchEntry(batch, CS_BATCH_JOIN, NULL, first, second, NULL) == CS_INVALID_ARG);
    u64 value = 0;
    OK(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, &value));
    OK(copy(a, &copied_value, 4, CS_COPY_FROM_DEVICE, 0));
    CHECK(value == 4 && copied_value == 17);
    CHECK(csFreeMem(a, &f) == CS_MEMORY_IN_USE);
    OK(csConfigureBatchEntry(batch, CS_BATCH_SPLIT, NULL, first, second, NULL));
    OK(csConfigureBatchEntry(batch, CS_BATCH_JOIN, NULL, second, first, NULL));
    CHECK(csHelperResizeBatchRequest(batch, 1) == CS_INVALID_LENGTH);
    OK(csHelperResizeBatchRequest(batch, 6));
    OK(csConfigureBatchEntry(batch, CS_BATCH_DELETE, NULL, second, 0, NULL));
    OK(csFreeBatchRequest(batch));
    CsCopyMemRequest write = {.Type = CS_COPY_TO_DEVICE, .DevMem = {a, 0}, .u.HostVAddress = values, .Bytes = 4};
    CHECK(csQueueCopyMemRequest(&write, (void *)123, callback, NULL, NULL, NULL) == CS_QUEUED);
    for (int i = 0; i < 3000 && !atomic_load(&callback_done); ++i) usleep(1000);
    CHECK(atomic_load(&callback_done));
    test_callback_sequence(a);
    test_batch_callback_requeue(a);
    CS_EVT_HANDLE event;
    OK(csCreateEvent(&event));
    CHECK(csQueueCopyMemRequest(&write, NULL, NULL, event, NULL, NULL) == CS_QUEUED);
    wait_event(event);
    OK(csDeleteEvent(event));
    // FEMU validates native scalar/MR restrictions before executing the sort.
    CsComputeRequest *compute = calloc(1, sizeof(*compute) + 3 * sizeof(CsComputeArg));
    compute->CSFId = sink;
    CHECK(csQueueComputeRequest(compute, NULL, NULL, NULL, NULL, NULL) == CS_ERROR_IN_EXECUTION);
    compute->NumArgs = 1;
    compute->Args[0].Type = CS_128BIT_VALUE_TYPE;
    CHECK(csQueueComputeRequest(compute, NULL, NULL, NULL, NULL, NULL) == CS_UNSUPPORTED_TYPE);
    compute->NumArgs = 3;
    for (int i = 0; i < 3; ++i) csHelperSetComputeArg(&compute->Args[i], CS_64BIT_VALUE_TYPE, (u64)0);
    CHECK(csQueueComputeRequest(compute, NULL, NULL, NULL, NULL, NULL) == CS_UNSUPPORTED);
    free(compute);
    /* Generic storage API maps LBA ranges to AFDM; no Guest buffer is involved. */
    CsStorageRequest storage = {
        .Mode = CS_STORAGE_BLOCK_IO,
        .DevHandle = d,
        .u.BlockIo = {
            .Type = CS_STORAGE_LOAD_TYPE, .DevMem = {a, 0}, .NumRanges = 1, .Range = {{1, 0, 1}}}};
    OK(csQueueStorageRequest(&storage, NULL, NULL, NULL, NULL, &value));
    CHECK(value == 512);
    OK(copy(a, buf, 16, CS_COPY_FROM_DEVICE, 0));
    CHECK(buf[0] == 0xa5);
    memset(buf, 0x37, sizeof(buf));
    OK(copy(a, buf, sizeof(buf), CS_COPY_TO_DEVICE, 0));
    storage.u.BlockIo.Type = CS_STORAGE_STORE_TYPE;
    storage.u.BlockIo.Range[0].StartLba = 8;
    OK(csQueueStorageRequest(&storage, NULL, NULL, NULL, NULL, &value));
    CHECK(value == 512);
    memset(buf, 0, sizeof(buf));
    OK(copy(a, buf, sizeof(buf), CS_COPY_TO_DEVICE, 0));
    storage.u.BlockIo.Type = CS_STORAGE_LOAD_TYPE;
    OK(csQueueStorageRequest(&storage, NULL, NULL, NULL, NULL, &value));
    OK(copy(a, buf, sizeof(buf), CS_COPY_FROM_DEVICE, 0));
    CHECK(buf[0] == 0x37 && buf[31] == 0x37);
    f.Flags = (CS_FDM_FLAG_TYPE)8;
    CHECK(csFreeMem(a, &f) == CS_INVALID_OPTION);
    OK(copy(a, buf, 16, CS_COPY_FROM_DEVICE, 0));
    f.Flags = 0;
    OK(csFreeMem(a, &f));
    CHECK(csFreeMem(a, &f) == CS_INVALID_HANDLE);
    OK(csFreeMem(b, &f));
    /* Multi-chunk overlapping software copies must preserve memmove semantics. */
    CS_MEM_HANDLE large;
    int large_bytes = (2 << 20) + 256;
    OK(csAllocMem(d, large_bytes, &f, &large, NULL));
    uint8_t *expected = malloc(large_bytes), *observed = malloc(large_bytes);
    CHECK(expected && observed);
    for (int i = 0; i < large_bytes; i++)
        expected[i] = (uint8_t)(i * 17 + i / 251);
    OK(copy(large, expected, large_bytes, CS_COPY_TO_DEVICE, 0));
    CsCopyMemRequest overlap = {.Type = CS_COPY_WITHIN_DEVICE,
        .u.SrcDevMem = {large, 0}, .DevMem = {large, 31}, .Bytes = large_bytes - 31};
    OK(csQueueCopyMemRequest(&overlap, NULL, NULL, NULL, NULL, NULL));
    memmove(expected + 31, expected, large_bytes - 31);
    OK(copy(large, observed, large_bytes, CS_COPY_FROM_DEVICE, 0));
    CHECK(!memcmp(expected, observed, large_bytes));
    overlap.u.SrcDevMem.ByteOffset = 31;
    overlap.DevMem.ByteOffset = 0;
    OK(csQueueCopyMemRequest(&overlap, NULL, NULL, NULL, NULL, NULL));
    memmove(expected, expected + 31, large_bytes - 31);
    OK(copy(large, observed, large_bytes, CS_COPY_FROM_DEVICE, 0));
    CHECK(!memcmp(expected, observed, large_bytes));
    OK(csFreeMem(large, &f));
    free(expected);
    free(observed);
    // Real sort CSFs replace the old scale/delay/spill demonstration functions.
    CsMemFlags clear = {.FDMId = 1, .Flags = CS_FDM_CLEAR};
    CS_MEM_HANDLE input, io, sorted, output;
    OK(csAllocMem(d, SORT_CHUNK_BYTES, &clear, &input, NULL));
    OK(csAllocMem(d, SORT_BLOCK_BYTES, &clear, &io, NULL));
    OK(csAllocMem(d, sizeof(struct sorted_chunk), &clear, &sorted, NULL));
    OK(csAllocMem(d, sizeof(struct average_result), &clear, &output, NULL));
    int32_t *data = malloc(SORT_CHUNK_BYTES);
    CHECK(data);
    CsBatchRequest *request = calloc(1, sizeof(*request) + 2 * sizeof(CsComputeArg));
    request->ReqType = CS_QUEUE_COMPUTE;
    request->u.Compute.CSFId = sink;
    request->u.Compute.NumArgs = 2;
    csHelperSetComputeArg(&request->u.Compute.Args[0], CS_AFDM_TYPE, input, 0UL);
    csHelperSetComputeArg(&request->u.Compute.Args[1], CS_AFDM_TYPE, io, 0UL);
    OK(csAllocBatchRequest(CS_BATCH_SERIAL, 1, &batch));
    OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, request, 0, 0, &first));
    int64_t expected_sum = 0;
    u64 state = 0;
    for (unsigned chunk = 0; chunk < 4; ++chunk) {
        for (unsigned i = 0; i < SORT_CHUNK_BYTES / 4; ++i) {
            data[i] = (int32_t)(i % 100) - 50 + (int32_t)chunk;
            expected_sum += data[i];
        }
        OK(copy(input, data, SORT_CHUNK_BYTES, CS_COPY_TO_DEVICE, 0));
        if(chunk==3) {
            /* Host copies select software scheduling; compute still spills on device. */
            OK(csHelperResizeBatchRequest(batch,2));
            CsBatchRequest host_copy={.ReqType=CS_COPY_AFDM,.u.CopyMem={
                .Type=CS_COPY_FROM_DEVICE,.DevMem={input,0},
                .u.HostVAddress=&copied_value,.Bytes=sizeof(copied_value)}};
            OK(csConfigureBatchEntry(batch,CS_BATCH_ADD,&host_copy,0,first,&second));
        }
        uint64_t commands=mock_graph_commands();
        OK(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, &value));
        CHECK(mock_graph_commands()==commands+1);
        if (!state) {
            state = value;
            CHECK(state > UINT32_MAX); // The first sink returns a complete 64-bit device handle.
            request->u.Compute.NumArgs = 3;
            csHelperSetComputeArg(&request->u.Compute.Args[2], CS_64BIT_VALUE_TYPE, state);
            OK(csConfigureBatchEntry(batch, CS_BATCH_RECONFIG, request, first, 0, NULL));
        } else if(chunk==3) CHECK(value==sizeof(copied_value) && (int32_t)copied_value==data[0]);
        else CHECK(value == state);
    }
    OK(csFreeBatchRequest(batch));
    request->u.Compute.CSFId = source;
    csHelperSetComputeArg(&request->u.Compute.Args[0], CS_AFDM_TYPE, sorted, 0UL);
    OK(csAllocBatchRequest(CS_BATCH_HYBRID, 2, &batch));
    OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, request, 0, 0, &first));
    request->u.Compute.CSFId = average;
    request->u.Compute.NumArgs = 2;
    csHelperSetComputeArg(&request->u.Compute.Args[1], CS_AFDM_TYPE, output, 0UL);
    OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, request, 0, first, &second));
    OK(csCreateEvent(&event));
    struct average_result result;
    do {
        CHECK(csQueueBatchRequest(batch, NULL, NULL, event, NULL, NULL) == CS_QUEUED);
        wait_event(event);
        OK(copy(output, &result, sizeof(result), CS_COPY_FROM_DEVICE, 0));
    } while (!result.finished);
    CHECK(result.count == 4ULL * SORT_OUTPUT_VALUES && result.sum == expected_sum);
    CHECK(result.average == (double)expected_sum / (double)result.count);
    CHECK(mock_hidden_used()==0);
    OK(csDeleteEvent(event));
    OK(csFreeBatchRequest(batch));
    OK(csFreeMem(output, &f)); OK(csFreeMem(sorted, &f));
    OK(csFreeMem(io, &f)); OK(csFreeMem(input, &f));
    free(data); free(request);
    CHECK(mock_used_bytes() == 0);
    OK(csCloseCSx(d));
    CHECK(csCloseCSx(d) == CS_INVALID_HANDLE);
    printf("PASS: %u API checks; memory-first sort across batches, spill/load/free and source EOF\n", checks);
    return 0;
}
