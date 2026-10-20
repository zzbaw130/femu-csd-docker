/* SNIA device open/close; owns the namespace transport and spill-pool reference. */
#include "femu_internal.h"
CS_STATUS femu_openCSx(const char *name, void *context, void **out, unsigned *workers)
{
    (void)context;
    *out = NULL;
    struct cs_device *d = calloc(1, sizeof(*d));
    if (!d) return CS_OUT_OF_RESOURCES;
    d->fd = d->admin = -1;
    if (cs_transport_open(d, name) < 0 || cs_profile_load(d, name) < 0) {
        cs_transport_close(d); free(d); return CS_DEVICE_NOT_AVAILABLE;
    }
    *out = d; *workers = d->caps.workers;
    return CS_SUCCESS;
}
void femu_closeCSx(void *device)
{
    struct cs_device *d = device;
    cs_spill_detach(d); cs_transport_close(d); free(d);
}
