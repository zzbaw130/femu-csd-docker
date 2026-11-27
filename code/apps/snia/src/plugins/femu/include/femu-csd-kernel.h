#ifndef FEMU_CSD_KERNEL_H
#define FEMU_CSD_KERNEL_H
#include <stdint.h>

struct femu_csd_args {
    int numr;
    void **mr_addr;
    long long *mr_len;
    long long cparam1;
    long long cparam2;
    /* Extra input bytes following inline MR descriptors, owned by this execution.
     * The current SNIA backend does not submit this payload: NULL / 0 there. */
    void *data_buffer;
    long long buffer_len;
    void *graph_context;
    uint64_t (*graph_event)(void *context, uintptr_t key, int signal);
    unsigned graph_stream, graph_input;
} __attribute__((packed));

#endif
