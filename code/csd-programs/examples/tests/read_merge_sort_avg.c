/* Guest CPU baseline: file read -> 4 MiB runs -> full merge -> average.
 * Uses the same bounded workmem allocator and run sort as the device program.
 * No SNIA/NVMe calls. O_DIRECT keeps spill outside the Guest page cache. */
#define _GNU_SOURCE
#include "batch_sort.h"
#include "cs_workmem.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RUN_VALUES (SORT_CHUNK_BYTES / sizeof(int32_t))
#define BLOCK_VALUES (SORT_BLOCK_BYTES / sizeof(int32_t))
struct run {
    int32_t *data;
    cs_work_context *owner;
    off_t offset;
    unsigned position, loaded_block;
    bool spilled;
};
struct file_timing { struct sort_timing time; uint64_t calls; };
struct sort_state {
    cs_work_context *context;
    struct run runs[SORT_MAX_RUNS];
    unsigned count;
    struct sort_metrics metrics;
    struct sort_heap heap;
    bool root_pending;
    struct file_timing input_read, spill_write;
};
static struct file_timing input_read, spill_read, spill_write, release_file;
static atomic_uint spill_next;
static int transfer(int fd, void *buffer, size_t bytes, off_t offset, bool writing, struct file_timing *timing)
{
    struct sort_timing start = sort_clock();
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = writing ? pwrite(fd, (char *)buffer + done, bytes - done, offset + done)
                            : pread(fd, (char *)buffer + done, bytes - done, offset + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += n;
    }
    sort_record(&timing->time, start);
    ++timing->calls;
    return 0;
}
static int resident(const struct sort_state *s, cs_work_context *owner)
{
    for (unsigned i = 0; i < s->count; ++i)
        if (s->runs[i].owner == owner && !s->runs[i].spilled && s->runs[i].data) return (int)i;
    return -1;
}
static int spill(struct sort_state *s, unsigned index, int file, void *io)
{
    struct run *run = &s->runs[index];
    run->offset = (off_t)atomic_fetch_add(&spill_next, 1) * SORT_CHUNK_BYTES;
    for (unsigned block = 0; block < SORT_BLOCKS_PER_RUN; ++block) {
        memcpy(io, run->data + block * BLOCK_VALUES, SORT_BLOCK_BYTES);
        if (transfer(file, io, SORT_BLOCK_BYTES, run->offset + block * SORT_BLOCK_BYTES, true, &s->spill_write)) return -1;
        s->metrics.spill_write_bytes += SORT_BLOCK_BYTES;
    }
    cs_work_free(run->owner, run->data);
    run->data = NULL;
    run->spilled = true;
    ++s->metrics.spilled_runs;
    return 0;
}
static int32_t head(const struct run *run)
{
    return run->data[run->spilled ? run->position % BLOCK_VALUES : run->position];
}
static int load_head(struct sort_state *s, unsigned index, int file, void *io)
{
    struct run *run = &s->runs[index];
    if (!run->spilled) return 0;
    if (!run->data) {
        while (!(run->data = cs_work_alloc(run->owner, SORT_BLOCK_BYTES))) {
            int victim = resident(s, run->owner);
            if (victim < 0 || spill(s, (unsigned)victim, file, io)) return -1;
        }
        run->loaded_block = SORT_BLOCKS_PER_RUN;
    }
    unsigned block = run->position / BLOCK_VALUES;
    if (run->loaded_block == block) return 0;
    if (transfer(file, io, SORT_BLOCK_BYTES, run->offset + block * SORT_BLOCK_BYTES, false, &spill_read)) return -1;
    memcpy(run->data, io, SORT_BLOCK_BYTES);
    run->loaded_block = block;
    s->metrics.spill_read_bytes += SORT_BLOCK_BYTES;
    return 0;
}
static double seconds(struct sort_timing first, struct sort_timing last)
{
    return (last.wall_ns - first.wall_ns) / 1e9;
}
struct worker {
    int file, temporary, error;
    unsigned lane;
    int32_t *input;
    void *io;
    cs_work_context *context;
    struct sort_state *state;
};
static void *generate_runs(void *opaque)
{
    struct worker *w = opaque;
    w->context = cs_work_begin_limit(sort_work_bytes(SORT_WORKERS) / SORT_WORKERS);
    struct sort_state *s = w->context ? cs_work_calloc(w->context, 1, sizeof(*s)) : NULL;
    w->state = s;
    if (!s) { w->error = 1; return NULL; }
    s->context = w->context;
    struct sort_timing start = sort_clock();
    for (unsigned chunk = w->lane; chunk < SORT_MAX_RUNS; chunk += SORT_WORKERS) {
        if (transfer(w->file, w->input, SORT_CHUNK_BYTES, (off_t)chunk * SORT_CHUNK_BYTES, false, &s->input_read)) {
            w->error = 1; return NULL;
        }
        int32_t *data;
        while (!(data = cs_work_alloc(s->context, SORT_CHUNK_BYTES))) {
            int victim = resident(s, s->context);
            if (victim < 0 || spill(s, (unsigned)victim, w->temporary, w->io)) { w->error = 1; return NULL; }
        }
        memcpy(data, w->input, SORT_CHUNK_BYTES);
        struct sort_timing sort_start = sort_clock();
        sort_int32_desc(data, RUN_VALUES);
        s->metrics.run_sort_ns += sort_clock().wall_ns - sort_start.wall_ns;
        s->runs[s->count].owner = s->context;
        s->runs[s->count++].data = data;
    }
    struct sort_timing end = sort_clock();
    s->metrics.sink.wall_ns = end.wall_ns - start.wall_ns - s->input_read.time.wall_ns - s->spill_write.time.wall_ns;
    s->metrics.sink.cpu_ns = end.cpu_ns - start.cpu_ns - s->input_read.time.cpu_ns - s->spill_write.time.cpu_ns;
    return NULL;
}
static void add_file_timing(struct file_timing *to, const struct file_timing *from)
{
    to->calls += from->calls;
    to->time.wall_ns += from->time.wall_ns;
    to->time.cpu_ns += from->time.cpu_ns;
}
int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/data/cs-batch-input.bin";
    int rc = 1, file = -1, temporary = -1;
    struct worker workers[SORT_WORKERS] = {0};
    void *io = NULL;
    struct sorted_chunk *output = NULL;
    struct average_result *result = NULL;
    struct stat info;
    file = open(path, O_RDONLY | O_DIRECT);
    if (file < 0 || flock(file, LOCK_SH | LOCK_NB) || fstat(file, &info) || info.st_size != SORT_FILE_BYTES) {
        fprintf(stderr, "expected a readable 256 MiB file: %s\n", path);
        goto cleanup;
    }
    /* The temporary file is beside the input, on the same FEMU data partition. */
    char temporary_path[PATH_MAX];
    const char *slash = strrchr(path, '/');
    int directory_bytes = slash ? (int)(slash - path + 1) : 0;
    if (snprintf(temporary_path, sizeof(temporary_path), "%.*s.sort-spill-XXXXXX", directory_bytes, path)
        >= (int)sizeof(temporary_path)) goto cleanup;
    temporary = mkstemp(temporary_path);
    if (temporary < 0) goto io_error;
    if (unlink(temporary_path) || fcntl(temporary, F_SETFL, O_DIRECT)) goto io_error;
    // Count all four input/staging pairs, task metadata and output in 64 MiB.
    for (unsigned i = 0; i < SORT_WORKERS; ++i) {
        workers[i].file = file; workers[i].temporary = temporary; workers[i].lane = i;
        if (posix_memalign((void **)&workers[i].input, 4096, SORT_CHUNK_BYTES) ||
            posix_memalign(&workers[i].io, 4096, SORT_BLOCK_BYTES)) goto cleanup;
    }
    io = workers[0].io;
    output = malloc(sizeof(*output));
    result = calloc(1, sizeof(*result));
    if (!output || !result || cs_work_init(sort_work_bytes(SORT_WORKERS))) goto cleanup;
    printf("CONFIG CPU sink_workers=%u merge_workers=1 work_bytes=%zu buffer_bytes=%zu\n",
           SORT_WORKERS, sort_work_bytes(SORT_WORKERS), sort_buffer_bytes(SORT_WORKERS));
    struct sort_timing start = sort_clock(), sink_done, end;
    pthread_t threads[SORT_WORKERS];
    unsigned started = 0;
    for (; started < SORT_WORKERS; ++started)
        if (pthread_create(&threads[started], NULL, generate_runs, &workers[started])) break;
    for (unsigned i = 0; i < started; ++i) pthread_join(threads[i], NULL);
    if (started != SORT_WORKERS) goto cleanup;
    for (unsigned i = 0; i < SORT_WORKERS; ++i) if (workers[i].error) goto io_error;
    struct sort_state *s = workers[0].state;
    for (unsigned i = 0; i < SORT_WORKERS; ++i) {
        struct sort_state *other = workers[i].state;
        add_file_timing(&input_read, &other->input_read);
        add_file_timing(&spill_write, &other->spill_write);
        if (!i) continue;
        memcpy(s->runs + s->count, other->runs, other->count * sizeof(*s->runs));
        s->count += other->count;
        s->metrics.spilled_runs += other->metrics.spilled_runs;
        s->metrics.spill_write_bytes += other->metrics.spill_write_bytes;
        s->metrics.sink.wall_ns += other->metrics.sink.wall_ns;
        s->metrics.sink.cpu_ns += other->metrics.sink.cpu_ns;
        s->metrics.run_sort_ns += other->metrics.run_sort_ns;
    }
    s->spill_write = spill_write;
    sink_done = sort_clock();
    struct sort_timing sink_io = spill_write.time;
    for (unsigned i = 0; i < s->count; ++i) {
        if (load_head(s, i, temporary, io)) goto io_error;
        struct sort_heap_entry entry = {head(&s->runs[i]), i};
        sort_heap_push(&s->heap, entry);
    }
    uint64_t emitted = 0;
    do {
        output->count = 0;
        while (output->count < SORT_OUTPUT_VALUES && emitted < SORT_FILE_BYTES / sizeof(int32_t)) {
            unsigned index = s->heap.entries[0].run;
            struct run *run = &s->runs[index];
            if (s->root_pending) {
                if (run->position == RUN_VALUES) {
                    cs_work_free(run->owner, run->data);
                    run->data = NULL;
                    sort_heap_pop(&s->heap);
                } else {
                    if (load_head(s, index, temporary, io)) goto io_error;
                    struct sort_heap_entry entry = {head(run), index};
                    sort_heap_replace(&s->heap, entry);
                }
                s->root_pending = false;
                continue;
            }
            output->values[output->count++] = s->heap.entries[0].value;
            ++run->position;
            s->root_pending = true;
            ++emitted;
        }
        output->finished = emitted == SORT_FILE_BYTES / sizeof(int32_t);
        if (output->finished) output->metrics = s->metrics;
        average_chunk(result, output);
    } while (emitted < SORT_FILE_BYTES / sizeof(int32_t));
    result->metrics = s->metrics;
    result->metrics.runs = s->count;
    result->metrics.peak_bytes = sort_buffer_bytes(SORT_WORKERS);
    spill_write = s->spill_write;
    for (unsigned i = 0; i < SORT_WORKERS; ++i) {
        cs_work_context_stats stats;
        if (cs_work_get_context_stats(workers[i].context, &stats)) goto cleanup;
        result->metrics.peak_bytes += stats.peak_used_bytes;
    }
    for (unsigned i = 0; i < SORT_WORKERS; ++i) {
        cs_work_end(workers[i].context);
        workers[i].context = NULL;
    }
    /* Closing the unlinked file releases all spill extents inside the timer. */
    struct sort_timing close_start = sort_clock();
    int close_status = close(temporary);
    sort_record(&release_file.time, close_start);
    release_file.calls = 1;
    temporary = -1;
    if (close_status) goto io_error;
    end = sort_clock();
    result->metrics.source.wall_ns = end.wall_ns - sink_done.wall_ns - spill_read.time.wall_ns
        - (spill_write.time.wall_ns - sink_io.wall_ns) - release_file.time.wall_ns - result->aggregate.wall_ns;
    result->metrics.source.cpu_ns = end.cpu_ns - sink_done.cpu_ns - spill_read.time.cpu_ns
        - (spill_write.time.cpu_ns - sink_io.cpu_ns) - release_file.time.cpu_ns - result->aggregate.cpu_ns;
    if (!result->finished || result->count != SORT_FILE_BYTES / sizeof(int32_t)) goto cleanup;
    printf("AVERAGE count=%llu sum=%lld value=%.17g\n", (unsigned long long)result->count,
           (long long)result->sum, result->average);
    printf("WORKLOAD CPU runs=%u spilled=%u write=%llu read=%llu peak=%llu limit=%u\n",
           result->metrics.runs, result->metrics.spilled_runs,
           (unsigned long long)result->metrics.spill_write_bytes, (unsigned long long)result->metrics.spill_read_bytes,
           (unsigned long long)result->metrics.peak_bytes, SORT_MEMORY_BYTES);
    printf("TIMING CPU %.6f %.6f %.6f\n", seconds(start, sink_done), seconds(sink_done, end), seconds(start, end));
    printf("PROFILE CPU sink_work wall_s=%.9f cpu_s=%.9f run_sort_s=%.9f\n",
           result->metrics.sink.wall_ns / 1e9, result->metrics.sink.cpu_ns / 1e9, result->metrics.run_sort_ns / 1e9);
    printf("PROFILE CPU source_work wall_s=%.9f cpu_s=%.9f\n",
           result->metrics.source.wall_ns / 1e9, result->metrics.source.cpu_ns / 1e9);
    printf("PROFILE CPU average_work wall_s=%.9f cpu_s=%.9f\n",
           result->aggregate.wall_ns / 1e9, result->aggregate.cpu_ns / 1e9);
    const char *names[] = {"input_read", "spill_write", "spill_read", "release"};
    const struct file_timing *timings[] = {&input_read, &spill_write, &spill_read, &release_file};
    for (unsigned i = 0; i < 4; ++i)
        printf("PROFILE CPU %s calls=%llu wall_s=%.9f cpu_s=%.9f\n", names[i],
               (unsigned long long)timings[i]->calls, timings[i]->time.wall_ns / 1e9, timings[i]->time.cpu_ns / 1e9);
    rc = 0;
    goto cleanup;
io_error:
    perror("file I/O");
cleanup:
    for (unsigned i = 0; i < SORT_WORKERS; ++i) {
        if (workers[i].context) cs_work_end(workers[i].context);
        free(workers[i].io); free(workers[i].input);
    }
    free(result); free(output);
    if (temporary >= 0) close(temporary);
    if (file >= 0) close(file);
    return rc;
}
