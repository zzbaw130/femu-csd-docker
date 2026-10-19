/* SNIA Computational Storage API v1.1: application C interface. */
#ifndef SNIA_CS_H
#define SNIA_CS_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef void *CS_DEV_HANDLE;
typedef void *CS_MEM_HANDLE;
typedef void *CS_MEM_PTR;
typedef void *CS_CSF_ID;
typedef void *CS_EVT_HANDLE;
typedef void *CS_BATCH_HANDLE;
typedef void *CS_REQ_HANDLE;
typedef uint32_t CS_BATCH_INDEX;
typedef enum {
    CS_CSx_TYPE = 1,
    CS_CSE_TYPE = 2,
    CS_CSEE_TYPE = 3,
    CS_FDM_TYPE = 4,
    CS_CSF_TYPE = 5,
    CS_VENDOR_SPECIFIC_TYPE = 6
} CS_RESOURCE_TYPE;
typedef enum {
    CS_CSEE_UNDEFINED = 0,
    CS_CSEE_FPGA = 1,
    CS_CSEE_BPF = 2,
    CS_CSEE_CONTAINER = 3,
    CS_CSEE_OPERATING_SYSTEM = 4,
    CS_CSEE_VENDOR_SPECIFIC = 65535
} CS_CSEE_RESOURCE_TYPE;
typedef enum {
    CS_CSF_UNDEFINED = 0,
    CS_CSF_FPGA_BITSTREAM = 1,
    CS_CSF_BPF_PROGRAM = 2,
    CS_CSF_CONTAINER_IMAGE = 3,
    CS_CSF_OPERATING_SYSTEM_IMAGE = 4,
    CS_CSF_VENDOR_SPECIFIC = 65535
} CS_CSF_RESOURCE_TYPE;
typedef enum {
    CS_SUBTYPE_UNDEFINED = 0,
    CS_SUBTYPE_FPGA_AMD = 1,
    CS_SUBTYPE_FPGA_INTEL = 2,
    CS_SUBTYPE_CONTAINER_DOCKER = 3,
    CS_SUBTYPE_CONTAINER_KUBERNETES = 4,
    CS_SUBTYPE_OS_LINUX = 5,
    CS_SUBTYPE_OS_VMWARE = 6,
    CS_SUBTYPE_VENDOR_SPECIFIC = 65535
} CS_RESOURCE_SUBTYPE;
typedef enum {
    CS_INACTIVE_STATE = 0,
    CS_ACTIVE_STATE = 1,
} CS_STATE;
typedef enum {
    CS_CSEE_ACTIVATE = 1,
    CS_CSF_ACTIVATE = 2,
    CS_FDM_ACTIVATE = 3,
    CS_VENDOR_SPECIFIC = 4
} CS_CONFIG_TYPE;
typedef enum {
    CS_FDM_CLEAR = 1, // clears AFDM to all zeroes
    CS_FDM_FILL = 2   // fill AFDM with specified value
} CS_FDM_FLAG_TYPE;
typedef enum {
    CS_COPY_TO_DEVICE = 1,
    CS_COPY_FROM_DEVICE = 2,
    CS_COPY_WITHIN_DEVICE = 3
} CS_MEM_COPY_TYPE;
typedef enum { CS_STORAGE_BLOCK_IO = 1, CS_STORAGE_FILE_IO = 2 } CS_STORAGE_REQ_MODE;
typedef enum { CS_STORAGE_LOAD_TYPE = 1, CS_STORAGE_STORE_TYPE = 2 } CS_STORAGE_IO_TYPE;
typedef enum {
    CS_AFDM_TYPE = 1,
    CS_8BIT_VALUE_TYPE = 2,
    CS_16BIT_VALUE_TYPE = 3,
    CS_32BIT_VALUE_TYPE = 4,
    CS_64BIT_VALUE_TYPE = 5,
    CS_128BIT_VALUE_TYPE = 6,
    CS_DESCRIPTOR_TYPE = 7
} CS_COMPUTE_ARG_TYPE;
typedef enum { CS_BATCH_SERIAL = 1, CS_BATCH_PARALLEL = 2, CS_BATCH_HYBRID = 3 } CS_BATCH_MODE;
typedef enum { CS_COPY_AFDM = 1, CS_STORAGE_IO = 2, CS_QUEUE_COMPUTE = 3 } CS_BATCH_REQ_TYPE;
typedef enum {
    CS_BATCH_ADD = 1,
    CS_BATCH_DELETE = 2,
    CS_BATCH_RECONFIG = 3,
    CS_BATCH_JOIN = 4,
    CS_BATCH_SPLIT = 5
} CS_BATCH_CONFIG_TYPE;
typedef enum {
    CS_STAT_CSE_USAGE = 1,     // query to provide CSE runtime statistics
    CS_STAT_CSx_MEM_USAGE = 2, // query CSx memory usage
    CS_STAT_CSF = 3            // query statistics on a specific function
} CS_STAT_TYPE;
typedef enum { CS_FILE_SYSTEMS_SUPPORTED = 1, CS_RESERVED = 2 } CS_LIBRARY_SUPPORT;
typedef enum {
    CS_PLUGIN_COMPUTE = 1,
    CS_PLUGIN_NVME = 2,
    CS_PLUGIN_FILE_SYSTEM = 4,
    CS_PLUGIN_CUSTOM = 8
} CS_PLUGIN_TYPE;
typedef enum CS_STATUS {
    CS_SUCCESS = 0,
    CS_COULD_NOT_MAP_MEMORY = 1,
    CS_DEVICE_ERROR = 2,
    CS_DEVICE_NOT_AVAILABLE = 3,
    CS_DEVICE_NOT_READY = 4,
    CS_DEVICE_NOT_PRESENT = 5,
    CS_INVALID_DEVICE_NAME = 6,
    CS_INVALID_PATH = 7,
    CS_ENTITY_NOT_ON_DEVICE = 8,
    CS_NO_MATCHING_DEVICE = 9,
    CS_ERROR_IN_EXECUTION = 10,
    CS_FATAL_ERROR = 11,
    CS_HANDLE_IN_USE = 12,
    CS_INVALID_HANDLE = 13,
    CS_INVALID_ARG = 14,
    CS_INVALID_ID = 15,
    CS_INVALID_LENGTH = 16,
    CS_INVALID_OPTION = 17,
    CS_INVALID_CSF_ID = 18,
    CS_INVALID_CSF_NAME = 19,
    CS_INVALID_FDM_ID = 20,
    CS_INVALID_GLOBAL_ID = 21,
    CS_IO_TIMEOUT = 22,
    CS_LOAD_ERROR = 23,
    CS_MEMORY_IN_USE = 24,
    CS_NO_PERMISSIONS = 25,
    CS_NOT_DONE = 26,
    CS_NOT_ENOUGH_MEMORY = 27,
    CS_OUT_OF_RESOURCES = 28,
    CS_QUEUED = 29,
    CS_NOTHING_QUEUED = 30,
    CS_COULD_NOT_UNMAP_MEMORY = 31,
    CS_UNKNOWN_MEMORY = 32,
    CS_UNSUPPORTED = 33,
    CS_UNSUPPORTED_TYPE = 34,
    CS_UNSUPPORTED_INDEX = 35
} CS_STATUS;
typedef enum {
    CS_NOTIFY_SYSTEM_ERROR = 1 << 0,
    CS_NOTIFY_CSE_UNRESPONSIVE = 1 << 1,
    CS_NOTIFY_CSEE_UNRESPONSIVE = 1 << 2,
    CS_NOTIFY_CSF_UNRESPONSIVE = 1 << 3,
    CS_NOTIFY_CSE_RESET = 1 << 4,
    CS_NOTIFY_CSEE_RESET = 1 << 5,
    CS_NOTIFY_CSx_RESET = 1 << 6,
    CS_NOTIFY_CSx_ADDED = 1 << 7,
    CS_NOTIFY_CSx_REMOVED = 1 << 8,
    CS_NOTIFY_CSF_ADDED = 1 << 9,
    CS_NOTIFY_CSF_REMOVED = 1 << 10,
    CS_NOTIFY_RESOURCE_WARNING = 1 << 11,
    CS_NOTIFY_DOWNLOAD_INFO = 1 << 12,
    CS_NOTIFY_CONFIG_INFO = 1 << 13
} CS_NOTIFY_TYPE;
/* v1.1 prose uses these names although its enums omit/rename them. */
#define CS_LENGTH_RETURNED CS_INVALID_LENGTH
#define CS_COPY_DEV_MEM CS_COPY_AFDM
typedef struct CRProperties CRProperties;
typedef struct {
    u16 HwVersion, SwVersion;
    char Name[32];
    u32 ERId;
    CRProperties *Features;
} ComputeResource;
typedef struct {
    u16 CSETypeToken;
    u8 RelativePerformance, RelativePower;
    u32 MaxRequestsPerBatch, MaxCSFParametersAllowed, CSEId;
    u16 MaxCSEEs, NumActivatedCSEEs, NumAvailableCRs, NumCRs;
    ComputeResource *CRs;
} CSEInfo;
typedef struct {
    u16 NumCSEs;
    CSEInfo CSE[1];
} CSEProperties;
typedef struct {
    CS_STATE State;
    u32 CSEId, EEIId;
} CSEEInstance;
typedef struct {
    CS_CSEE_RESOURCE_TYPE Type;
    u16 SwVersion;
    char Name[32];
    u16 CSETypeToken, NumBuiltinCSFs;
    u32 CSEEId;
    u16 MaxCSFs, NumActivatedCSFs, NumEEIs;
    CSEEInstance *EEInstances;
} CSEEInfo;
typedef struct {
    u16 NumCSEEs;
    CSEEInfo CSEE[1];
} CSEEProperties;
typedef struct {
    u64 DeviceManaged : 1, HostVisible : 1, ClearContentsSupported : 1,
        InvalidateContentsSupported : 1, ConfigSupported : 1, Reserved : 59;
} FDMFlags;
typedef struct {
    u32 CSEId;
    u8 RelativePerformance, RelativePower;
} CSEAccess;
typedef struct {
    u32 FDMId;
    u64 FDMSize;
    FDMFlags Flags;
    u16 NumCSEs;
    CSEAccess *CSEList;
} FDMInfo;
typedef struct {
    u16 NumFDMs;
    FDMInfo FDM[1];
} FDMProperties;
typedef struct {
    CS_STATE State;
    u32 EEIId, FIId;
    u16 NumCRs;
    u32 *ERList;
} CSFInstance;
typedef struct {
    u64 GlobalId;
    char UniqueName[32];
    u16 CSETypeToken;
    u32 CSFId;
    u8 Builtin : 1, Reserved : 7;
    u16 NumFIs;
    CSFInstance *FInstances;
} CSFInfo;
typedef struct {
    u16 NumCSFs;
    CSFInfo CSF[1];
} CSFProperties;
typedef struct {
    void *VSData;
} CSVendorSpecific;
typedef struct {
    u16 HwVersion, SwVersion;
    u32 VendorId, DeviceId;
    char FriendlyName[32];
    struct {
        u64 BatchRequestsSupported : 1, Reserved : 63;
    } Flags;
} CSxProperties;
typedef union {
    CSxProperties CSxDetails;
    CSEProperties CSEDetails[1];
    CSEEProperties CSEEDetails[1];
    FDMProperties FDMDetails[1];
    CSFProperties CSFDetails[1];
    CSVendorSpecific VSDetails;
} CsProperties;
typedef struct {
    CS_STATE State;
    u32 CSEEId, CSEId;
} CSEEConfig;
typedef struct {
    CS_STATE State;
    u32 CSFId, EEIId;
    u16 NumCRs;
    u32 CRArray[1];
} CSFConfig;
typedef struct {
    CS_STATE State;
    u32 FDMId;
} FDMConfig;
typedef struct {
    void *VSData;
} CSVendorConfig;
typedef struct {
    CS_CONFIG_TYPE Type;
    union {
        CSEEConfig CSEEActivateInfo;
        CSFConfig CSFActivateInfo;
        FDMConfig FDMActivateInfo;
        CSVendorConfig VSInfo;
    } u;
} CsConfigInfo;
typedef struct {
    CS_STATE State;
    u32 ID;
} CsActivationInfo;
typedef union {
    CsActivationInfo Data;
    void *VSData;
} CsConfigData;
typedef struct {
    u32 FDMId;
    CS_FDM_FLAG_TYPE Flags;
    u32 FillValue;
} CsMemFlags;
typedef struct {
    CS_MEM_HANDLE MemHandle;
    unsigned long ByteOffset;
} CsDevAFDM;
typedef struct {
    CS_MEM_COPY_TYPE Type;
    union {
        void *HostVAddress;
        CsDevAFDM SrcDevMem;
    } u;
    CsDevAFDM DevMem;
    int Bytes;
} CsCopyMemRequest;
typedef struct {
    u32 NamespaceId;
    u64 StartLba;
    u32 NumBlocks;
} CSBlockRange;
typedef struct {
    CS_STORAGE_IO_TYPE Type;
    u32 StorageIndex;
    CsDevAFDM DevMem;
    int NumRanges;
    CSBlockRange Range[1];
} CsBlockIo;
typedef struct {
    CS_STORAGE_IO_TYPE Type;
    void *FileHandle;
    u64 Offset;
    u32 Bytes;
    CsDevAFDM DevMem;
} CsFileIo;
typedef struct {
    CS_STORAGE_REQ_MODE Mode;
    CS_DEV_HANDLE DevHandle;
    union {
        CsBlockIo BlockIo;
        CsFileIo FileIo;
    } u;
} CsStorageRequest;
typedef struct {
    CS_COMPUTE_ARG_TYPE Type;
    union {
        CsDevAFDM DevMem;
        u64 Value64;
        u32 Value32;
        u16 Value16;
        u8 Value8;
    } u;
} CsComputeArg;
typedef struct {
    CS_CSF_ID CSFId;
    int NumArgs;
    CsComputeArg Args[1];
} CsComputeRequest;
typedef struct {
    CS_BATCH_REQ_TYPE ReqType;
    union {
        CsCopyMemRequest CopyMem;
        CsStorageRequest StorageIo;
        CsComputeRequest Compute;
    } u;
} CsBatchRequest;
typedef struct {
    u32 PowerOnMins, IdleTimeMins;
} CSEUsage;
typedef struct {
    u64 TotalAllocatedFDM, LargestBlockAvailableFDM, AverageAllocatedSizeFDM, TotalFreeFDM,
        TotalAllocationsFDM, TotalDeAllocationsFDM, TotalFDMtoHostinMB, TotalHosttoFDMinMB,
        TotalFDMtoStorageinMB, TotalStoragetoFDMinMB;
} CSxMemory;
typedef struct {
    u64 TotalUptimeSeconds, TotalExecutions, ShortestTimeUsecs, LongestTimeUsecs, AverageTimeUsecs;
} CSFUsage;
typedef union {
    CSEUsage CSEDetails;
    CSxMemory MemoryDetails;
    CSFUsage CSFDetails;
} CsStatsInfo;
typedef struct {
    u32 FDMId;
    u8 RelativePerformance, RelativePower;
    FDMFlags Flags;
} FDMAccess;
typedef struct {
    char UniqueName[32];
    u64 GlobalId;
} CSFUniqueId;
typedef struct {
    CS_CSF_ID CSFId;
    u8 RelativePerformance, RelativePower, Count, NumFDMs;
    FDMAccess *FDMList;
} CSFIdInfo;
typedef CSFIdInfo CSFIdinfo;
typedef struct {
    char UniqueName[32];
    int Unload, Length;
    void *DataBuffer;
} CsCommonDownloadInfo;
typedef struct {
    CS_CSEE_RESOURCE_TYPE Type;
    CS_RESOURCE_SUBTYPE SubType;
    u32 CSEId, Index;
    CsCommonDownloadInfo common;
} CsCSEEDownloadInfo;
typedef struct {
    CS_CSF_RESOURCE_TYPE Type;
    CS_RESOURCE_SUBTYPE SubType;
    u64 GlobalId;
    u32 CSEEId, Index;
    CsCommonDownloadInfo common;
} CsCSFDownloadInfo;
typedef struct {
    CS_PLUGIN_TYPE Type;
    char PluginPath[4096];
} CsPluginRequest;
typedef void (*csDevNotificationFn)(u32 Notification, void *Context, CS_STATUS Status, int Length,
                                    void *Buffer);
typedef void (*csQueueCallbackFn)(void *QueueContext, CS_STATUS Status, u64 CompValue);
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
