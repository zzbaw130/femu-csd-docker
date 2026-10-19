/* Example-specific data layout shared by the Guest and device programs. */
#ifndef BATCH_SORT_H
#define BATCH_SORT_H
#include <stdint.h>
#define SORT_FILE_BYTES (256U << 20)
#define SORT_CHUNK_BYTES (4U << 20)
#define SORT_BLOCK_BYTES (256U << 10)
#define SORT_MAX_RUNS (SORT_FILE_BYTES / SORT_CHUNK_BYTES)
#define SORT_BLOCKS_PER_RUN (SORT_CHUNK_BYTES / SORT_BLOCK_BYTES)
#define SORT_OUTPUT_VALUES 1024
struct sorted_chunk { uint32_t count; int32_t values[SORT_OUTPUT_VALUES]; };
struct top_five { uint32_t count; int32_t values[5]; };
#endif
