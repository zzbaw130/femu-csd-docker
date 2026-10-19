#include "internal.h"
CS_STATUS csCSFDownload(CS_DEV_HANDLE handle, const CsCSFDownloadInfo *info, u32 *out)
{
    if (!info || !out) return CS_INVALID_ARG;
    struct cs_device *d = (void *)cs_get(handle, DEV);
    if (!d) return CS_INVALID_HANDLE;
    pthread_mutex_lock(&cs_binding_lock);
    CS_STATUS s = cs_function_busy(d, info->Index) ? CS_HANDLE_IN_USE
        : d->plugin->ops->download_csf ? d->plugin->ops->download_csf(d->private_data, info, out) : CS_UNSUPPORTED;
    pthread_mutex_unlock(&cs_binding_lock);
    cs_put(&d->base); return s;
}
CS_STATUS csConfig(CS_DEV_HANDLE handle, int *length, const CsConfigInfo *info, CsConfigData *out)
{
    if (!info || !length) return CS_INVALID_ARG;
    struct cs_device *d = (void *)cs_get(handle, DEV);
    if (!d) return CS_INVALID_HANDLE;
    pthread_mutex_lock(&cs_binding_lock);
    CS_STATUS s = info->Type == CS_CSF_ACTIVATE && info->u.CSFActivateInfo.State == CS_INACTIVE_STATE &&
        cs_function_busy(d, info->u.CSFActivateInfo.CSFId) ? CS_HANDLE_IN_USE
        : d->plugin->ops->config ? d->plugin->ops->config(d->private_data, length, info, out) : CS_UNSUPPORTED;
    pthread_mutex_unlock(&cs_binding_lock);
    cs_put(&d->base); return s;
}
CS_STATUS csReset(CS_DEV_HANDLE handle, CS_RESOURCE_TYPE type, u32 id)
{
    struct cs_device *d = (void *)cs_get(handle, DEV);
    if (!d) return CS_INVALID_HANDLE;
    CS_STATUS s = d->plugin->ops->reset ? d->plugin->ops->reset(d->private_data, type, id) : CS_UNSUPPORTED;
    cs_put(&d->base); return s;
}
CS_STATUS csQueryDeviceProperties(CS_DEV_HANDLE handle, CS_RESOURCE_TYPE type, int *length, CsProperties *out)
{
    if (!length) return CS_INVALID_ARG;
    struct cs_device *d = (void *)cs_get(handle, DEV);
    if (!d) return CS_INVALID_HANDLE;
    CS_STATUS s = d->plugin->ops->properties ? d->plugin->ops->properties(d->private_data, type, length, out) : CS_UNSUPPORTED;
    cs_put(&d->base); return s;
}
CS_STATUS csQueryDeviceStatistics(CS_DEV_HANDLE handle, CS_STAT_TYPE type, void *id, CsStatsInfo *out)
{
    if (!out) return CS_INVALID_ARG;
    struct cs_device *d = (void *)cs_get(handle, DEV);
    if (!d) return CS_INVALID_HANDLE;
    CS_STATUS s = d->plugin->ops->statistics ? d->plugin->ops->statistics(d->private_data, type, id, out) : CS_UNSUPPORTED;
    cs_put(&d->base); return s;
}
