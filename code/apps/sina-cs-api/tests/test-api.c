#include "cs.sh"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "spill-scenario.h"
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
extern atomic_uint mock_active, mock_peak;
extern uint64_t mock_used_bytes(void);
extern uint64_t mock_allocation_count(void);
static CS_CSF_ID load(CS_DEV_HANDLE d, const char *symbol, const char *name, unsigned index)
{
    char descriptor[128] = "sina-cs-scale.so";
    strcpy(descriptor + strlen(descriptor) + 1, symbol);
    CsCSFDownloadInfo info = {.Type = CS_CSF_VENDOR_SPECIFIC,
                              .SubType = CS_SUBTYPE_VENDOR_SPECIFIC,
                              .CSEEId = 1,
                              .Index = index};
    strcpy(info.common.UniqueName, name);
    info.common.DataBuffer = descriptor;
    info.common.Length = strlen("sina-cs-scale.so") + strlen(symbol) + 2;
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
    CHECK(value == 21);
    atomic_store(&callback_done, 1);
}
int main(void)
{
    CS_DEV_HANDLE d;
    CsMemFlags f = {.FDMId = 1, .Flags = CS_FDM_CLEAR};
    CS_MEM_HANDLE a, b;
    CHECK(csCloseCSx((void *)1) == CS_INVALID_HANDLE);
    CHECK(csCreateEvent(NULL) == CS_INVALID_ARG);
    unsetenv("SINA_CS_FEMU_DEVICES");
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
    CS_CSF_ID scale = load(d, "cs_scale", "scale", 201), sum = load(d, "cs_scalar_sum", "sum", 202),
              delay = load(d, "cs_delay", "delay", 203);
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
    CHECK(props->CSFDetails[0].NumCSFs == 3);
    free(props);
    CsComputeRequest *r = calloc(1, sizeof(*r) + 64 * sizeof(CsComputeArg));
    r->CSFId = sum;
    r->NumArgs = 2;
    csHelperSetComputeArg(&r->Args[0], CS_8BIT_VALUE_TYPE, 1);
    csHelperSetComputeArg(&r->Args[1], CS_16BIT_VALUE_TYPE, 20);
    u64 value = 0;
    uint64_t allocations = mock_allocation_count();
    OK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, &value));
    CHECK(value == 21);
    CHECK(mock_used_bytes() == 8192);
    CHECK(mock_allocation_count() == allocations); /* No hidden argument AFDM. */
    r->NumArgs = 3;
    csHelperSetComputeArg(&r->Args[2], CS_32BIT_VALUE_TYPE, (u32)99);
    CHECK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, &value) == CS_UNSUPPORTED);
    r->NumArgs = 1;
    csHelperSetComputeArg(&r->Args[0], CS_128BIT_VALUE_TYPE, ((u128)3 << 64) | 7);
    OK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, &value));
    CHECK(value == 10); /* One 128-bit value occupies native cparam1/2. */
    r->NumArgs = 2;
    csHelperSetComputeArg(&r->Args[0], CS_32BIT_VALUE_TYPE, (u32)1);
    csHelperSetComputeArg(&r->Args[1], CS_64BIT_VALUE_TYPE, (u64)20);
    CS_DEV_HANDLE second_handle;
    OK(csOpenCSx("/dev/nvme0n1", NULL, &second_handle));
    n = 0;
    CHECK(csQueryDeviceProperties(second_handle, CS_CSF_TYPE, &n, NULL) == CS_INVALID_LENGTH);
    props = calloc(1, n);
    OK(csQueryDeviceProperties(second_handle, CS_CSF_TYPE, &n, props));
    CHECK(props->CSFDetails[0].NumCSFs == 3);
    free(props);
    OK(csCloseCSx(second_handle));
    CHECK(csCloseCSx(d) == CS_HANDLE_IN_USE);
    OK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, &value));
    CHECK(value == 21);
    r->CSFId = scale;
    r->NumArgs = 0;
    CHECK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, NULL) == CS_ERROR_IN_EXECUTION);
    CHECK(mock_used_bytes() == 8192); /* Errors also allocate no hidden AFDM. */
    r->CSFId = sum;
    r->NumArgs = 65;
    CHECK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, NULL) == CS_INVALID_ARG);
    r->NumArgs = 2;
    CHECK(csQueueComputeRequest(r, NULL, callback, (void *)1, NULL, NULL) == CS_INVALID_ARG);
    CHECK(csQueueComputeRequest(r, (void *)123, callback, NULL, NULL, NULL) == CS_QUEUED);
    for (int i = 0; i < 3000 && !atomic_load(&callback_done); i++)
        usleep(1000);
    CHECK(atomic_load(&callback_done));
    CS_EVT_HANDLE event;
    OK(csCreateEvent(&event));
    CHECK(csPollEvent(event, NULL, NULL) == CS_NOT_DONE);
    r->CSFId = scale;
    r->NumArgs = 4;
    csHelperSetComputeArg(&r->Args[0], CS_AFDM_TYPE, a, 4UL);
    csHelperSetComputeArg(&r->Args[1], CS_AFDM_TYPE, b, 0UL);
    csHelperSetComputeArg(&r->Args[2], CS_32BIT_VALUE_TYPE, (u32)4);
    csHelperSetComputeArg(&r->Args[3], CS_64BIT_VALUE_TYPE, ((u64)8 << 32) | 4);
    CHECK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, NULL) == CS_UNSUPPORTED);
    csHelperSetComputeArg(&r->Args[0], CS_AFDM_TYPE, a, 0UL);
    uint32_t input[] = {2, 4, 6, 8}, output[4];
    OK(copy(a, input, sizeof(input), CS_COPY_TO_DEVICE, 4));
    OK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, &value));
    OK(copy(b, output, sizeof(output), CS_COPY_FROM_DEVICE, 8));
    CHECK(output[0] == 4 && output[3] == 16);
    /* Deep-copy descriptors, reject cycles without corrupting the graph. */
    CS_BATCH_HANDLE batch;
    OK(csAllocBatchRequest(CS_BATCH_HYBRID, 4, &batch));
    CsBatchRequest *entry = calloc(1, sizeof(*entry) + 3 * sizeof(CsComputeArg));
    entry->ReqType = CS_QUEUE_COMPUTE;
    entry->u.Compute.CSFId = sum;
    entry->u.Compute.NumArgs = 1;
    csHelperSetComputeArg(&entry->u.Compute.Args[0], CS_64BIT_VALUE_TYPE, (u64)9);
    CS_BATCH_INDEX first, second;
    OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, entry, 0, 0, &first));
    entry->u.Compute.Args[0].u.Value64 = 17;
    OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, entry, 0, first, &second));
    entry->u.Compute.Args[0].u.Value64 = 999;
    CHECK(csConfigureBatchEntry(batch, CS_BATCH_JOIN, NULL, first, second, NULL) == CS_INVALID_ARG);
    OK(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, &value));
    CHECK(value == 17);
    OK(csConfigureBatchEntry(batch, CS_BATCH_SPLIT, NULL, first, second, NULL));
    OK(csConfigureBatchEntry(batch, CS_BATCH_JOIN, NULL, second, first, NULL));
    entry->u.Compute.Args[0].u.Value64 = 23;
    OK(csConfigureBatchEntry(batch, CS_BATCH_RECONFIG, entry, second, 0, NULL));
    OK(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, &value));
    CHECK(value == 23);
    CHECK(csHelperResizeBatchRequest(batch, 1) == CS_INVALID_LENGTH);
    OK(csHelperResizeBatchRequest(batch, 6));
    OK(csConfigureBatchEntry(batch, CS_BATCH_DELETE, NULL, second, 0, NULL));
    OK(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, &value));
    CHECK(value == 9);
    OK(csFreeBatchRequest(batch));
    /* Four independent delay CSFs overlap, rather than silently running serially. */
    OK(csAllocBatchRequest(CS_BATCH_PARALLEL, 4, &batch));
    entry->u.Compute.CSFId = delay;
    entry->u.Compute.NumArgs = 0;
    CS_CSF_ID delays[4] = {delay, load(d, "cs_delay", "delay-2", 204),
                           load(d, "cs_delay", "delay-3", 205),
                           load(d, "cs_delay", "delay-4", 206)};
    for (unsigned i = 0; i < 4; i++) {
        entry->u.Compute.CSFId = delays[i];
        CS_BATCH_INDEX index;
        OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, entry, 0, 0, &index));
    }
    CHECK(csQueueBatchRequest(batch, NULL, NULL, event, NULL, NULL) == CS_QUEUED);
    CHECK(csFreeBatchRequest(batch) == CS_HANDLE_IN_USE);
    CHECK(csDeleteEvent(event) == CS_HANDLE_IN_USE);
    wait_event(event);
    CHECK(atomic_load(&mock_peak) >= 2);
    OK(csConfigureBatchEntry(batch, CS_BATCH_RECONFIG, entry, 1, 0, NULL));
    OK(csConfigureBatchEntry(batch, CS_BATCH_DELETE, NULL, 2, 0, NULL));
    OK(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, NULL));
    OK(csFreeBatchRequest(batch));
    /* In-flight AFDM ownership and explicit unsupported running abort. */
    r->CSFId = delay;
    r->NumArgs = 1;
    csHelperSetComputeArg(&r->Args[0], CS_AFDM_TYPE, a, 0UL);
    CS_REQ_HANDLE request;
    CHECK(csQueueComputeRequest(r, NULL, NULL, event, &request, NULL) == CS_QUEUED);
    CHECK(csFreeMem(a, &f) == CS_MEMORY_IN_USE);
    while (!atomic_load(&mock_active))
        usleep(100);
    CHECK(csAbortRequest(request) == CS_UNSUPPORTED);
    wait_event(event);
    CHECK(csAbortRequest(request) == CS_INVALID_HANDLE);
    OK(csDeleteEvent(event));
    CHECK(csDeleteEvent(event) == CS_INVALID_HANDLE);
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
    CsStatsInfo stats;
    CHECK(csQueryDeviceStatistics(d, CS_STAT_CSx_MEM_USAGE, NULL, &stats) == CS_UNSUPPORTED);
    CS_CSF_ID round = load(d, "cs_spill_roundtrip", "spill-round", 210),
              store = load(d, "cs_spill_store", "spill-store", 211),
              source = load(d, "cs_spill_load", "spill-load", 212),
              fail = load(d, "cs_spill_fail", "spill-fail", 213),
              wide = load(d, "cs_result64", "result64", 214);
    spill_scenario(d, round, store, source, fail, wide);
    CS_CSF_ID round2 = load(d, "cs_spill_roundtrip", "spill-round2", 215),
              validate = load(d, "cs_spill_validate", "spill-validate", 216);
    spill_graph(d, round, round2, validate);
    CHECK(mock_used_bytes() == 0);
    OK(csCloseCSx(d));
    CHECK(csCloseCSx(d) == CS_INVALID_HANDLE);
    free(entry);
    free(r);
    printf("PASS: %u API checks\n", checks);
    return 0;
}
