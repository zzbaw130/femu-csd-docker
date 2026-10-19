/* Device CSF: LIMIT 5 consumes a globally sorted chunk, not unsorted local maxima. */
#include "femu-csd-kernel.h"
#include "batch_sort.h"
#include <string.h>
int64_t limit_five(struct femu_csd_args *args)
{
    if (args->numr != 2 || args->mr_len[0] < (long long)sizeof(struct sorted_chunk) ||
        args->mr_len[1] < (long long)sizeof(struct top_five)) return -1;
    const struct sorted_chunk *input = args->mr_addr[0];
    struct top_five *output = args->mr_addr[1];
    if (input->count > SORT_OUTPUT_VALUES) return -1;
    output->count = input->count < 5 ? input->count : 5;
    memcpy(output->values, input->values, output->count * sizeof(int32_t));
    return output->count;
}
