/* Exercise the production NVMe backend, without opening real devices. */
#define _GNU_SOURCE
#include "femu_internal.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/nvme_ioctl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

static const char *model = "FEMU Computational Storage Controller";
static bool fail_controller, fail_namespace, fail_io;
static unsigned opens, closes, controller_reads, namespace_reads;

int __wrap_open(const char *path, int flags, ...)
{
    assert(flags == (O_RDWR | O_CLOEXEC));
    const char *paths[] = {"/dev/nvme0n1", "/dev/nvme0", "/dev/nvme1n1", "/dev/nvme1"};
    for (unsigned i = 0; i < 4; i++)
        if (!strcmp(path, paths[i])) {
            opens++;
            return 100 + i;
        }
    abort();
}

int __wrap_close(int fd)
{
    assert(fd >= 100 && fd <= 103);
    closes++;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    if (request == NVME_IOCTL_ID) {
        assert(fd == 100 || fd == 102);
        return 1;
    }
    va_list ap;
    va_start(ap, request);
    struct nvme_passthru_cmd *cmd = va_arg(ap, struct nvme_passthru_cmd *);
    va_end(ap);
    if (request == NVME_IOCTL_IO_CMD || request == NVME_IOCTL_IO64_CMD) {
        assert(fd == 100);
        if (fail_io) { errno = EACCES; return -1; }
        return 0;
    }
    assert((request == NVME_IOCTL_ADMIN_CMD || request == NVME_IOCTL_ADMIN64_CMD) &&
           (fd == 101 || fd == 103));
    if (request == NVME_IOCTL_ADMIN64_CMD) {
        struct nvme_passthru_cmd64 *wide = (void *)cmd;
        const uint64_t info[] = {1, 64U << 20, 0, 4, 3};
        assert(wide->opcode == 0xc0 && wide->cdw10 < 5);
        wide->result = info[wide->cdw10];
        return 0;
    }
    assert(cmd->opcode == 6 && cmd->data_len == 4096);
    unsigned char *data = (void *)(uintptr_t)cmd->addr;
    memset(data, 0, 4096);
    if (cmd->cdw10 == 1) {
        assert(cmd->nsid == 0);
        controller_reads++;
        if (fail_controller)
            return 1;
        const char *name = fd == 101 ? model : "Ordinary NVMe SSD";
        memset(data + 24, ' ', 40);
        memcpy(data + 24, name, strlen(name));
    } else {
        assert(cmd->cdw10 == 0 && cmd->nsid == 1);
        namespace_reads++;
        if (fail_namespace)
            return 1;
        uint64_t blocks = 8192;
        memcpy(data, &blocks, 8);
        data[130] = 9; /* LBA format 0: 512-byte sectors. */
    }
    return 0;
}

int __wrap_glob(const char *pattern, int flags, int (*error)(const char *, int), glob_t *out)
{
    assert(!strcmp(pattern, "/dev/nvme*n*") && !flags && !error);
    static char *paths[] = {"/dev/nvme0n1", "/dev/nvme1n1", NULL};
    out->gl_pathc = 2;
    out->gl_pathv = paths;
    return 0;
}

void __wrap_globfree(glob_t *out)
{
    (void)out;
}

static void rejected(const char *path)
{
    void *dev = (void *)1;
    assert(femu_openCSx(path, NULL, &dev, &(unsigned){0}) == CS_DEVICE_NOT_AVAILABLE);
    assert(dev == NULL && opens == closes);
}

int main(void)
{
    unsetenv("SNIA_FEMU_PROFILE");
    void *dev;
    assert(femu_openCSx("/dev/nvme0n1", NULL, &dev, &(unsigned){0}) == CS_SUCCESS);
    assert(controller_reads == 1 && namespace_reads == 1);
    femu_closeCSx(dev); assert(opens == closes);

    rejected("/dev/nvme1n1"); /* A configured ordinary SSD is still rejected. */
    assert(namespace_reads == 1);
    model = "FEMU Computational Storage ControllerX";
    rejected("/dev/nvme0n1"); /* Prefix matches do not establish the device identity. */
    assert(namespace_reads == 1);
    model = "FEMU Computational Storage Controller";
    fail_controller = true;
    rejected("/dev/nvme0n1");
    assert(namespace_reads == 1);
    fail_controller = false;
    fail_namespace = true;
    rejected("/dev/nvme0n1");
    fail_namespace = false;

    char list[128];
    int bytes = sizeof(list);
    assert(femu_queryCSxList(&bytes, list) == CS_SUCCESS);
    assert(!strcmp(list, "/dev/nvme0n1") && bytes == (int)strlen(list) + 1);
    assert(opens == closes);
    setenv("SNIA_FEMU_PROFILE", "1", 1);
    assert(femu_openCSx("/dev/nvme0n1", NULL, &dev, &(unsigned){0}) == CS_SUCCESS);
    struct cs_device *device = dev;
    assert(device->profile);
    for (unsigned i = 0; i < 5; ++i) assert(!device->io_timing[i].calls);
    assert(device->io_timing[11].calls == 2); // Controller and namespace Identify.
    assert(device->io_timing[12].calls == 5); // Admin C0 GET_INFO.
    const unsigned setup_opcodes[] = {0xb0, 0xc0, 0xf5};
    for (unsigned i = 0; i < 3; ++i) {
        struct nvme_passthru_cmd command = {.opcode = setup_opcodes[i]};
        assert(cs_io(device, false, &command) == CS_SUCCESS);
        assert(device->io_timing[6 + i].calls == 1);
    }
    assert(device->io_timing[12].calls == 5); // I/O C0 FREE is a different command.
    struct nvme_passthru_cmd64 compute = {.opcode = 0xe1};
    assert(cs_io64(device, false, &compute) == CS_SUCCESS);
    const unsigned opcodes[] = {0xd0, 0xd1, 9, 0xf2};
    for (unsigned i = 0; i < 4; ++i) {
        struct nvme_passthru_cmd command = {.opcode = opcodes[i]};
        assert(cs_io(device, false, &command) == CS_SUCCESS);
    }
    fail_io = true;
    struct nvme_passthru_cmd denied = {.opcode = 9};
    assert(cs_io(device, false, &denied) == CS_NO_PERMISSIONS); // Profiling preserves errno.
    fail_io = false;
    for (unsigned i = 0; i < 5; ++i) assert(device->io_timing[i].calls == (i == 3 ? 2U : 1U));
    device->profile = false;
    struct nvme_passthru_cmd unprofiled = {.opcode = 0xd0};
    assert(cs_io(device, false, &unprofiled) == CS_SUCCESS && device->io_timing[1].calls == 1);
    femu_closeCSx(dev);
    unsetenv("SNIA_FEMU_PROFILE");
    assert(opens == closes);
    puts("PASS: Identify Controller identity, ordinary SSD exclusion, errors and discovery");
    return 0;
}
