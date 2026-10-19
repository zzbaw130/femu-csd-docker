#include "internal.h"
CS_STATUS csQueryLibrarySupport(CS_LIBRARY_SUPPORT type, int *length, char *out)
{
    if (type != CS_FILE_SYSTEMS_SUPPORTED)
        return CS_UNSUPPORTED;
    CS_STATUS s = cs_output(length, out, 1);
    if (s == CS_SUCCESS)
        *out = 0;
    return s;
}
CS_STATUS csRegisterPlugin(const CsPluginRequest *req) { return cs_plugin_register(req); }
CS_STATUS csDeregisterPlugin(const CsPluginRequest *req) { return cs_plugin_deregister(req); }
