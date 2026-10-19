/* FEMU vendor transport discovery; the public API remains SNIA v1.1. */
#include "internal.h"
int cs_profile_load(struct cs_device *d, const char *name)
{
    (void)name;
    uint64_t values[5];
    for (unsigned i = 0; i < 5; i++) {
        struct nvme_passthru_cmd64 cmd = {.opcode = 0xc0, .cdw10 = i};
        if (cs_io64(d, true, &cmd) != CS_SUCCESS)
            return -1;
        values[i] = cmd.result;
    }
    if (values[0] != 1 || !values[1] || !values[3] || values[3] > 65535 ||
        (values[4] & 3) != 3)
        return -1;
    d->caps.fdm_bytes = values[1];
    d->caps.workers = values[3];
    d->caps.features = values[4];
    return 0;
}
