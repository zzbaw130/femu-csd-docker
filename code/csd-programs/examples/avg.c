/* MR[0]: sorted chunk; MR[1]: persistent sum/count/average, initially zeroed. */
#include "femu-csd-kernel.h"
#include "batch_sort.h"
int64_t avg_int32(struct femu_csd_args *args)
{
    if (args->numr != 2 || args->mr_len[0] < (long long)sizeof(struct sorted_chunk) ||
        args->mr_len[1] < (long long)sizeof(struct average_result)) return -1;
    const struct sorted_chunk *input = args->mr_addr[0];
    struct average_result *output = args->mr_addr[1];
    if (input->count > SORT_OUTPUT_VALUES || output->finished) return -1;
    average_chunk(output, input);
    return output->count;
}
