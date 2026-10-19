/* 设备插件接口，应用只包含 cs.h。
 * 标准操作名称去掉公开 API 的 cs 前缀，首单词小写、CSF/CSEE 缩写保留，例如：
 * csQueryDeviceProperties -> queryDeviceProperties。
 * 所有跨接口的句柄均为插件私有标识，公共库负责公开句柄与资源引用。
 * queue* 方法在调用线程中完成一次操作；异步排队、callback/event 和
 * batch 依赖由公共库处理，因此插件不重复实现这些通用机制。 */
#ifndef CS_BACKEND_H
#define CS_BACKEND_H
#include "cs.h"
#include <stdbool.h>
#define CS_BACKEND_ABI 2
struct cs_backend_ops {
    unsigned abi;
    const char *name;
    CS_PLUGIN_TYPE type;

    /* SNIA 设备发现与访问。openCSx 另返回并发数，供软件 batch 使用；
     * closeCSx 销毁私有设备，公开句柄检查和退役已由公共层完成。 */
    CS_STATUS (*queryCSxList)(int *length, char *buffer);
    CS_STATUS (*queryCSFList)(const char *path, int *length, int *count, CSFUniqueId *buffer);
    CS_STATUS (*getCSxFromPath)(const char *path, unsigned *length, char *deviceName);
    CS_STATUS (*openCSx)(const char *name, void *context, void **device, unsigned *concurrency);
    void (*closeCSx)(void *device);
    CS_STATUS (*registerNotify)(const char *name, u32 options, csDevNotificationFn callback);
    CS_STATUS (*deregisterNotify)(const char *name, u32 options, csDevNotificationFn callback);

    /* SNIA AFDM 管理；allocMem 额外返回实际分配长度，用于公共范围检查。 */
    CS_STATUS (*allocMem)(void *device, int bytes, const CsMemFlags *flags,
                          void **memory, u64 *allocatedBytes, CS_MEM_PTR *address);
    CS_STATUS (*freeMem)(void *device, void *memory, const CsMemFlags *flags);
    CS_STATUS (*initMem)(void *device, void *memory, unsigned long offset,
                         int bytes, const CsMemFlags *flags);
    CS_STATUS (*queueCopyMemRequest)(void *device, const CsCopyMemRequest *request, u64 *value);

    /* SNIA 存储 I/O 和 CSF 调度。CSF 私有标识有效到设备关闭。 */
    CS_STATUS (*queueStorageRequest)(void *device, const CsStorageRequest *request, u64 *value);
    CS_STATUS (*getCSFId)(void *device, const char *name, u64 globalId,
                          int *length, int *count, CSFIdinfo *buffer);
    CS_STATUS (*queueComputeRequest)(void *device, const CsComputeRequest *request, u64 *value);
    CS_STATUS (*CSEEDownload)(void *device, const CsCSEEDownloadInfo *info, u32 *id);

    /* SNIA 设备管理。 */
    CS_STATUS (*queryDeviceProperties)(void *device, CS_RESOURCE_TYPE type, int *length, CsProperties *buffer);
    CS_STATUS (*queryDeviceStatistics)(void *device, CS_STAT_TYPE type, void *id, CsStatsInfo *buffer);
    CS_STATUS (*CSFDownload)(void *device, const CsCSFDownloadInfo *info, u32 *id);
    CS_STATUS (*config)(void *device, int *length, const CsConfigInfo *info, CsConfigData *buffer);
    CS_STATUS (*reset)(void *device, CS_RESOURCE_TYPE type, u32 id);

    /* 以下为本项目内部钩子，不对应额外的 SNIA API。 */

    /* getCSFId 后调用：将 CSF 私有标识转换为设备资源编号，供公共层检查
     * CSFDownload/config 是否会修改仍被请求或 batch 使用的函数。 */
    u32 (*getCSFResourceId)(void *device, CS_CSF_ID function);

    /* 可选。在配置 batch 或提交请求时检查设备特有限制；请求已完成
     * 句柄绑定，尚未执行。失败时立即返回，不保留无效的 batch 节点。 */
    CS_STATUS (*validateRequest)(void *device, const CsBatchRequest *request);

    /* 可选。执行 batch 中的一个逻辑节点，包括适配器自己的重入循环。
     * 例如 FEMU 在这里处理 NEED_STORE/LOAD/FREE；返回成功后后继才就绪。
     * 未提供时，公共层调用上面的 queue* 方法。execution 标识本次整批执行。 */
    CS_STATUS (*executeBatchEntry)(void *device, const CsBatchRequest *request,
                                   bool (*isCancelled)(void *), void *cancelContext,
                                   void *execution, u64 *value);

    /* 可选。在整批所有工作线程退出后调用，success 表示整批是否成功。
     * FEMU 据此保留成功执行的 spill 区段，或回收失败执行新增的区段。
     * 这是适配器的批次资源清理钩子，不是算子的完成回调。 */
    void (*finishBatchExecution)(void *device, void *execution, bool success);
};
/* 内部插件工厂契约，不属于 SNIA API。每个动态库导出这两个 C 符号；
 * destroy 只在设备、请求等引用全部结束后执行。 */
typedef const struct cs_backend_ops *(*cs_backend_create_fn)(void);
typedef void (*cs_backend_destroy_fn)(const struct cs_backend_ops *);
#ifdef __cplusplus
extern "C" {
#endif
const struct cs_backend_ops *cs_backend_create(void);
void cs_backend_destroy(const struct cs_backend_ops *);
#ifdef __cplusplus
}
#endif
#endif
