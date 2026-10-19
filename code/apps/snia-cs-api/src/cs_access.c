/* SNIA 设备访问：打开/关闭设备并绑定插件，转发通知注册。 */
#include "cs_internal.h"
static void device_destroy(struct cs_object *o)
{
    struct cs_device *d = (void *)o;
    if (d->private_data) d->plugin->ops->closeCSx(d->private_data);
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
    struct cs_device *d = (void *)cs_handle_create(sizeof(*d), DEV, device_destroy);
    if (!d) { for (size_t i = 0; i < count; i++) cs_plugin_put(plugins[i]); free(plugins); return CS_OUT_OF_RESOURCES; }
    s = CS_DEVICE_NOT_AVAILABLE;
    for (size_t i = 0; i < count; i++) {
        void *device = NULL;
        unsigned workers = 1;
        CS_STATUS result = plugins[i]->ops->openCSx(name, ctx, &device, &workers);
        if (result == CS_SUCCESS) {
            d->private_data = device; d->plugin = plugins[i]; plugins[i] = NULL;
            d->workers = workers ? workers : 1; d->context = ctx; strcpy(d->path, name);
            s = CS_SUCCESS; break;
        }
        if (result != CS_DEVICE_NOT_AVAILABLE && result != CS_NO_MATCHING_DEVICE) s = result;
    }
    for (size_t i = 0; i < count; i++) if (plugins[i]) cs_plugin_put(plugins[i]);
    free(plugins);
    if (s != CS_SUCCESS) { cs_handle_retain(&d->base); cs_handle_retire(&d->base); cs_handle_release(&d->base); return s; }
    *handle = (void *)d->base.id;
    return CS_SUCCESS;
}
CS_STATUS csCloseCSx(CS_DEV_HANDLE h)
{
    struct cs_device *d = (void *)cs_handle_acquire(h, DEV);
    if (!d)
        return CS_INVALID_HANDLE;
    struct cs_object *garbage = NULL;
    unsigned functions = 0;
    CS_STATUS status = CS_SUCCESS;
    pthread_mutex_lock(&cs_handle_lock);
    for (struct cs_object *o = cs_objects; o; o = o->next) {
        if (o->kind == FUNCTION && ((struct cs_function *)o)->dev == d) {
            functions++;
            if (o->refs != 1)
                status = CS_HANDLE_IN_USE;
        }
    }
    if (d->base.refs != functions + 2)
        status = CS_HANDLE_IN_USE;
    if (status == CS_SUCCESS) {
        struct cs_object **p = &cs_objects;
        while (*p) {
            struct cs_object *o = *p;
            if (o == &d->base) {
                *p = o->next;
                o->refs--;
            } else if (o->kind == FUNCTION && ((struct cs_function *)o)->dev == d) {
                *p = o->next;
                o->refs--;
                o->next = garbage;
                garbage = o;
            } else
                p = &o->next;
        }
    }
    pthread_mutex_unlock(&cs_handle_lock);
    while (garbage) {
        struct cs_object *o = garbage;
        garbage = o->next;
        if (o->destroy)
            o->destroy(o);
        free(o);
    }
    cs_handle_release(&d->base);
    return status;
}
static CS_STATUS notify(const char *name, u32 options, csDevNotificationFn callback, bool add)
{
    if (!name || !options || !callback) return CS_INVALID_ARG;
    CS_DEV_HANDLE handle;
    CS_STATUS s = csOpenCSx(name, NULL, &handle);
    if (s != CS_SUCCESS) return s;
    struct cs_device *d = (void *)cs_handle_acquire(handle, DEV);
    const struct cs_backend_ops *ops = d->plugin->ops;
    if (add)
        s = ops->registerNotify ? ops->registerNotify(name, options, callback) : CS_UNSUPPORTED;
    else
        s = ops->deregisterNotify ? ops->deregisterNotify(name, options, callback) : CS_UNSUPPORTED;
    cs_handle_release(&d->base); csCloseCSx(handle); return s;
}
CS_STATUS csRegisterNotify(const char *name, u32 options, csDevNotificationFn callback)
{ return notify(name, options, callback, true); }
CS_STATUS csDeregisterNotify(const char *name, u32 options, csDevNotificationFn callback)
{ return notify(name, options, callback, false); }
