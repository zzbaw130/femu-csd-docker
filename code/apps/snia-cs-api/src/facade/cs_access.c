#include "internal.h"
static void device_destroy(struct cs_object *o)
{
    struct cs_device *d = (void *)o;
    if (d->private_data) d->plugin->ops->close(d->private_data);
    if (d->plugin) cs_plugin_put(d->plugin);
}
CS_STATUS csOpenCSx(const char *name, void *ctx, CS_DEV_HANDLE *handle)
{
    if (!name || !handle) return CS_INVALID_ARG;
    *handle = NULL;
    if (strlen(name) >= sizeof(((struct cs_device *)0)->path)) return CS_INVALID_DEVICE_NAME;
    struct cs_plugin **plugins;
    size_t count;
    CS_STATUS s = cs_plugins_snapshot(&plugins, &count);
    if (s != CS_SUCCESS) return s;
    struct cs_device *d = (void *)cs_new(sizeof(*d), DEV, device_destroy);
    if (!d) { for (size_t i = 0; i < count; i++) cs_plugin_put(plugins[i]); free(plugins); return CS_OUT_OF_RESOURCES; }
    s = CS_DEVICE_NOT_AVAILABLE;
    for (size_t i = 0; i < count; i++) {
        void *device = NULL;
        unsigned workers = 1;
        CS_STATUS result = plugins[i]->ops->open(name, ctx, &device, &workers);
        if (result == CS_SUCCESS) {
            d->private_data = device; d->plugin = plugins[i]; plugins[i] = NULL;
            d->workers = workers ? workers : 1; d->context = ctx; strcpy(d->path, name);
            s = CS_SUCCESS; break;
        }
        if (result != CS_DEVICE_NOT_AVAILABLE && result != CS_NO_MATCHING_DEVICE) s = result;
    }
    for (size_t i = 0; i < count; i++) if (plugins[i]) cs_plugin_put(plugins[i]);
    free(plugins);
    if (s != CS_SUCCESS) { cs_hold(&d->base); cs_retire(&d->base); cs_put(&d->base); return s; }
    *handle = (void *)d->base.id;
    return CS_SUCCESS;
}
CS_STATUS csCloseCSx(CS_DEV_HANDLE handle) { return cs_device_close(handle); }
static CS_STATUS notify(const char *name, u32 options, csDevNotificationFn callback, bool add)
{
    if (!name || !options || !callback) return CS_INVALID_ARG;
    CS_DEV_HANDLE handle;
    CS_STATUS s = csOpenCSx(name, NULL, &handle);
    if (s != CS_SUCCESS) return s;
    struct cs_device *d = (void *)cs_get(handle, DEV);
    s = d->plugin->ops->notify ? d->plugin->ops->notify(name, options, callback, add) : CS_UNSUPPORTED;
    cs_put(&d->base); csCloseCSx(handle); return s;
}
CS_STATUS csRegisterNotify(const char *name, u32 options, csDevNotificationFn callback)
{ return notify(name, options, callback, true); }
CS_STATUS csDeregisterNotify(const char *name, u32 options, csDevNotificationFn callback)
{ return notify(name, options, callback, false); }
