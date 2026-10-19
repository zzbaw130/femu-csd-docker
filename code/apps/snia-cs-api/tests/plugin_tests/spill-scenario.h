/* Shared mock/live scenario: no host copies of spilled payloads. */
#include "cs_csf.h"
static u64 spill_batch(CS_CSF_ID csf, CS_MEM_HANDLE mem, u64 token, int with_token,
                       CS_STATUS expected)
{
    CsBatchRequest *r = calloc(1, sizeof(*r) + 3 * sizeof(CsComputeArg));
    assert(r);
    r->ReqType = CS_QUEUE_COMPUTE;
    r->u.Compute.CSFId = csf;
    r->u.Compute.NumArgs = 1 + with_token;
    if (with_token)
        csHelperSetComputeArg(&r->u.Compute.Args[0], CS_64BIT_VALUE_TYPE, token);
    csHelperSetComputeArg(&r->u.Compute.Args[with_token], CS_AFDM_TYPE, mem, (unsigned long)0);
    CS_BATCH_HANDLE b;
    assert(csAllocBatchRequest(CS_BATCH_SERIAL, 1, &b) == CS_SUCCESS);
    CS_BATCH_INDEX index;
    assert(csConfigureBatchEntry(b, CS_BATCH_ADD, r, 0, 0, &index) == CS_SUCCESS);
    u64 value = 0;
    assert(csQueueBatchRequest(b, NULL, NULL, NULL, NULL, &value) == expected);
    assert(csFreeBatchRequest(b) == CS_SUCCESS);
    free(r);
    return value;
}
static void spill_scenario(CS_DEV_HANDLE d, CS_CSF_ID round, CS_CSF_ID store,
                            CS_CSF_ID load, CS_CSF_ID fail, CS_CSF_ID wide)
{
    CsMemFlags flags = {.FDMId = 1};
    CS_MEM_HANDLE io;
    assert(csAllocMem(d, 8192, &flags, &io, NULL) == CS_SUCCESS);
    CsComputeRequest r = {.CSFId = wide, .NumArgs = 1};
    u64 value = UINT64_C(0x4353123456789abc), result = 0;
    csHelperSetComputeArg(&r.Args[0], CS_64BIT_VALUE_TYPE, value);
    assert(csQueueComputeRequest(&r, NULL, NULL, NULL, NULL, &result) == CS_SUCCESS);
    assert(result == value); /* Positive low word exceeds INT32_MAX in other cases too. */
    csHelperSetComputeArg(&r.Args[0], CS_64BIT_VALUE_TYPE, UINT64_C(0x12345678fedcba98));
    assert(csQueueComputeRequest(&r, NULL, NULL, NULL, NULL, &result) == CS_SUCCESS);
    assert(result == UINT64_C(0x12345678fedcba98));
    value = spill_batch(round, io, 0, 0, CS_SUCCESS);
    assert(value == 8191);
    value = spill_batch(store, io, 0, 0, CS_SUCCESS);
    u64 token = value;
    assert(token);
    /* A failed later batch must preserve this earlier batch's retained run. */
    spill_batch(fail, io, 0, 0, CS_ERROR_IN_EXECUTION);
    value = spill_batch(load, io, token, 1, CS_SUCCESS);
    assert(value == 8191);
    spill_batch(load, io, token, 1, CS_INVALID_ID); /* Freed tokens cannot be reused. */
    /* The mock arena is only 1 MiB. Repeated failures expose leaked extents. */
    for (unsigned i = 0; i < 140; i++)
        spill_batch(fail, io, 0, 0, CS_ERROR_IN_EXECUTION);
    value = spill_batch(round, io, 0, 0, CS_SUCCESS);
    assert(value == 8191);
    assert(csFreeMem(io, &flags) == CS_SUCCESS);
}

static void spill_graph(CS_DEV_HANDLE d, CS_CSF_ID round1, CS_CSF_ID round2,
                         CS_CSF_ID validate)
{
    CsMemFlags flags = {.FDMId = 1};
    CS_MEM_HANDLE io[2];
    CS_BATCH_HANDLE batch;
    assert(csAllocBatchRequest(CS_BATCH_HYBRID, 3, &batch) == CS_SUCCESS);
    CS_BATCH_INDEX nodes[3];
    CsBatchRequest *r = calloc(1, sizeof(*r) + 2 * sizeof(CsComputeArg));
    assert(r);
    r->ReqType = CS_QUEUE_COMPUTE;
    r->u.Compute.NumArgs = 1;
    for (unsigned i = 0; i < 2; i++) {
        assert(csAllocMem(d, 8192, &flags, &io[i], NULL) == CS_SUCCESS);
        r->u.Compute.CSFId = i ? round2 : round1;
        csHelperSetComputeArg(&r->u.Compute.Args[0], CS_AFDM_TYPE, io[i], (unsigned long)0);
        assert(csConfigureBatchEntry(batch, CS_BATCH_ADD, r, 0, 0, &nodes[i]) == CS_SUCCESS);
    }
    r->u.Compute.CSFId = validate;
    r->u.Compute.NumArgs = 2;
    for (unsigned i = 0; i < 2; i++)
        csHelperSetComputeArg(&r->u.Compute.Args[i], CS_AFDM_TYPE, io[i], (unsigned long)0);
    assert(csConfigureBatchEntry(batch, CS_BATCH_ADD, r, 0, nodes[0], &nodes[2]) == CS_SUCCESS);
    assert(csConfigureBatchEntry(batch, CS_BATCH_JOIN, NULL, nodes[2], nodes[1], NULL) == CS_SUCCESS);
    CS_EVT_HANDLE event;
    assert(csCreateEvent(&event) == CS_SUCCESS);
    assert(csQueueBatchRequest(batch, NULL, NULL, event, NULL, NULL) == CS_QUEUED);
    CS_STATUS status = CS_NOT_DONE;
    u64 value = 0;
    for (unsigned i = 0; i < 10000 && status == CS_NOT_DONE; i++) {
        status = csPollEvent(event, NULL, &value);
        if (status == CS_NOT_DONE)
            usleep(1000);
    }
    assert(status == CS_SUCCESS && value == 16382);
    assert(csDeleteEvent(event) == CS_SUCCESS);
    /* Configuration was not modified while resuming: the graph is reusable. */
    assert(csQueueBatchRequest(batch, NULL, NULL, NULL, NULL, &value) == CS_SUCCESS);
    assert(value == 16382);
    assert(csFreeBatchRequest(batch) == CS_SUCCESS);
    for (unsigned i = 0; i < 2; i++)
        assert(csFreeMem(io[i], &flags) == CS_SUCCESS);
    free(r);
}
