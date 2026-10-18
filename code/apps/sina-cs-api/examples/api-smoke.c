/* Guest application: no NVMe structs or ioctl calls. */
#include "cs.sh"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        CS_STATUS status = (expr);                                                                 \
        if (status != CS_SUCCESS) {                                                                \
            fprintf(stderr, "%s: status=%d\n", #expr, status);                                     \
            goto cleanup;                                                                          \
        }                                                                                          \
    } while (0)
int main(int argc, char **argv)
{
    CS_DEV_HANDLE dev = NULL;
    CS_MEM_HANDLE in = NULL, out = NULL;
    CS_BATCH_HANDLE batch = NULL;
    CS_EVT_HANDLE event = NULL;
    CsMemFlags flags = {.FDMId = 1, .Flags = CS_FDM_CLEAR};
    int rc = 1;
    u32 index = 400;
    int loaded = 0, active = 0;
    const char descriptor[] = "sina-cs-scale.so\0cs_scale";
    CsCSFDownloadInfo dl = {.Type = CS_CSF_VENDOR_SPECIFIC,
                            .SubType = CS_SUBTYPE_VENDOR_SPECIFIC,
                            .CSEEId = 1,
                            .Index = index,
                            .common = {.UniqueName = "sina-scale",
                                       .Length = sizeof(descriptor),
                                       .DataBuffer = (void *)descriptor}};
    CsConfigInfo config = {
        .Type = CS_CSF_ACTIVATE,
        .u.CSFActivateInfo = {.CSFId = index, .EEIId = 1, .State = CS_ACTIVE_STATE}};
    REQUIRE(csOpenCSx(argc > 1 ? argv[1] : "/dev/nvme0n1", NULL, &dev));
    REQUIRE(csCSFDownload(dev, &dl, &index));
    loaded = 1;
    int size = sizeof(CsConfigData);
    CsConfigData activated;
    REQUIRE(csConfig(dev, &size, &config, &activated));
    active = 1;
    int length = 0, count = 0;
    CS_STATUS status = csGetCSFId(dev, "sina-scale", 0, &length, &count, NULL);
    if (status != CS_LENGTH_RETURNED)
        goto cleanup;
    CSFIdInfo *functions = calloc(1, length);
    if (!functions)
        goto cleanup;
    status = csGetCSFId(dev, "sina-scale", 0, &length, &count, functions);
    CS_CSF_ID function = status == CS_SUCCESS ? functions[0].CSFId : NULL;
    free(functions);
    if (!function)
        goto cleanup;
    REQUIRE(csAllocMem(dev, 4096, &flags, &in, NULL));
    REQUIRE(csAllocMem(dev, 4096, &flags, &out, NULL));
    REQUIRE(csAllocBatchRequest(CS_BATCH_SERIAL, 4, &batch));
    uint32_t input[4] = {1, 3, 7, 11}, result[4] = {0};
    CsBatchRequest copy_in = {.ReqType = CS_COPY_AFDM,
                              .u.CopyMem = {.Type = CS_COPY_TO_DEVICE,
                                            .u.HostVAddress = input,
                                            .DevMem = {in, 4},
                                            .Bytes = sizeof(input)}};
    CsBatchRequest *compute = calloc(1, sizeof(*compute) + 2 * sizeof(CsComputeArg));
    if (!compute)
        goto cleanup;
    compute->ReqType = CS_QUEUE_COMPUTE;
    compute->u.Compute.CSFId = function;
    compute->u.Compute.NumArgs = 3;
    csHelperSetComputeArg(&compute->u.Compute.Args[0], CS_AFDM_TYPE, in, 4UL);
    csHelperSetComputeArg(&compute->u.Compute.Args[1], CS_AFDM_TYPE, out, 8UL);
    csHelperSetComputeArg(&compute->u.Compute.Args[2], CS_32BIT_VALUE_TYPE, (u32)4);
    CsBatchRequest copy_out = {.ReqType = CS_COPY_AFDM,
                               .u.CopyMem = {.Type = CS_COPY_FROM_DEVICE,
                                             .u.HostVAddress = result,
                                             .DevMem = {out, 8},
                                             .Bytes = sizeof(result)}};
    CS_BATCH_INDEX a, b, c, d;
    status = csConfigureBatchEntry(batch, CS_BATCH_ADD, &copy_in, 0, 0, &a);
    if (status == CS_SUCCESS)
        status = csConfigureBatchEntry(batch, CS_BATCH_ADD, compute, 0, a, &b);
    compute->u.Compute.Args[0].u.DevMem = (CsDevAFDM){out, 8};
    if (status == CS_SUCCESS)
        status = csConfigureBatchEntry(batch, CS_BATCH_ADD, compute, 0, b, &c);
    free(compute);
    if (status != CS_SUCCESS)
        goto cleanup;
    REQUIRE(csConfigureBatchEntry(batch, CS_BATCH_ADD, &copy_out, 0, c, &d));
    REQUIRE(csCreateEvent(&event));
    if (csQueueBatchRequest(batch, (void *)input, NULL, event, NULL, NULL) != CS_QUEUED)
        goto cleanup;
    do {
        status = csPollEvent(event, NULL, NULL);
        if (status == CS_NOT_DONE)
            usleep(1000);
    } while (status == CS_NOT_DONE);
    if (status != CS_SUCCESS)
        goto cleanup;
    for (unsigned i = 0; i < 4; i++)
        if (result[i] != input[i] * 4)
            goto cleanup;
    printf("SNIA API batch PASS: %u %u %u %u\n", result[0], result[1], result[2], result[3]);
    rc = 0;
cleanup:
    if (event)
        csDeleteEvent(event);
    if (batch)
        csFreeBatchRequest(batch);
    flags.Flags = 0;
    if (out)
        csFreeMem(out, &flags);
    if (in)
        csFreeMem(in, &flags);
    if (active) {
        config.u.CSFActivateInfo.State = CS_INACTIVE_STATE;
        int n = sizeof(CsConfigData);
        CsConfigData data;
        csConfig(dev, &n, &config, &data);
    }
    if (loaded) {
        dl.common.Unload = 1;
        csCSFDownload(dev, &dl, &index);
    }
    if (dev)
        csCloseCSx(dev);
    return rc;
}
