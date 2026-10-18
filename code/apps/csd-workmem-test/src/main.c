/* Guest-only client: validates device workmem through SNIA APIs, not local linking. */
#include "cs.h"
#include "workmem-probe.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FIRST 63000U
#define CHECK(expr) do { CS_STATUS s = (expr); if (s != CS_SUCCESS) { \
    fprintf(stderr, "%s: status=%d\n", #expr, s); exit(1); } } while (0)
static CS_CSF_ID load(CS_DEV_HANDLE dev, unsigned index, const char *file, const char *symbol)
{
    char descriptor[128];
    size_t first = strlen(file) + 1;
    strcpy(descriptor, file);
    strcpy(descriptor + first, symbol);
    CsCSFDownloadInfo info = {.Type = CS_CSF_VENDOR_SPECIFIC,
        .SubType = CS_SUBTYPE_VENDOR_SPECIFIC, .CSEEId = 1, .Index = index};
    snprintf(info.common.UniqueName, 32, "workmem-%u", index);
    info.common.DataBuffer = descriptor;
    info.common.Length = first + strlen(symbol) + 1;
    u32 id;
    CHECK(csCSFDownload(dev, &info, &id));
    CsConfigInfo config = {.Type = CS_CSF_ACTIVATE,
        .u.CSFActivateInfo = {.CSFId = id, .EEIId = 1, .State = CS_ACTIVE_STATE}};
    CsConfigData data;
    int n = sizeof(data);
    CHECK(csConfig(dev, &n, &config, &data));
    int count;
    n = 0;
    assert(csGetCSFId(dev, info.common.UniqueName, 0, &n, &count, NULL) == CS_INVALID_LENGTH);
    CSFIdInfo *result = calloc(1, n);
    assert(result);
    CHECK(csGetCSFId(dev, info.common.UniqueName, 0, &n, &count, result));
    CS_CSF_ID handle = result->CSFId;
    free(result);
    return handle;
}
static u64 scalar(CS_CSF_ID f)
{
    CsComputeRequest req = {.CSFId = f, .NumArgs = 0};
    u64 result;
    CHECK(csQueueComputeRequest(&req, NULL, NULL, NULL, NULL, &result));
    return result;
}
int main(int argc, char **argv)
{
    CS_DEV_HANDLE dev;
    CHECK(csOpenCSx(argc > 1 ? argv[1] : "/dev/nvme0n1", NULL, &dev));
    CS_CSF_ID functions[4];
    for (unsigned i = 0; i < 4; i++)
        functions[i] = load(dev, FIRST + i, i % 2 ? "csd-workmem-probe-b.so" :
                            "csd-workmem-probe-a.so", "cs_workmem_probe");
    CS_CSF_ID used = load(dev, FIRST + 4, "csd-workmem-probe-a.so", "cs_workmem_used");
    CS_CSF_ID capacity_fn = load(dev, FIRST + 5, "csd-workmem-probe-a.so", "cs_workmem_capacity");
    u64 capacity = scalar(capacity_fn), bytes = capacity * 3 / 8;
    assert(capacity >= (1U << 20) && scalar(used) == 0);
    CsMemFlags flags = {.FDMId = 1, .Flags = CS_FDM_CLEAR};
    CS_MEM_HANDLE memory;
    CHECK(csAllocMem(dev, sizeof(struct workmem_probe_control), &flags, &memory, NULL));
    CS_BATCH_HANDLE batch;
    CHECK(csAllocBatchRequest(CS_BATCH_PARALLEL, 4, &batch));
    for (unsigned i = 0; i < 4; i++) {
        CsBatchRequest *req = calloc(1, sizeof(*req) + 4 * sizeof(CsComputeArg));
        assert(req);
        req->ReqType = CS_QUEUE_COMPUTE;
        req->u.Compute.CSFId = functions[i];
        req->u.Compute.NumArgs = 4;
        csHelperSetComputeArg(&req->u.Compute.Args[0], CS_AFDM_TYPE, memory, 0UL);
        csHelperSetComputeArg(&req->u.Compute.Args[1], CS_32BIT_VALUE_TYPE, (u32)i);
        csHelperSetComputeArg(&req->u.Compute.Args[2], CS_64BIT_VALUE_TYPE, bytes);
        csHelperSetComputeArg(&req->u.Compute.Args[3], CS_32BIT_VALUE_TYPE, (u32)4);
        CS_BATCH_INDEX index;
        CHECK(csConfigureBatchEntry(batch, CS_BATCH_ADD, req, 0, 0, &index));
        free(req);
    }
    for (unsigned round = 0; round < 2; round++) {
        CHECK(csInitMem(memory, 0, sizeof(struct workmem_probe_control), &flags));
        CHECK(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, NULL));
        struct workmem_probe_control report;
        CsCopyMemRequest read = {.Type = CS_COPY_FROM_DEVICE,
            .DevMem = {memory, 0}, .u.HostVAddress = &report, .Bytes = sizeof(report)};
        CHECK(csQueueCopyMemRequest(&read, NULL, NULL, NULL, NULL, NULL));
        unsigned success = 0;
        for (unsigned i = 0; i < 4; i++) {
            struct workmem_probe_result *r = &report.slots[i];
            assert(r->capacity == capacity && r->peak <= capacity &&
                   r->used <= capacity && r->requested == bytes * 2 &&
                   r->used == report.slots[0].used && r->variant == i % 2 + 1);
            success += r->allocated;
            assert(r->allocated || r->error == ENOMEM);
        }
        assert(success == 2 && scalar(used) == 0);
        printf("WORKMEM LIVE round %u PASS: 4 CUs / 2 .so, pool=%llu MiB, "
               "2 allocations granted, 2 ENOMEM, peak=%llu, used-after=0\n",
               round + 1, (unsigned long long)(capacity >> 20),
               (unsigned long long)report.slots[0].peak);
    }
    CHECK(csFreeBatchRequest(batch));
    flags.Flags = 0;
    CHECK(csFreeMem(memory, &flags));
    for (unsigned i = 0; i < 6; i++) {
        CsConfigInfo config = {.Type = CS_CSF_ACTIVATE,
            .u.CSFActivateInfo = {.CSFId = FIRST + i, .EEIId = 1, .State = CS_INACTIVE_STATE}};
        CsConfigData data;
        int n = sizeof(data);
        CHECK(csConfig(dev, &n, &config, &data));
        CsCSFDownloadInfo info = {.CSEEId = 1, .Index = FIRST + i, .common.Unload = 1};
        u32 id;
        CHECK(csCSFDownload(dev, &info, &id));
    }
    CHECK(csCloseCSx(dev));
}
