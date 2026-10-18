/* Explicit laboratory binding: original FEMU exposes no capability query.
 * Entries: /dev/nvme0n1:67108864:4[,/dev/nvme1n1:...]. No implicit probing. */
#define _GNU_SOURCE
#include "internal.h"
#include <errno.h>
#include <stdio.h>
int cs_profile_load(struct cs_device *d, const char *name)
{
    const char *config = getenv("SINA_CS_FEMU_DEVICES");
    if (!config || !*config)
        return -1;
    char *copy = strdup(config);
    if (!copy)
        return -1;
    int result = -1;
    char *save = NULL;
    for (char *entry = strtok_r(copy, ",", &save); entry; entry = strtok_r(NULL, ",", &save)) {
        char *size = strchr(entry, ':');
        if (!size)
            continue;
        *size++ = 0;
        if (strcmp(name, entry))
            continue;
        char *workers = strchr(size, ':');
        if (!workers)
            break;
        *workers++ = 0;
        char *end;
        errno = 0;
        unsigned long long bytes = strtoull(size, &end, 10);
        if (errno || *size < '0' || *size > '9' || *end || !bytes || bytes % 4096)
            break;
        errno = 0;
        unsigned long count = strtoul(workers, &end, 10);
        if (errno || *workers < '0' || *workers > '9' || *end || !count || count > 65535)
            break;
        d->caps.workers = count;
        d->caps.fdm_bytes = bytes;
        result = 0;
        break;
    }
    free(copy);
    return result;
}
