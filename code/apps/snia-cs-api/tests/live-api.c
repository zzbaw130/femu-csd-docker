/* Uses only the labelled femu-spill partition for temporary NVM writes. */
#include "cs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "spill-scenario.h"
#define OK(x)                                                                                      \
    do {                                                                                           \
        CS_STATUS s = (x);                                                                         \
        if (s != CS_SUCCESS) {                                                                     \
            fprintf(stderr, "%s: %d\n", #x, s);                                                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static CS_CSF_ID load(CS_DEV_HANDLE dev, const char *symbol, unsigned index)
{
    char descriptor[128] = "snia-cs-scale.so";
    strcpy(descriptor + strlen(descriptor) + 1, symbol);
    CsCSFDownloadInfo info = {.Type = CS_CSF_VENDOR_SPECIFIC,
                              .SubType = CS_SUBTYPE_VENDOR_SPECIFIC,
                              .CSEEId = 1,
                              .Index = index};
    snprintf(info.common.UniqueName, 32, "live-%u", index);
    info.common.DataBuffer = descriptor;
    info.common.Length = strlen("snia-cs-scale.so") + strlen(symbol) + 2;
    u32 id;
    OK(csCSFDownload(dev, &info, &id));
    CsConfigInfo config = {
        .Type = CS_CSF_ACTIVATE,
        .u.CSFActivateInfo = {.CSFId = id, .EEIId = 1, .State = CS_ACTIVE_STATE}};
    CsConfigData data;
    int len = sizeof(data);
    OK(csConfig(dev, &len, &config, &data));
    int count = 0;
    len = 0;
    assert(csGetCSFId(dev, info.common.UniqueName, 0, &len, &count, NULL) == CS_LENGTH_RETURNED);
    CSFIdInfo *out = calloc(1, len);
    OK(csGetCSFId(dev, info.common.UniqueName, 0, &len, &count, out));
    CS_CSF_ID f = out->CSFId;
    free(out);
    return f;
}
static void unload(CS_DEV_HANDLE d, unsigned index)
{
    CsConfigInfo config = {
        .Type = CS_CSF_ACTIVATE,
        .u.CSFActivateInfo = {.CSFId = index, .EEIId = 1, .State = CS_INACTIVE_STATE}};
    CsConfigData data;
    int len = sizeof(data);
    OK(csConfig(d, &len, &config, &data));
    CsCSFDownloadInfo info = {.CSEEId = 1, .Index = index, .common.Unload = 1};
    u32 id;
    OK(csCSFDownload(d, &info, &id));
}
int main(int argc, char **argv)
{
    CS_DEV_HANDLE d;
    OK(csOpenCSx(argc > 1 ? argv[1] : "/dev/nvme0n1", NULL, &d));
    int length = 0;
    assert(csQueryDeviceProperties(d, CS_CSE_TYPE, &length, NULL) == CS_INVALID_LENGTH);
    CsProperties *properties = calloc(1, length);
    OK(csQueryDeviceProperties(d, CS_CSE_TYPE, &length, properties));
    assert(properties->CSEDetails[0].CSE[0].NumCRs == 4);
    free(properties);
    CsStatsInfo stats;
    assert(csQueryDeviceStatistics(d, CS_STAT_CSx_MEM_USAGE, NULL, &stats) == CS_UNSUPPORTED);
    CsMemFlags flags = {.FDMId = 1, .Flags = CS_FDM_FILL, .FillValue = 0xaabbccdd};
    CS_MEM_HANDLE memory;
    OK(csAllocMem(d, 4096, &flags, &memory, NULL));
    unsigned char bytes[32];
    CsCopyMemRequest read = {
        .Type = CS_COPY_FROM_DEVICE, .u.HostVAddress = bytes, .DevMem = {memory, 0}, .Bytes = 32};
    OK(csQueueCopyMemRequest(&read, NULL, NULL, NULL, NULL, NULL));
    assert(bytes[0] == 0xdd && bytes[3] == 0xaa);
    CS_CSF_ID sum = load(d, "cs_scalar_sum", 401), pair = load(d, "cs_check_params", 402),
              scale = load(d, "cs_scale", 403);
    CsComputeRequest *r = calloc(1, sizeof(*r) + 64 * sizeof(CsComputeArg));
    r->CSFId = sum;
    r->NumArgs = 2;
    for (int i = 0; i < 2; i++)
        csHelperSetComputeArg(&r->Args[i], CS_64BIT_VALUE_TYPE, (u64)1);
    u64 result;
    OK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, &result));
    assert(result == 2);
    r->NumArgs = 0;
    OK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, &result));
    assert(result == 0);
    r->CSFId = pair;
    r->NumArgs = 2;
    csHelperSetComputeArg(&r->Args[0], CS_64BIT_VALUE_TYPE, (u64)7);
    csHelperSetComputeArg(&r->Args[1], CS_64BIT_VALUE_TYPE, (u64)3);
    OK(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, &result));
    assert(result == 1);
    r->CSFId = scale;
    r->NumArgs = 0;
    assert(csQueueComputeRequest(r, NULL, NULL, NULL, NULL, NULL) == CS_ERROR_IN_EXECUTION);
    /* Standard block LOAD uses the device's actual LBA size in the library. */
    CsStorageRequest storage = {.Mode = CS_STORAGE_BLOCK_IO,
                                .DevHandle = d,
                                .u.BlockIo = {.Type = CS_STORAGE_LOAD_TYPE,
                                              .DevMem = {memory, 0},
                                              .NumRanges = 1,
                                              .Range = {{1, 0, 1}}}};
    OK(csQueueStorageRequest(&storage, NULL, NULL, NULL, NULL, &result));
    assert(result > 0 && result <= 4096);
    CsCopyMemRequest within = {.Type = CS_COPY_WITHIN_DEVICE,
                               .u.SrcDevMem = {memory, 0},
                               .DevMem = {memory, 16},
                               .Bytes = 16};
    OK(csQueueCopyMemRequest(&within, NULL, NULL, NULL, NULL, NULL));
    CS_BATCH_HANDLE batch;
    CS_EVT_HANDLE event;
    OK(csAllocBatchRequest(CS_BATCH_PARALLEL, 4, &batch));
    OK(csCreateEvent(&event));
    CS_CSF_ID delays[4];
    for (unsigned i = 0; i < 4; i++) {
        delays[i] = load(d, "cs_delay", 404 + i);
        CsBatchRequest *req = calloc(1, sizeof(*req) + sizeof(CsComputeArg));
        req->ReqType = CS_QUEUE_COMPUTE;
        req->u.Compute.CSFId = delays[i];
        req->u.Compute.NumArgs = 2;
        csHelperSetComputeArg(&req->u.Compute.Args[0], CS_AFDM_TYPE, memory, (unsigned long)0);
        csHelperSetComputeArg(&req->u.Compute.Args[1], CS_64BIT_VALUE_TYPE, (u64)i * 16);
        CS_BATCH_INDEX index;
        OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, req, 0, 0, &index));
        free(req);
    }
    assert(csQueueBatchRequest(batch, NULL, NULL, event, NULL, NULL) == CS_QUEUED);
    CS_STATUS status = CS_NOT_DONE;
    for (unsigned i = 0; i < 3000; i++) {
        status = csPollEvent(event, NULL, NULL);
        if (status != CS_NOT_DONE)
            break;
        usleep(1000);
    }
    assert(status == CS_SUCCESS);
    uint64_t stamps[8];
    read.u.HostVAddress = stamps;
    read.Bytes = sizeof(stamps);
    OK(csQueueCopyMemRequest(&read, NULL, NULL, NULL, NULL, NULL));
    unsigned max_busy = 0;
    for (unsigned i = 0; i < 4; i++) {
        assert(stamps[2 * i] && stamps[2 * i + 1] > stamps[2 * i]);
        unsigned busy = 0;
        for (unsigned j = 0; j < 4; j++)
            busy += stamps[2 * j] <= stamps[2 * i] && stamps[2 * i] < stamps[2 * j + 1];
        if (busy > max_busy)
            max_busy = busy;
    }
    assert(max_busy >= 2); /* Run this integration profile with four native FEMU CUs. */
    OK(csFreeBatchRequest(batch));
    OK(csDeleteEvent(event));
    CS_CSF_ID round = load(d, "cs_spill_roundtrip", 410),
              store = load(d, "cs_spill_store", 411),
              source = load(d, "cs_spill_load", 412),
              fail = load(d, "cs_spill_fail", 413),
              result64 = load(d, "cs_result64", 414);
    spill_scenario(d, round, store, source, fail, result64);
    CS_CSF_ID round2 = load(d, "cs_spill_roundtrip", 415),
              validate = load(d, "cs_spill_validate", 416);
    spill_graph(d, round, round2, validate);
    for (unsigned i = 410; i <= 416; i++)
        unload(d, i);
    for (unsigned i = 401; i <= 407; i++)
        unload(d, i);
    flags.Flags = 0;
    OK(csFreeMem(memory, &flags));
    OK(csCloseCSx(d));
    free(r);
    printf("LIVE PASS: native MR/cparam arguments, scalar-only, independent 64-bit cparams, native error, FDM, storage LOAD, %u "
           "overlapping native CSFs, result64, internal STORE/LOAD, cross-batch spill, cleanup\n",
           max_busy);
    return 0;
}
