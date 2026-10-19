#define _GNU_SOURCE
#include "femu.h"
#include <endian.h>
#include <errno.h>
#include <glob.h>
#include <sys/file.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
int cs_transport_open(struct cs_device *d, const char *name)
{
    unsigned controller, ns;
    int end = 0;
    if (sscanf(name, "/dev/nvme%un%u%n", &controller, &ns, &end) != 2 || name[end] || !ns ||
        strlen(name) >= sizeof(d->path)) {
        errno = EINVAL;
        return -1;
    }
    char admin[64];
    snprintf(admin, sizeof(admin), "/dev/nvme%u", controller);
    d->fd = open(name, O_RDWR | O_CLOEXEC);
    if (d->fd < 0)
        return -1;
    d->admin = open(admin, O_RDWR | O_CLOEXEC);
    if (d->admin < 0)
        return -1;
    int actual = ioctl(d->fd, NVME_IOCTL_ID);
    if (actual <= 0 || actual != (int)ns)
        return -1;
    d->nsid = actual;
    snprintf(d->path, sizeof(d->path), "%s", name);
    unsigned char identify[4096] = {0};
    /* FEMU's existing controller model identifies its CSD mode. This is a
     * backend-specific check, not a SNIA-defined NVMe capability bit. */
    struct nvme_passthru_cmd controller_id = {
        .opcode = 6, .cdw10 = 1, .addr = (uintptr_t)identify, .data_len = sizeof(identify)};
    if (cs_io(d, true, &controller_id) != CS_SUCCESS)
        return -1;
    static const char model[] = "FEMU Computational Storage Controller";
    if (memcmp(identify + 24, model, sizeof(model) - 1))
        return -1;
    for (size_t i = sizeof(model) - 1; i < 40; i++)
        if (identify[24 + i] != ' ')
            return -1;
    memset(identify, 0, sizeof(identify));
    struct nvme_passthru_cmd cmd = {
        .opcode = 6, .nsid = ns, .addr = (uintptr_t)identify, .data_len = sizeof(identify)};
    if (cs_io(d, true, &cmd) != CS_SUCCESS)
        return -1;
    unsigned index = identify[26] & 15, shift = identify[128 + index * 4 + 2];
    if (shift < 9 || shift > 16)
        return -1;
    d->lba = 1U << shift;
    uint64_t blocks;
    memcpy(&blocks, identify, sizeof(blocks));
    d->blocks = le64toh(blocks);
    return 0;
}
void cs_transport_close(struct cs_device *d)
{
    if (d->fd >= 0)
        close(d->fd);
    if (d->admin >= 0)
        close(d->admin);
}
static CS_STATUS io_status(int rc)
{
    if (rc < 0) {
        if (errno == EPERM || errno == EACCES)
            return CS_NO_PERMISSIONS;
        if (errno == ETIMEDOUT)
            return CS_IO_TIMEOUT;
        return CS_DEVICE_ERROR;
    }
    if (rc) {
        unsigned status = rc & 0x7ff;
        if (status == 0x81)
            return CS_NOT_ENOUGH_MEMORY;
        if (status == 1)
            return CS_UNSUPPORTED;
        return CS_ERROR_IN_EXECUTION;
    }
    return CS_SUCCESS;
}

CS_STATUS cs_io(struct cs_device *d, bool admin, struct nvme_passthru_cmd *cmd)
{
    cmd->timeout_ms = 600000;
    return io_status(ioctl(admin ? d->admin : d->fd,
                           admin ? NVME_IOCTL_ADMIN_CMD : NVME_IOCTL_IO_CMD, cmd));
}
CS_STATUS cs_io64(struct cs_device *d, bool admin, struct nvme_passthru_cmd64 *cmd)
{
    cmd->timeout_ms = 600000;
    return io_status(ioctl(admin ? d->admin : d->fd,
                           admin ? NVME_IOCTL_ADMIN64_CMD : NVME_IOCTL_IO64_CMD, cmd));
}
/* GPT PARTNAME determines the role, never the partition number. Sysfs start/size
 * use 512-byte sectors even on a namespace formatted with 4 KiB LBAs. */
int cs_spill_region(struct cs_device *d, uint64_t *start, uint64_t *blocks)
{
    const char *name = strrchr(d->path, '/') + 1;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "/sys/class/block/%s/%sp*/uevent", name, name);
    glob_t matches = {0};
    int fd = -1;
    if (glob(pattern, 0, NULL, &matches))
        return -1;
    for (size_t i = 0; i < matches.gl_pathc; i++) {
        char path[4096], line[256];
        snprintf(path, sizeof(path), "%s", matches.gl_pathv[i]);
        FILE *f = fopen(path, "r");
        bool spill = false;
        if (!f)
            continue;
        while (fgets(line, sizeof(line), f))
            if (!strcmp(line, "PARTNAME=femu-spill\n"))
                spill = true;
        fclose(f);
        if (!spill)
            continue;
        char *tail = strrchr(path, '/') + 1;
        strcpy(tail, "start");
        unsigned long long first, count;
        f = fopen(path, "r");
        if (!f)
            break;
        int ok = fscanf(f, "%llu", &first);
        fclose(f);
        if (ok != 1)
            break;
        strcpy(tail, "size");
        f = fopen(path, "r");
        if (!f)
            break;
        ok = fscanf(f, "%llu", &count);
        fclose(f);
        if (ok != 1 || !count || first > UINT64_MAX / 512 || count > UINT64_MAX / 512 ||
            first * 512 % d->lba || count * 512 % d->lba)
            break;
        *start = first * 512 / d->lba;
        *blocks = count * 512 / d->lba;
        if (*start > d->blocks || *blocks > d->blocks - *start)
            break;
        tail[-1] = 0;
        const char *part = strrchr(path, '/') + 1;
        char device[256];
        snprintf(device, sizeof(device), "/dev/%s", part);
        fd = open(device, O_RDWR | O_CLOEXEC);
        if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB)) {
            close(fd);
            fd = -1;
        }
        break;
    }
    globfree(&matches);
    return fd;
}
