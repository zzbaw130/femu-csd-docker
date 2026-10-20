/* Example-specific layout and run sort shared by Guest and device programs. */
#ifndef BATCH_SORT_H
#define BATCH_SORT_H
#include <stdint.h>
#include <stddef.h>
#define SORT_FILE_BYTES (256U << 20)
#define SORT_CHUNK_BYTES (4U << 20)
#define SORT_BLOCK_BYTES (256U << 10)
#define SORT_MAX_RUNS (SORT_FILE_BYTES / SORT_CHUNK_BYTES)
#define SORT_BLOCKS_PER_RUN (SORT_CHUNK_BYTES / SORT_BLOCK_BYTES)
#define SORT_OUTPUT_VALUES (SORT_CHUNK_BYTES / sizeof(int32_t))
struct sort_metrics {
    uint64_t spill_write_bytes, spill_read_bytes, peak_bytes;
    uint32_t runs, spilled_runs;
};
struct sorted_chunk {
    uint32_t count, finished;
    struct sort_metrics metrics;
    int32_t values[SORT_OUTPUT_VALUES];
};
struct top_five {
    uint32_t count, finished;
    struct sort_metrics metrics;
    int32_t values[5];
};
#define SORT_MEMORY_BYTES (64U << 20)
/* Count the four I/O buffers in the same 64 MiB budget on both paths. */
#define SORT_BUFFER_BYTES (SORT_CHUNK_BYTES + SORT_BLOCK_BYTES + sizeof(struct sorted_chunk) + sizeof(struct top_five))
#define SORT_WORK_BYTES ((SORT_MEMORY_BYTES - SORT_BUFFER_BYTES) & ~(size_t)4095)

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
