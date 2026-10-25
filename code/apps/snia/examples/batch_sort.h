/* Example-specific layout and run sort shared by Guest and device programs. */
#ifndef BATCH_SORT_H
#define BATCH_SORT_H
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#define SORT_FILE_BYTES (256U << 20)
#define SORT_CHUNK_BYTES (4U << 20)
#define SORT_BLOCK_BYTES (256U << 10)
#define SORT_MAX_RUNS (SORT_FILE_BYTES / SORT_CHUNK_BYTES)
#define SORT_WORKERS 4
#define SORT_BLOCKS_PER_RUN (SORT_CHUNK_BYTES / SORT_BLOCK_BYTES)
#define SORT_OUTPUT_VALUES (SORT_CHUNK_BYTES / sizeof(int32_t))
struct sort_timing { uint64_t wall_ns, cpu_ns; };
static inline struct sort_timing sort_clock(void)
{
    struct timespec wall, cpu;
    clock_gettime(CLOCK_MONOTONIC, &wall);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    struct sort_timing stamp = {(uint64_t)wall.tv_sec * 1000000000 + wall.tv_nsec,
                               (uint64_t)cpu.tv_sec * 1000000000 + cpu.tv_nsec};
    return stamp;
}
static inline void sort_record(struct sort_timing *total, struct sort_timing start)
{
    struct sort_timing end = sort_clock();
    total->wall_ns += end.wall_ns - start.wall_ns;
    total->cpu_ns += end.cpu_ns - start.cpu_ns;
}
struct sort_metrics {
    uint64_t spill_write_bytes, spill_read_bytes, peak_bytes;
    uint32_t runs, spilled_runs;
    struct sort_timing sink, source;
    uint64_t run_sort_ns;
};
struct sorted_chunk {
    uint32_t count, finished;
    struct sort_metrics metrics;
    int32_t values[SORT_OUTPUT_VALUES];
};
struct average_result {
    int64_t sum;
    uint64_t count;
    double average;
    uint32_t finished;
    struct sort_timing aggregate;
    struct sort_metrics metrics;
};
#define SORT_MEMORY_BYTES (64U << 20)
/* Count the four I/O buffers in the same 64 MiB budget on both paths. */
#define SORT_BUFFER_BYTES (SORT_CHUNK_BYTES + SORT_BLOCK_BYTES + sizeof(struct sorted_chunk) + sizeof(struct average_result))
#define SORT_WORK_BYTES ((SORT_MEMORY_BYTES - SORT_BUFFER_BYTES) & ~(size_t)4095)
/* Small AFDM outputs carry opaque device handles, never algorithm workspaces.
 * In the three-MR ABI the spill I/O AFDM is still the last MR. */
struct sort_sink_task { uint64_t state; uint32_t workers, reserved; };
struct sort_run_group { uint64_t states[SORT_WORKERS]; uint32_t count, reserved; };
static inline size_t sort_buffer_bytes(unsigned workers)
{
    return workers == 1 ? SORT_BUFFER_BYTES :
        workers * (SORT_CHUNK_BYTES + SORT_BLOCK_BYTES + 4096U) +
        ((sizeof(struct sorted_chunk) + 4095U) & ~(size_t)4095U) + 8192U;
}
static inline size_t sort_work_bytes(unsigned workers)
{
    return (SORT_MEMORY_BYTES - sort_buffer_bytes(workers)) & ~(size_t)4095U;
}

/* Retain exact integer sum/count across chunks; never average batch averages.
 * 256 MiB of int32 values has a sum bounded by 2^57, so int64 is sufficient. */
static inline void average_chunk(struct average_result *result, const struct sorted_chunk *input)
{
    struct sort_timing start = sort_clock();
    int64_t sum = 0;
    for (uint32_t i = 0; i < input->count; ++i) sum += input->values[i];
    result->sum += sum;
    result->count += input->count;
    result->average = result->count ? (double)result->sum / (double)result->count : 0;
    result->finished = input->finished;
    if (input->finished) result->metrics = input->metrics;
    sort_record(&result->aggregate, start);
}

/* Cached heads keep heap comparisons independent of spill-buffer residency. */
struct sort_heap_entry { int32_t value; unsigned run; };
struct sort_heap { struct sort_heap_entry entries[SORT_MAX_RUNS]; unsigned count; };
static inline void sort_heap_push(struct sort_heap *heap, struct sort_heap_entry entry)
{
    unsigned i = heap->count++;
    while (i && heap->entries[(i - 1) / 2].value < entry.value) {
        heap->entries[i] = heap->entries[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    heap->entries[i] = entry;
}
static inline void sort_heap_replace(struct sort_heap *heap, struct sort_heap_entry entry)
{
    unsigned i = 0;
    while (2 * i + 1 < heap->count) {
        unsigned child = 2 * i + 1;
        if (child + 1 < heap->count && heap->entries[child + 1].value > heap->entries[child].value) ++child;
        if (heap->entries[child].value <= entry.value) break;
        heap->entries[i] = heap->entries[child];
        i = child;
    }
    heap->entries[i] = entry;
}
static inline void sort_heap_pop(struct sort_heap *heap)
{
    struct sort_heap_entry last = heap->entries[--heap->count];
    if (heap->count) sort_heap_replace(heap, last);
}

/* Same in-place descending quicksort for the C baseline and the device CSF.
 * Recurse only on the smaller partition: stack depth is bounded by log2(n).
 * No libc qsort scratch allocation outside the workmem budget. */
static inline void sort_int32_desc(int32_t *data, size_t count)
{
    while (count > 16) {
        int32_t a = data[0], b = data[count / 2], c = data[count - 1];
        int32_t pivot = a < b ? (b < c ? b : (a < c ? c : a))
                              : (a < c ? a : (b < c ? c : b));
        ptrdiff_t left = 0, right = (ptrdiff_t)count - 1;
        while (left <= right) {
            while (data[left] > pivot) ++left;
            while (data[right] < pivot) --right;
            if (left <= right) {
                int32_t value = data[left]; data[left++] = data[right]; data[right--] = value;
            }
        }
        size_t first = (size_t)(right + 1), last = count - (size_t)left;
        if (first < last) {
            sort_int32_desc(data, first);
            data += left; count = last;
        } else {
            sort_int32_desc(data + left, last);
            count = first;
        }
    }
    for (size_t i = 1; i < count; ++i) {
        int32_t value = data[i];
        size_t j = i;
        while (j && data[j - 1] < value) { data[j] = data[j - 1]; --j; }
        data[j] = value;
    }
}
#endif
