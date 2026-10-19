// SNIA 库管理；模块由 shared_ptr 保活，卸载路径无需手工引用计数。
#include "cs_common.hpp"
#include <dlfcn.h>
#include <glob.h>
#include <cstdlib>
using namespace snia;
namespace {
std::mutex pluginsMutex, moduleMutex;
std::once_flag discoverOnce;
std::vector<std::shared_ptr<Plugin>> plugins;
std::string canonicalPath(const char *name)
{
    char resolved[4096];
    return realpath(name, resolved) ? std::string(resolved) : std::string();
}
CS_STATUS loadModule(const std::string &path, unsigned type)
{
    std::lock_guard<std::mutex> moduleLock(moduleMutex);
    {
        std::lock_guard<std::mutex> lock(pluginsMutex);
        for (const auto &plugin : plugins)
            if (plugin->path == path)
                return !type || (type & plugin->ops->type) ? CS_SUCCESS : CS_UNSUPPORTED_TYPE;
    }
    auto plugin = std::make_shared<Plugin>();
    plugin->path = path;
    plugin->library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!plugin->library) return CS_LOAD_ERROR;
    auto create = reinterpret_cast<cs_backend_create_fn>(dlsym(plugin->library, "cs_backend_create"));
    plugin->destroy = reinterpret_cast<cs_backend_destroy_fn>(dlsym(plugin->library, "cs_backend_destroy"));
    plugin->ops = create && plugin->destroy ? create() : nullptr;
    const auto *ops = plugin->ops;
    if (!ops || ops->abi != CS_BACKEND_ABI || !ops->name || !ops->queryCSxList || !ops->getCSxFromPath ||
        !ops->openCSx || !ops->closeCSx || !ops->allocMem || !ops->freeMem || !ops->initMem ||
        (!ops->queueComputeRequest && !ops->queueCopyMemRequest && !ops->queueStorageRequest)) return CS_LOAD_ERROR;
    if (type && !(type & ops->type)) return CS_UNSUPPORTED_TYPE;
    std::lock_guard<std::mutex> lock(pluginsMutex);
    plugins.push_back(std::move(plugin));
    return CS_SUCCESS;
}
void discoverPlugins()
{
    Dl_info info;
    if (!dladdr(reinterpret_cast<void *>(csOpenCSx), &info)) return;
    std::string path = canonicalPath(info.dli_fname);
    auto slash = path.rfind('/');
    if (slash == std::string::npos) return;
    path.replace(slash, std::string::npos, "/snia-cs-api/*.so");
    struct Matches { glob_t paths{}; ~Matches() { globfree(&paths); } } matches;
    if (!glob(path.c_str(), 0, nullptr, &matches.paths))
        for (size_t i = 0; i < matches.paths.gl_pathc; ++i)
            loadModule(canonicalPath(matches.paths.gl_pathv[i]), 0);
}
bool validPluginRequest(const CsPluginRequest *request)
{
    return request && request->Type && !(request->Type & ~15U) && request->PluginPath[0] &&
           std::memchr(request->PluginPath, 0, sizeof(request->PluginPath));
}
} // namespace
namespace snia {
Plugin::~Plugin()
{
    if (ops) destroy(ops);
    if (library) dlclose(library);
}
std::vector<std::shared_ptr<Plugin>> pluginsSnapshot()
{
    std::call_once(discoverOnce, discoverPlugins);
    std::lock_guard<std::mutex> lock(pluginsMutex);
    return plugins;
}
} // namespace snia
CS_STATUS csRegisterPlugin(const CsPluginRequest *request)
{
    return apiCall([&] {
        if (!validPluginRequest(request)) return CS_INVALID_ARG;
        std::call_once(discoverOnce, discoverPlugins);
        auto path = canonicalPath(request->PluginPath);
        return path.empty() ? CS_LOAD_ERROR : loadModule(path, request->Type);
    });
}
CS_STATUS csDeregisterPlugin(const CsPluginRequest *request)
{
    return apiCall([&] {
        if (!validPluginRequest(request)) return CS_INVALID_ARG;
        std::call_once(discoverOnce, discoverPlugins);
        auto path = canonicalPath(request->PluginPath);
        if (path.empty()) return CS_LOAD_ERROR;
        std::lock_guard<std::mutex> moduleLock(moduleMutex);
        std::lock_guard<std::mutex> lock(pluginsMutex);
        for (auto it = plugins.begin(); it != plugins.end(); ++it) {
            if ((*it)->path != path) continue;
            if (!(request->Type & (*it)->ops->type)) return CS_UNSUPPORTED_TYPE;
            if (it->use_count() != 1) return CS_HANDLE_IN_USE;
            plugins.erase(it);
            return CS_SUCCESS;
        }
        return CS_NO_MATCHING_DEVICE;
    });
}
CS_STATUS csQueryLibrarySupport(CS_LIBRARY_SUPPORT type, int *length, char *out)
{
    return apiCall([&] {
        if (type != CS_FILE_SYSTEMS_SUPPORTED) return CS_UNSUPPORTED;
        auto status = outputSize(length, out, 1);
        if (status == CS_SUCCESS) *out = 0;
        return status;
    });
}
