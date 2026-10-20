/* Guest CPU: four run builders -> exact midpoint -> two parallel merges -> AVG.
 * Uses the same bounded workmem allocator and run sort as the device program.
 * No SNIA/NVMe calls. O_DIRECT keeps spill outside the Guest page cache. */
#define _GNU_SOURCE
#include "merge_path_sort.h"
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
    bool spilled;
};
struct file_timing { struct sort_timing time; uint64_t calls; };
struct source {
    int32_t *buffers[SORT_MAX_RUNS];
    unsigned position[SORT_MAX_RUNS], end[SORT_MAX_RUNS], loaded[SORT_MAX_RUNS];
    struct sort_heap heap;
    uint64_t emitted, target;
    unsigned initialized;
    bool root_pending;
    struct sort_timing time;
    struct file_timing read;
};
struct sort_state {
    cs_work_context *context;
    struct run runs[SORT_MAX_RUNS];
    unsigned count;
    struct sort_metrics metrics;
    struct source sources[MERGE_PATH_WORKERS];
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
static int load_value(struct sort_state *s, struct source *source, unsigned index,
                      unsigned position, int file, void *io, int32_t *value)
{
    const struct run *run = &s->runs[index];
    if (!run->spilled) { *value = run->data[position]; return 0; }
    unsigned block = position / BLOCK_VALUES;
    if (source->loaded[index] != block) {
        if (transfer(file, io, SORT_BLOCK_BYTES, run->offset + block * SORT_BLOCK_BYTES, false, &source->read)) return -1;
        memcpy(source->buffers[index], io, SORT_BLOCK_BYTES);
        source->loaded[index] = block;
    }
    *value = source->buffers[index][position % BLOCK_VALUES];
    return 0;
}
static int prepare(struct sort_state *s, int file, void *io)
{
restart:
    for (unsigned i = 0; i < s->count; ++i) if (s->runs[i].spilled)
        for (unsigned part = 0; part < MERGE_PATH_WORKERS; ++part) {
            struct source *source = &s->sources[part];
            if (source->buffers[i]) continue;
            source->buffers[i] = cs_work_alloc(s->runs[i].owner, SORT_BLOCK_BYTES);
            if (!source->buffers[i]) {
                int victim = resident(s, s->runs[i].owner);
                if (victim < 0 || spill(s, (unsigned)victim, file, io)) return -1;
                goto restart;
            }
            source->loaded[i] = SORT_BLOCKS_PER_RUN;
        }
    return 0;
}
static int partition(struct sort_state *s, int file, void *io)
{
    struct merge_path_partition path = {.count = s->count,
        .remaining = (uint64_t)s->count * RUN_VALUES / 2};
    unsigned run, position;
    while (merge_path_probe(&path, RUN_VALUES, &run, &position)) {
        int32_t value;
        if (load_value(s, &s->sources[0], run, position, file, io, &value)) return -1;
        merge_path_consider(&path, value);
    }
    for (unsigned i = 0; i < s->count; ++i) {
        s->sources[0].end[i] = path.cut[i];
        s->sources[1].position[i] = path.cut[i];
        s->sources[1].end[i] = RUN_VALUES;
    }
    s->sources[0].target = (uint64_t)s->count * RUN_VALUES / 2;
    s->sources[1].target = (uint64_t)s->count * RUN_VALUES - s->sources[0].target;
    return 0;
}
struct merge_worker {
    struct sort_state *state;
    unsigned part;
    int file, error;
    void *io;
    struct sorted_chunk *output;
    struct average_result *result;
};
static int merge_chunk(struct merge_worker *w)
{
    struct sort_state *s = w->state;
    struct source *source = &s->sources[w->part];
    struct sorted_chunk *output = w->output;
    while (source->initialized < s->count) {
        unsigned i = source->initialized;
        if (source->position[i] < source->end[i]) {
            int32_t value;
            if (load_value(s, source, i, source->position[i], w->file, w->io, &value)) return -1;
            sort_heap_push(&source->heap, (struct sort_heap_entry){value, i});
        }
        ++source->initialized;
    }
    output->count = 0;
    while (output->count < SORT_OUTPUT_VALUES && source->emitted < source->target) {
        unsigned i = source->heap.entries[0].run;
        if (source->root_pending) {
            if (source->position[i] == source->end[i]) sort_heap_pop(&source->heap);
            else {
                int32_t value;
                if (load_value(s, source, i, source->position[i], w->file, w->io, &value)) return -1;
                sort_heap_replace(&source->heap, (struct sort_heap_entry){value, i});
            }
            source->root_pending = false; continue;
        }
        output->values[output->count++] = source->heap.entries[0].value;
        ++source->position[i]; ++source->emitted; source->root_pending = true;
    }
    output->finished = source->emitted == source->target;
    if (output->finished) output->metrics = s->metrics;
    average_chunk(w->result, output);
    return 0;
}
static void *merge_partition(void *opaque)
{
    struct merge_worker *w = opaque;
    struct source *source = &w->state->sources[w->part];
    struct sort_timing start = sort_clock(), io_before = source->read.time,
        avg_before = w->result->aggregate;
    while (!w->result->finished) if (merge_chunk(w)) { w->error = 1; break; }
    struct sort_timing end = sort_clock();
    source->time.wall_ns += end.wall_ns - start.wall_ns - (source->read.time.wall_ns - io_before.wall_ns)
        - (w->result->aggregate.wall_ns - avg_before.wall_ns);
    source->time.cpu_ns += end.cpu_ns - start.cpu_ns - (source->read.time.cpu_ns - io_before.cpu_ns)
        - (w->result->aggregate.cpu_ns - avg_before.cpu_ns);
    return NULL;
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
    w->context = cs_work_begin_limit(merge_path_work_bytes() / SORT_WORKERS);
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
    struct sorted_chunk *outputs[MERGE_PATH_WORKERS] = {0};
    struct average_result results[MERGE_PATH_WORKERS] = {0}, *result = &results[0];
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
    for (unsigned part = 0; part < MERGE_PATH_WORKERS; ++part) {
        outputs[part] = malloc(sizeof(*outputs[part]));
        if (!outputs[part]) goto cleanup;
    }
    if (cs_work_init(merge_path_work_bytes())) goto cleanup;
    printf("CONFIG CPU sink_workers=%u merge_workers=2 work_bytes=%zu buffer_bytes=%zu\n",
           SORT_WORKERS, merge_path_work_bytes(), merge_path_buffer_bytes());
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
    struct merge_worker merging[MERGE_PATH_WORKERS];
    for (unsigned part = 0; part < MERGE_PATH_WORKERS; ++part)
        merging[part] = (struct merge_worker){.state = s, .part = part, .file = temporary,
            .io = workers[part].io, .output = outputs[part], .result = &results[part]};
    struct sort_timing prepare_start = sort_clock();
    if (prepare(s, temporary, io)) goto io_error;
    uint64_t prepare_ns = sort_clock().wall_ns - prepare_start.wall_ns;
    struct sort_timing partition_start = sort_clock();
    if (partition(s, temporary, io)) goto io_error;
    uint64_t partition_ns = sort_clock().wall_ns - partition_start.wall_ns;
    uint64_t partition_bytes = s->sources[0].read.calls * SORT_BLOCK_BYTES;
    // Match device priming: part 0 prepares and emits one chunk before both run.
    if (merge_chunk(&merging[0])) goto io_error;
    struct sort_timing prime_done = sort_clock();
    s->sources[0].time.wall_ns = prime_done.wall_ns - prepare_start.wall_ns -
        s->sources[0].read.time.wall_ns - (s->spill_write.time.wall_ns - spill_write.time.wall_ns) - results[0].aggregate.wall_ns;
    s->sources[0].time.cpu_ns = prime_done.cpu_ns - prepare_start.cpu_ns -
        s->sources[0].read.time.cpu_ns - (s->spill_write.time.cpu_ns - spill_write.time.cpu_ns) - results[0].aggregate.cpu_ns;
    started = 0;
    for (; started < MERGE_PATH_WORKERS; ++started)
        if (pthread_create(&threads[started], NULL, merge_partition, &merging[started])) break;
    for (unsigned part = 0; part < started; ++part) pthread_join(threads[part], NULL);
    if (started != MERGE_PATH_WORKERS || merging[0].error || merging[1].error) goto io_error;
    for (unsigned part = 0; part < MERGE_PATH_WORKERS; ++part) {
        add_file_timing(&spill_read, &s->sources[part].read);
        s->metrics.source.wall_ns += s->sources[part].time.wall_ns;
        s->metrics.source.cpu_ns += s->sources[part].time.cpu_ns;
    }
    s->metrics.spill_read_bytes = spill_read.calls * SORT_BLOCK_BYTES;
    result->sum += results[1].sum; result->count += results[1].count;
    result->average = (double)result->sum / result->count;
    result->finished &= results[1].finished;
    result->aggregate.wall_ns += results[1].aggregate.wall_ns;
    result->aggregate.cpu_ns += results[1].aggregate.cpu_ns;
    result->metrics = s->metrics;
    result->metrics.runs = s->count;
    result->metrics.peak_bytes = merge_path_buffer_bytes();
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
    printf("PROFILE CPU prepare wall_s=%.9f\n", prepare_ns / 1e9);
    printf("PROFILE CPU partition wall_s=%.9f read_bytes=%llu\n", partition_ns / 1e9,
           (unsigned long long)partition_bytes);
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
    for (unsigned part = 0; part < MERGE_PATH_WORKERS; ++part) free(outputs[part]);
    if (temporary >= 0) close(temporary);
    if (file >= 0) close(file);
    return rc;
}
