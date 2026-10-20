/* Two output partitions of a k-way merge; shared by C baseline and device CSF. */
#ifndef MERGE_PATH_SORT_H
#define MERGE_PATH_SORT_H
#include "batch_sort.h"
#define MERGE_PATH_WORKERS 2
struct merge_path_task {
    uint32_t part, reserved;
    uint64_t prepare_ns, partition_ns, partition_read_bytes;
};
static inline size_t merge_path_buffer_bytes(void)
{
    return sort_buffer_bytes(SORT_WORKERS) +
        ((sizeof(struct sorted_chunk) + 4095U) & ~(size_t)4095U) + 3U * 4096U;
}
static inline size_t merge_path_work_bytes(void)
{
    return (SORT_MEMORY_BYTES - merge_path_buffer_bytes()) & ~(size_t)4095U;
}
/* Descending k-way Merge Path. cut[] is the intersection at an exact output
 * rank. In each step choose the greatest candidate delta items ahead and
 * consume that run's safe prefix. Equal values prefer the lower run index.
 * Call probe again after I/O, and consider only once its value is available. */
struct merge_path_partition {
    unsigned cut[SORT_MAX_RUNS], count, scan, delta, best;
    uint64_t remaining;
    int32_t best_value;
};
static inline int merge_path_probe(struct merge_path_partition *p, unsigned run_values,
                                  unsigned *run, unsigned *position)
{
    while (p->remaining) {
        if (!p->delta) {
            unsigned active = 0;
            for (unsigned i = 0; i < p->count; ++i) active += p->cut[i] < run_values;
            p->delta = (unsigned)((p->remaining + active - 1) / active);
            p->scan = 0; p->best = p->count;
        }
        while (p->scan < p->count && p->cut[p->scan] == run_values) ++p->scan;
        if (p->scan < p->count) {
            *run = p->scan;
            unsigned ahead = p->cut[p->scan] + p->delta;
            *position = (ahead < run_values ? ahead : run_values) - 1;
            return 1;
        }
        unsigned available = run_values - p->cut[p->best];
        unsigned take = p->delta < available ? p->delta : available;
        p->cut[p->best] += take;
        p->remaining -= take;
        p->delta = 0;
    }
    return 0;
}
static inline void merge_path_consider(struct merge_path_partition *p, int32_t value)
{
    unsigned i = p->scan++;
    if (p->best == p->count || value > p->best_value ||
        (value == p->best_value && i < p->best)) {
        p->best = i; p->best_value = value;
    }
}
#endif
