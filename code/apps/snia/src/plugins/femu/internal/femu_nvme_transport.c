#define _GNU_SOURCE
#include "femu_internal.h"
#include <endian.h>
#include <errno.h>
#include <glob.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
/* Bit 8 separates admin opcodes from I/O opcodes with the same byte value. */
static const unsigned profile_opcodes[] = {0xe1, 0xd0, 0xd1, 9, 0xf2, 0xe9,
    0xb0, 0xc0, 0xf5, 0x125, 0x123, 0x106, 0x1c0};
static const char *profile_names[] = {"E1", "D0", "D1", "TRIM", "F2", "EXEC_GRAPH",
    "ALLOC", "FREE", "F5", "CSF_DOWNLOAD", "CSF_ACTIVATE", "IDENTIFY", "GET_INFO"};
/* A serial LOAD->CSF batch runs both requests on the same SNIA thread. */
static _Thread_local struct cs_device *load_device;
static _Thread_local uint64_t load_returned;
static uint64_t profile_clock(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
static uint64_t profile_record(struct cs_device *d, bool admin, unsigned opcode, uint64_t start)
{
    if (!d->profile) return 0;
    if (admin) opcode |= 0x100;
    uint64_t end = profile_clock(), elapsed = end - start;
    for (unsigned i = 0; i < sizeof(profile_opcodes)/sizeof(profile_opcodes[0]); ++i) if (profile_opcodes[i] == opcode) {
        __atomic_fetch_add(&d->io_timing[i].calls, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&d->io_timing[i].wall_ns, elapsed, __ATOMIC_RELAXED);
        break;
    }
    return end;
}
int cs_transport_open(struct cs_device *d, const char *name)
{
    const char *profile = getenv("SNIA_FEMU_PROFILE");
    d->profile = profile && !strcmp(profile, "1");
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
    if (load_device == d) { load_device = NULL; load_returned = 0; }
    if (d->profile) for (unsigned i = 0; i < sizeof(profile_opcodes)/sizeof(profile_opcodes[0]); ++i)
        fprintf(stderr, "PROFILE CSD %s calls=%llu wall_s=%.9f\n", profile_names[i],
                (unsigned long long)d->io_timing[i].calls, d->io_timing[i].wall_ns / 1e9);
    if (d->profile)
        fprintf(stderr, "PROFILE CSD LOAD_to_E1 calls=%llu wall_s=%.9f\n",
                (unsigned long long)d->load_to_compute_calls, d->load_to_compute_ns / 1e9);
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
    uint64_t start = d->profile ? profile_clock() : 0;
    int result = ioctl(admin ? d->admin : d->fd,
                       admin ? NVME_IOCTL_ADMIN_CMD : NVME_IOCTL_IO_CMD, cmd);
    int saved_errno = errno;
    uint64_t end = profile_record(d, admin, cmd->opcode, start);
    if (end && cmd->opcode == 0xd0 && !result) {
        load_device = d; load_returned = end;
    }
    errno = saved_errno;
    return io_status(result);
}
CS_STATUS cs_io64(struct cs_device *d, bool admin, struct nvme_passthru_cmd64 *cmd)
{
    cmd->timeout_ms = 600000;
    if(!admin && cmd->opcode==0xe9 && load_device==d) { load_device=NULL; load_returned=0; }
    uint64_t start = d->profile ? profile_clock() : 0;
    if (start && !admin && cmd->opcode == 0xe1 && load_device == d && load_returned) {
        __atomic_fetch_add(&d->load_to_compute_calls, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&d->load_to_compute_ns, start-load_returned, __ATOMIC_RELAXED);
        load_device = NULL; load_returned = 0;
    }
    int result = ioctl(admin ? d->admin : d->fd,
                       admin ? NVME_IOCTL_ADMIN64_CMD : NVME_IOCTL_IO64_CMD, cmd);
    int saved_errno = errno;
    profile_record(d, admin, cmd->opcode, start);
    errno = saved_errno;
    return io_status(result);
}
