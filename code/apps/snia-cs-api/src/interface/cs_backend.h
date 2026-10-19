/* Internal plugin contract. Applications include only cs.h.
 * All handles crossing this interface are plugin-owned tokens, not public handles.
 * CSF tokens remain valid until close; function_id maps them to the device CSF ID.
 * Requests borrow tokens while the core pins their public owners. Methods are
 * synchronous; async completion and batch dependencies are provided by the core. */
#ifndef CS_BACKEND_H
#define CS_BACKEND_H
#include "cs.h"
#include <stdbool.h>
#define CS_BACKEND_ABI 1
struct cs_backend_ops {
    unsigned abi;
    const char *name;
    CS_PLUGIN_TYPE type;
    CS_STATUS (*enumerate)(int *, char *);
    CS_STATUS (*resolve)(const char *, unsigned *, char *);
    CS_STATUS (*open)(const char *, void *, void **, unsigned *);
    void (*close)(void *);
    CS_STATUS (*csf_list)(const char *, int *, int *, CSFUniqueId *);
    CS_STATUS (*get_csf)(void *, const char *, u64, int *, int *, CSFIdinfo *);
    u32 (*function_id)(void *, CS_CSF_ID);
    CS_STATUS (*alloc)(void *, int, const CsMemFlags *, void **, u64 *, CS_MEM_PTR *);
    CS_STATUS (*free_mem)(void *, void *, const CsMemFlags *);
    CS_STATUS (*init_mem)(void *, void *, unsigned long, int, const CsMemFlags *);
    CS_STATUS (*validate)(void *, const CsBatchRequest *);
    CS_STATUS (*execute)(void *, const CsBatchRequest *, u64 *);
    /* Optional: execute one logical batch node, including adapter-specific
     * resumable computation. scope identifies this execution for cleanup. */
    CS_STATUS (*execute_batch)(void *, const CsBatchRequest *, bool (*)(void *), void *, void *, u64 *);
    void (*scope_end)(void *, void *, bool);
    CS_STATUS (*properties)(void *, CS_RESOURCE_TYPE, int *, CsProperties *);
    CS_STATUS (*statistics)(void *, CS_STAT_TYPE, void *, CsStatsInfo *);
    CS_STATUS (*download_csf)(void *, const CsCSFDownloadInfo *, u32 *);
    CS_STATUS (*download_csee)(void *, const CsCSEEDownloadInfo *, u32 *);
    CS_STATUS (*config)(void *, int *, const CsConfigInfo *, CsConfigData *);
    CS_STATUS (*reset)(void *, CS_RESOURCE_TYPE, u32);
    CS_STATUS (*notify)(const char *, u32, csDevNotificationFn, bool);
};
/* Every plugin exports these two C symbols, created once per loaded library.
 * The core calls destroy only after all device and discovery references end. */
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
