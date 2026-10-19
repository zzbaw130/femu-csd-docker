#include "internal.h"
CS_STATUS csQueryCSxList(int *length, char *buffer)
{
    if (!length) return CS_INVALID_ARG;
    struct cs_plugin **plugins; size_t count;
    CS_STATUS status = cs_plugins_snapshot(&plugins, &count);
    if (status != CS_SUCCESS) return status;
    char *list = calloc(1, 1); size_t bytes = 1;
    if (!list) status = CS_OUT_OF_RESOURCES;
    for (size_t i = 0; i < count && status == CS_SUCCESS; i++) {
        int n = 0;
        CS_STATUS s = plugins[i]->ops->enumerate(&n, NULL);
        if (s != CS_INVALID_LENGTH && s != CS_SUCCESS) { status = s; break; }
        if (n <= 1) continue;
        char *part = malloc(n);
        if (!part) { status = CS_OUT_OF_RESOURCES; break; }
        s = plugins[i]->ops->enumerate(&n, part);
        if (s == CS_SUCCESS) {
            size_t extra = strlen(part);
            if (extra) {
                char *next = realloc(list, bytes + extra + (bytes > 1));
                if (!next) s = CS_OUT_OF_RESOURCES;
                else {
                    list = next;
                    if (bytes > 1) list[bytes++ - 1] = ',';
                    memcpy(list + bytes - 1, part, extra + 1); bytes += extra;
                }
            }
        }
        free(part); status = s;
    }
    for (size_t i = 0; i < count; i++) cs_plugin_put(plugins[i]);
    free(plugins);
    if (status == CS_SUCCESS) status = cs_output(length, buffer, bytes);
    if (status == CS_SUCCESS) memcpy(buffer, list, bytes);
    free(list); return status;
}
CS_STATUS csGetCSxFromPath(const char *path, unsigned *length, char *out)
{
    if (!path || !length) return CS_INVALID_ARG;
    struct cs_plugin **plugins; size_t count;
    CS_STATUS s = cs_plugins_snapshot(&plugins, &count);
    if (s != CS_SUCCESS) return s;
    s = CS_NO_MATCHING_DEVICE;
    for (size_t i = 0; i < count; i++) {
        unsigned n = *length;
        CS_STATUS result = plugins[i]->ops->resolve(path, &n, out);
        if (result == CS_SUCCESS || result == CS_INVALID_LENGTH) { *length = n; s = result; break; }
        if (result == CS_INVALID_PATH) s = result;
    }
    for (size_t i = 0; i < count; i++) cs_plugin_put(plugins[i]);
    free(plugins); return s;
}
CS_STATUS csQueryCSFList(const char *path, int *length, int *count, CSFUniqueId *out)
{
    if (!path || !length || !count) return CS_INVALID_ARG;
    char name[4096]; unsigned n = sizeof(name);
    CS_STATUS s = csGetCSxFromPath(path, &n, name);
    if (s != CS_SUCCESS) return s;
    CS_DEV_HANDLE handle;
    s = csOpenCSx(name, NULL, &handle);
    if (s != CS_SUCCESS) return s;
    struct cs_device *d = (void *)cs_get(handle, DEV);
    s = d->plugin->ops->csf_list ? d->plugin->ops->csf_list(path, length, count, out) : CS_UNSUPPORTED;
    cs_put(&d->base); csCloseCSx(handle); return s;
}
