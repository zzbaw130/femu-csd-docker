// SNIA 设备发现；字符串、插件快照和临时设备均由作用域管理。
#include "cs_internal.hpp"
using namespace snia;
CS_STATUS csQueryCSxList(int *length, char *out)
{
    return apiCall([&] {
        if (!length) return CS_INVALID_ARG;
        std::string devices;
        for (const auto &plugin : pluginsSnapshot()) {
            int bytes = 0;
            auto status = plugin->ops->queryCSxList(&bytes, nullptr);
            if (status != CS_SUCCESS && status != CS_INVALID_LENGTH) return status;
            if (bytes <= 1) continue;
            std::vector<char> names(bytes);
            check(plugin->ops->queryCSxList(&bytes, names.data()));
            if (!names[0]) continue;
            if (!devices.empty()) devices += ',';
            devices += names.data();
        }
        auto status = outputSize(length, out, devices.size() + 1);
        if (status == CS_SUCCESS) std::memcpy(out, devices.c_str(), devices.size() + 1);
        return status;
    });
}
CS_STATUS csGetCSxFromPath(const char *path, unsigned *length, char *out)
{
    return apiCall([&] {
        if (!path || !length) return CS_INVALID_ARG;
        CS_STATUS result = CS_NO_MATCHING_DEVICE;
        for (const auto &plugin : pluginsSnapshot()) {
            unsigned bytes = *length;
            auto status = plugin->ops->getCSxFromPath(path, &bytes, out);
            if (status == CS_SUCCESS || status == CS_INVALID_LENGTH) { *length = bytes; return status; }
            if (status == CS_INVALID_PATH) result = status;
        }
        return result;
    });
}
CS_STATUS csQueryCSFList(const char *path, int *length, int *count, CSFUniqueId *out)
{
    return apiCall([&] {
        if (!path || !length || !count) return CS_INVALID_ARG;
        char name[4096];
        unsigned bytes = sizeof(name);
        check(csGetCSxFromPath(path, &bytes, name));
        auto device = openDevice(name, nullptr);
        auto operation = device->ops().queryCSFList;
        return operation ? operation(path, length, count, out) : CS_UNSUPPORTED;
    });
}
