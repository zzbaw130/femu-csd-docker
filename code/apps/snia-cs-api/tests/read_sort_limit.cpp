// Guest baseline: ordinary file read, full descending std::sort, then LIMIT 5.
#include "batch_sort.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/data/cs-batch-input.bin";
    int file = open(path, O_RDONLY);
    struct stat info;
    if (file < 0 || flock(file, LOCK_SH | LOCK_NB) || fstat(file, &info) || info.st_size != SORT_FILE_BYTES) {
        std::fprintf(stderr, "expected a readable 256 MiB file: %s\n", path);
        if (file >= 0) close(file);
        return 1;
    }
    // Evict this clean file's Guest page-cache pages; no system-wide cache drop.
    int error = posix_fadvise(file, 0, SORT_FILE_BYTES, POSIX_FADV_DONTNEED);
    if (error) { errno = error; std::perror("posix_fadvise"); close(file); return 1; }
    constexpr size_t count = SORT_FILE_BYTES / sizeof(int32_t);
    std::unique_ptr<int32_t[]> values(new int32_t[count]);
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    for (size_t offset = 0; offset < SORT_FILE_BYTES;) {
        ssize_t bytes = read(file, reinterpret_cast<char *>(values.get()) + offset,
                             std::min<size_t>(SORT_CHUNK_BYTES, SORT_FILE_BYTES - offset));
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) { std::fprintf(stderr, "short read: %s\n", path); close(file); return 1; }
        offset += bytes;
    }
    auto read_done = Clock::now();
    std::sort(values.get(), values.get() + count, std::greater<int32_t>());
    std::array<int32_t, 5> top;
    std::copy_n(values.get(), top.size(), top.begin());
    auto end = Clock::now();
    std::printf("Top 5: %d %d %d %d %d\n", top[0], top[1], top[2], top[3], top[4]);
    double read_seconds = std::chrono::duration<double>(read_done - start).count();
    double sort_seconds = std::chrono::duration<double>(end - read_done).count();
    std::printf("TIMING CPU %.6f %.6f %.6f\n", read_seconds, sort_seconds, read_seconds + sort_seconds);
    close(file);
}
