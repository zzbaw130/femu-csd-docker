/* SNIA Computational Storage API v1.1, Linux/FEMU implementation. */
#ifndef SINA_CS_H
#define SINA_CS_H
#include "cs_types.h"
#ifdef __cplusplus
extern "C" {
#endif
CS_STATUS csQueryCSxList(int *Length, char *Buffer);
CS_STATUS csQueryCSFList(const char *Path, int *Length, int *Count, CSFUniqueId *Buffer);
CS_STATUS csGetCSxFromPath(const char *Path, unsigned int *Length, char *DevName);
CS_STATUS csOpenCSx(const char *DevName, void *DevContext, CS_DEV_HANDLE *DevHandle);
CS_STATUS csCloseCSx(CS_DEV_HANDLE DevHandle);
CS_STATUS csRegisterNotify(const char *DevName, u32 NotifyOptions, csDevNotificationFn NotifyFn);
CS_STATUS csDeregisterNotify(const char *DevName, u32 NotifyOptions, csDevNotificationFn NotifyFn);
CS_STATUS csAllocMem(CS_DEV_HANDLE DevHandle, int Bytes, const CsMemFlags *MemFlags,
                     CS_MEM_HANDLE *MemHandle, CS_MEM_PTR *VAddressPtr);
CS_STATUS csFreeMem(CS_MEM_HANDLE MemHandle, const CsMemFlags *MemFlags);
CS_STATUS csInitMem(CS_MEM_HANDLE MemHandle, unsigned long ByteOffset, int Bytes,
                    const CsMemFlags *MemFlags);
CS_STATUS csQueueStorageRequest(const CsStorageRequest *Req, void *Context,
                                csQueueCallbackFn CallbackFn, CS_EVT_HANDLE EventHandle,
                                CS_REQ_HANDLE *ReqHandle, u64 *CompValue);
CS_STATUS csQueueCopyMemRequest(const CsCopyMemRequest *CopyReq, void *Context,
                                csQueueCallbackFn CallbackFn, CS_EVT_HANDLE EventHandle,
                                CS_REQ_HANDLE *ReqHandle, u64 *CompValue);
CS_STATUS csGetCSFId(CS_DEV_HANDLE DevHandle, const char *CSFName, u64 GlobalId, int *Length,
                     int *Count, CSFIdinfo *Buffer);
CS_STATUS csAbortRequest(CS_REQ_HANDLE ReqHandle);
CS_STATUS csQueueComputeRequest(const CsComputeRequest *Req, void *Context,
                                csQueueCallbackFn CallbackFn, CS_EVT_HANDLE EventHandle,
                                CS_REQ_HANDLE *ReqHandle, u64 *CompValue);
CS_STATUS csCSEEDownload(CS_DEV_HANDLE DevHandle, const CsCSEEDownloadInfo *Info, u32 *CSEEId);
void csHelperSetComputeArg(CsComputeArg *ArgPtr, CS_COMPUTE_ARG_TYPE Type, ...);
CS_STATUS csAllocBatchRequest(CS_BATCH_MODE Mode, int MaxReqs, CS_BATCH_HANDLE *BatchHandle);
CS_STATUS csFreeBatchRequest(CS_BATCH_HANDLE BatchHandle);
CS_STATUS csConfigureBatchEntry(CS_BATCH_HANDLE BatchHandle, CS_BATCH_CONFIG_TYPE Action,
                                const CsBatchRequest *Req, CS_BATCH_INDEX Before,
                                CS_BATCH_INDEX After, CS_BATCH_INDEX *Curr);
CS_STATUS csHelperResizeBatchRequest(CS_BATCH_HANDLE BatchHandle, int MaxReqs);
CS_STATUS csQueueBatchRequest(CS_BATCH_HANDLE BatchHandle, void *Context,
                              csQueueCallbackFn CallbackFn, CS_EVT_HANDLE EventHandle,
                              CS_REQ_HANDLE *ReqHandle, u64 *CompValue);
CS_STATUS csCreateEvent(CS_EVT_HANDLE *EventHandle);
CS_STATUS csDeleteEvent(CS_EVT_HANDLE EventHandle);
CS_STATUS csPollEvent(CS_EVT_HANDLE EventHandle, void **Context, u64 *CompValue);
CS_STATUS csQueryDeviceProperties(CS_DEV_HANDLE DevHandle, CS_RESOURCE_TYPE Type, int *Length,
                                  CsProperties *Buffer);
CS_STATUS csQueryDeviceStatistics(CS_DEV_HANDLE DevHandle, CS_STAT_TYPE Type, void *Identifier,
                                  CsStatsInfo *Stats);
CS_STATUS csCSFDownload(CS_DEV_HANDLE DevHandle, const CsCSFDownloadInfo *Info, u32 *CSFId);
CS_STATUS csConfig(CS_DEV_HANDLE DevHandle, int *Length, const CsConfigInfo *Info,
                   CsConfigData *Data);
CS_STATUS csReset(CS_DEV_HANDLE DevHandle, CS_RESOURCE_TYPE ResourceType, u32 ResourceId);
CS_STATUS csQueryLibrarySupport(CS_LIBRARY_SUPPORT Type, int *Length, char *Buffer);
CS_STATUS csRegisterPlugin(const CsPluginRequest *Req);
CS_STATUS csDeregisterPlugin(const CsPluginRequest *Req);
#ifdef __cplusplus
}
#endif
#endif
