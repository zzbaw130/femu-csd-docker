#include "cs.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define OK(call) assert((call) == CS_SUCCESS)
static CS_CSF_ID function(CS_DEV_HANDLE dev, const char *name)
{
    CSFIdinfo info;
    int n = sizeof(info), count = 0;
    OK(csGetCSFId(dev, name, 0, &n, &count, &info));
    assert(count == 1); return info.CSFId;
}
int main(void)
{
    CsPluginRequest plugin = {.Type = CS_PLUGIN_CUSTOM};
    strcpy(plugin.PluginPath, CS_TEST_PLUGIN);
    OK(csRegisterPlugin(&plugin));
    CsPluginRequest wrong = plugin; wrong.Type = CS_PLUGIN_NVME;
    assert(csRegisterPlugin(&wrong) == CS_UNSUPPORTED_TYPE);
    CsPluginRequest missing = {.Type = CS_PLUGIN_CUSTOM, .PluginPath = "/no-such-snia-plugin.so"};
    assert(csRegisterPlugin(&missing) == CS_LOAD_ERROR);
    char list[256]; int n = sizeof(list);
    OK(csQueryCSxList(&n, list));
    assert(strstr(list, "mock://device") && strstr(list, "/dev/nvme0n1"));
    unsigned size = sizeof(list);
    OK(csGetCSxFromPath("mock://device", &size, list));
    assert(!strcmp(list, "mock://device"));
    CS_EVT_HANDLE event; CS_BATCH_HANDLE batch;
    OK(csCreateEvent(&event)); OK(csAllocBatchRequest(CS_BATCH_HYBRID, 2, &batch));
    CS_DEV_HANDLE dev, femu;
    OK(csOpenCSx("mock://device", NULL, &dev));
    OK(csOpenCSx("/dev/nvme0n1", NULL, &femu));
    assert(csDeregisterPlugin(&plugin) == CS_HANDLE_IN_USE);
    OK(csRegisterPlugin(&plugin)); /* Repeated registration retains existing state. */
    CsMemFlags flags = {.FDMId = 9};
    CS_MEM_HANDLE mem;
    OK(csAllocMem(dev, 32, &flags, &mem, NULL));
    char data[] = "abcdefg", result[8] = {0};
    CsCopyMemRequest copy = {.Type = CS_COPY_TO_DEVICE, .u.HostVAddress = data, .DevMem = {mem, 3}, .Bytes = 8};
    OK(csQueueCopyMemRequest(&copy, NULL, NULL, NULL, NULL, NULL));
    copy.Type = CS_COPY_FROM_DEVICE; copy.u.HostVAddress = result;
    OK(csQueueCopyMemRequest(&copy, NULL, NULL, NULL, NULL, NULL));
    assert(!memcmp(data, result, 8));
    CsBatchRequest *req = calloc(1, offsetof(CsBatchRequest, u.Compute.Args) + 4 * sizeof(CsComputeArg));
    req->ReqType = CS_QUEUE_COMPUTE;
    req->u.Compute.CSFId = function(dev, "sum"); req->u.Compute.NumArgs = 4;
    csHelperSetComputeArg(&req->u.Compute.Args[0], CS_AFDM_TYPE, mem, 3UL);
    for (int i = 1; i < 4; i++) csHelperSetComputeArg(&req->u.Compute.Args[i], CS_64BIT_VALUE_TYPE, (u64)i);
    u64 value;
    OK(csQueueComputeRequest(&req->u.Compute, NULL, NULL, NULL, NULL, &value));
    assert(value == 'a' + 6); /* Non-FEMU offset and three scalars work unchanged. */
    CS_BATCH_INDEX first, second;
    OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, req, 0, 0, &first));
    req->u.Compute.CSFId = function(dev, "reserved"); req->u.Compute.NumArgs = 0;
    OK(csConfigureBatchEntry(batch, CS_BATCH_ADD, req, 0, first, &second));
    OK(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, &value));
    assert(value == ((UINT64_C(1) << 60) | 128)); /* No FEMU reentry protocol in core. */
    assert(csCloseCSx(dev) == CS_HANDLE_IN_USE);
    assert(csFreeMem(mem, &flags) == CS_MEMORY_IN_USE);
    CS_REQ_HANDLE request;
    assert(csQueueBatchRequest(batch, NULL, NULL, event, &request, NULL) == CS_QUEUED);
    assert(csDeregisterPlugin(&plugin) == CS_HANDLE_IN_USE);
    CS_STATUS s;
    do { s = csPollEvent(event, NULL, &value); if (s == CS_NOT_DONE) usleep(1000); } while (s == CS_NOT_DONE);
    assert(s == CS_SUCCESS && value == ((UINT64_C(1) << 60) | 128));
    OK(csDeleteEvent(event)); OK(csFreeBatchRequest(batch));
    CsStorageRequest bad = {.Mode = CS_STORAGE_BLOCK_IO, .DevHandle = femu,
        .u.BlockIo = {.Type = CS_STORAGE_LOAD_TYPE, .DevMem = {mem, 0}, .NumRanges = 1, .Range = {{1,0,1}}}};
    assert(csQueueStorageRequest(&bad, NULL, NULL, NULL, NULL, NULL) == CS_INVALID_ARG);
    OK(csFreeMem(mem, &flags)); OK(csCloseCSx(dev)); OK(csCloseCSx(femu)); free(req);
    OK(csDeregisterPlugin(&plugin));
    assert(csOpenCSx("mock://device", NULL, &dev) == CS_DEVICE_NOT_AVAILABLE);
    OK(csRegisterPlugin(&plugin)); OK(csOpenCSx("mock://device", NULL, &dev));
    OK(csCloseCSx(dev)); OK(csDeregisterPlugin(&plugin));
    puts("PASS: two runtime adapters, opaque handle routing, generic async/batch, unload ownership");
    return 0;
}
