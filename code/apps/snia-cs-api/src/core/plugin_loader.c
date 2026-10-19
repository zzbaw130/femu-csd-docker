#define _GNU_SOURCE
#include "internal.h"
#include <dlfcn.h>
#include <glob.h>
#include <stdio.h>
static pthread_mutex_t plugin_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t module_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t plugin_once = PTHREAD_ONCE_INIT;
static struct cs_plugin *plugins;
static void dispose(struct cs_plugin *p)
{
    p->destroy(p->ops);
    dlclose(p->library);
    free(p->path);
    free(p);
}
static CS_STATUS load_locked(const char *path, unsigned type)
{
    char *resolved = realpath(path, NULL);
    if (!resolved) return CS_LOAD_ERROR;
    pthread_mutex_lock(&plugin_lock);
    for (struct cs_plugin *p = plugins; p; p = p->next)
        if (!strcmp(p->path, resolved)) {
            CS_STATUS s = !type || (type & p->ops->type) ? CS_SUCCESS : CS_UNSUPPORTED_TYPE;
            pthread_mutex_unlock(&plugin_lock); free(resolved); return s;
        }
    pthread_mutex_unlock(&plugin_lock);
    void *library = dlopen(resolved, RTLD_NOW | RTLD_LOCAL);
    if (!library) { free(resolved); return CS_LOAD_ERROR; }
    cs_backend_create_fn create = (cs_backend_create_fn)dlsym(library, "cs_backend_create");
    cs_backend_destroy_fn destroy = (cs_backend_destroy_fn)dlsym(library, "cs_backend_destroy");
    const struct cs_backend_ops *ops = create && destroy ? create() : NULL;
    CS_STATUS status = CS_LOAD_ERROR;
    if (!ops || ops->abi != CS_BACKEND_ABI || !ops->name || !ops->enumerate || !ops->resolve ||
        !ops->open || !ops->close || !ops->alloc || !ops->free_mem || !ops->init_mem ||
        !ops->validate || !ops->execute) goto fail;
    if (type && !(type & ops->type)) { status = CS_UNSUPPORTED_TYPE; goto fail; }
    struct cs_plugin *p = calloc(1, sizeof(*p));
    if (!p) { status = CS_OUT_OF_RESOURCES; goto fail; }
    *p = (struct cs_plugin){.library = library, .ops = ops, .destroy = destroy,
                           .path = resolved, .refs = 1};
    pthread_mutex_lock(&plugin_lock);
    p->next = plugins; plugins = p;
    pthread_mutex_unlock(&plugin_lock);
    return CS_SUCCESS;
fail:
    if (ops) destroy(ops);
    dlclose(library); free(resolved); return status;
}
static CS_STATUS load(const char *path, unsigned type)
{
    pthread_mutex_lock(&module_lock);
    CS_STATUS s = load_locked(path, type);
    pthread_mutex_unlock(&module_lock);
    return s;
}
static void discover(void)
{
    Dl_info info;
    if (!dladdr((void *)csOpenCSx, &info)) return;
    char *path = realpath(info.dli_fname, NULL);
    if (!path) return;
    char *slash = strrchr(path, '/');
    if (!slash) { free(path); return; }
    *slash = 0;
    char pattern[4096];
    snprintf(pattern, sizeof(pattern), "%s/snia-cs-api/*.so", path);
    free(path);
    glob_t matches = {0};
    if (!glob(pattern, 0, NULL, &matches))
        for (size_t i = 0; i < matches.gl_pathc; i++) load(matches.gl_pathv[i], 0);
    globfree(&matches);
}
CS_STATUS cs_plugins_snapshot(struct cs_plugin ***out, size_t *count)
{
    pthread_once(&plugin_once, discover);
    *out = NULL; *count = 0;
    pthread_mutex_lock(&plugin_lock);
    for (struct cs_plugin *p = plugins; p; p = p->next) (*count)++;
    struct cs_plugin **list = calloc(*count ? *count : 1, sizeof(*list));
    if (list) {
        size_t i = 0;
        for (struct cs_plugin *p = plugins; p; p = p->next) { p->refs++; list[i++] = p; }
    }
    pthread_mutex_unlock(&plugin_lock);
    if (!list) return CS_OUT_OF_RESOURCES;
    *out = list; return CS_SUCCESS;
}
void cs_plugin_put(struct cs_plugin *p)
{
    pthread_mutex_lock(&plugin_lock);
    bool dead = !--p->refs;
    pthread_mutex_unlock(&plugin_lock);
    if (dead) dispose(p);
}
static bool valid(const CsPluginRequest *r)
{
    return r && r->Type && !(r->Type & ~15U) && r->PluginPath[0] &&
           memchr(r->PluginPath, 0, sizeof(r->PluginPath));
}
CS_STATUS cs_plugin_register(const CsPluginRequest *r)
{
    if (!valid(r)) return CS_INVALID_ARG;
    pthread_once(&plugin_once, discover);
    return load(r->PluginPath, r->Type);
}
static CS_STATUS deregister_locked(const CsPluginRequest *r)
{
    if (!valid(r)) return CS_INVALID_ARG;
    char *path = realpath(r->PluginPath, NULL);
    if (!path) return CS_LOAD_ERROR;
    pthread_mutex_lock(&plugin_lock);
    struct cs_plugin **slot = &plugins;
    while (*slot && strcmp((*slot)->path, path)) slot = &(*slot)->next;
    free(path);
    struct cs_plugin *p = *slot;
    CS_STATUS s = !p ? CS_NO_MATCHING_DEVICE : !(r->Type & p->ops->type) ? CS_UNSUPPORTED_TYPE
                  : p->refs != 1 ? CS_HANDLE_IN_USE : CS_SUCCESS;
    if (s == CS_SUCCESS) { *slot = p->next; p->refs--; }
    pthread_mutex_unlock(&plugin_lock);
    if (s == CS_SUCCESS) dispose(p);
    return s;
}
CS_STATUS cs_plugin_deregister(const CsPluginRequest *r)
{
    if (!valid(r)) return CS_INVALID_ARG;
    pthread_once(&plugin_once, discover);
    pthread_mutex_lock(&module_lock);
    CS_STATUS s = deregister_locked(r);
    pthread_mutex_unlock(&module_lock);
    return s;
}
