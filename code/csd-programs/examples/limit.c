/* LIMIT 5 keeps the first five values across successive globally sorted chunks. */
#include "femu-csd-kernel.h"
#include "batch_sort.h"
#include <string.h>
int64_t limit_five(struct femu_csd_args *args)
{
    if (args->numr != 2 || args->mr_len[0] < (long long)sizeof(struct sorted_chunk) ||
        args->mr_len[1] < (long long)sizeof(struct top_five)) return -1;
    const struct sorted_chunk *input = args->mr_addr[0];
    struct top_five *output = args->mr_addr[1];
    if (input->count > SORT_OUTPUT_VALUES || output->count > 5) return -1;
    unsigned count = 5 - output->count;
    if (count > input->count) count = input->count;
    memcpy(output->values + output->count, input->values, count * sizeof(int32_t));
    output->count += count;
    output->finished = input->finished;
    return output->count;
}
