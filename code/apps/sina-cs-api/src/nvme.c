#define _GNU_SOURCE
#include "internal.h"
#include <endian.h>
#include <errno.h>
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
    struct nvme_passthru_cmd cmd = {
        .opcode = 6, .nsid = ns, .addr = (uintptr_t)identify, .data_len = sizeof(identify)};
    if (cs_io(d, true, &cmd) != CS_SUCCESS)
        return -1;
    unsigned index = identify[26] & 15, shift = identify[128 + index * 4 + 2];
    if (shift < 9 || shift > 16)
        return -1;
    d->lba = 1U << shift;
    return 0;
}
void cs_transport_close(struct cs_device *d)
{
    if (d->fd >= 0)
        close(d->fd);
    if (d->admin >= 0)
        close(d->admin);
}
CS_STATUS cs_io(struct cs_device *d, bool admin, struct nvme_passthru_cmd *cmd)
{
    cmd->timeout_ms = 600000;
    int rc = ioctl(admin ? d->admin : d->fd, admin ? NVME_IOCTL_ADMIN_CMD : NVME_IOCTL_IO_CMD, cmd);
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
