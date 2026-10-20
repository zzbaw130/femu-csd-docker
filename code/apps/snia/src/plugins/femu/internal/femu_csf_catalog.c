#include "femu_internal.h"
pthread_mutex_t cs_catalog_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cs_catalog_entry *catalog;
struct cs_catalog_entry *cs_catalog_find_locked(struct cs_device *d, unsigned id)
{
    for (struct cs_catalog_entry *e = catalog; e; e = e->next)
        if (e->info.id == id && !strcmp(e->path, d->path))
            return e;
    return NULL;
}
struct cs_catalog_entry *cs_catalog_reserve_locked(struct cs_device *d, unsigned id)
{
    struct cs_catalog_entry *e = cs_catalog_find_locked(d, id);
    if (e)
        return e;
    e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    strcpy(e->path, d->path);
    e->info.id = id;
    e->next = catalog;
    catalog = e;
    return e;
}
CS_STATUS cs_program(struct cs_device *d, unsigned start, struct cs_program_info *out)
{
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&cs_catalog_lock);
    for (struct cs_catalog_entry *e = catalog; e; e = e->next)
        if (!strcmp(e->path, d->path) && e->info.type && e->info.id >= start &&
            (!out->id || e->info.id < out->id))
            *out = e->info;
    pthread_mutex_unlock(&cs_catalog_lock);
    return CS_SUCCESS;
}
void cs_catalog_clear(void)
{
    while (catalog) {
        struct cs_catalog_entry *e = catalog;
        catalog = e->next;
        free(e);
    }
}
