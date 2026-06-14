/*
 * src/test/stress/stress_memory.c — Memory stress test for Hermes OS
 *
 * Allocates and frees large pages repeatedly to stress the PMM/VMM.
 * Tests both small and large allocations.
 *
 * Usage:
 *   stress_memory [num_iterations] [max_size_kb]
 *
 * Defaults: num_iterations=100, max_size_kb=4096
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

#define DEFAULT_ITERATIONS 100
#define DEFAULT_MAX_KB     4096

int main(int argc, char *argv[])
{
    int iterations = DEFAULT_ITERATIONS;
    int max_kb = DEFAULT_MAX_KB;

    if (argc > 1)
        iterations = atoi(argv[1]);
    if (argc > 2)
        max_kb = atoi(argv[2]);

    if (iterations < 1) iterations = 1;
    if (iterations > 10000) iterations = 10000;
    if (max_kb < 4) max_kb = 4;
    if (max_kb > 65536) max_kb = 65536;

    printf("[stress_memory] Running %d iterations, max size %d KB\n",
           iterations, max_kb);

    unsigned long total_allocated = 0;
    unsigned long peak_allocated = 0;

    for (int i = 0; i < iterations; i++) {
        /* Varying sizes: small, medium, large, very large */
        int sizes[] = {4, 16, 64, 256, 1024, max_kb};
        int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

        for (int s = 0; s < num_sizes; s++) {
            int kb = sizes[s];
            if (kb > max_kb) kb = max_kb;

            void *ptr = malloc((size_t)kb * 1024);
            if (!ptr) {
                printf("[stress_memory] FAIL: malloc(%d KB) returned NULL "
                       "at iteration %d\n", kb, i);
                return 1;
            }

            /* Touch every page to force physical allocation */
            memset(ptr, 0xAA, (size_t)kb * 1024);

            /* Verify the pattern */
            unsigned char *p = (unsigned char *)ptr;
            int ok = 1;
            for (int j = 0; j < kb * 1024; j += 4096) {
                if (p[j] != 0xAA) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) {
                printf("[stress_memory] FAIL: memory corruption detected "
                       "at iteration %d, size %d KB\n", i, kb);
                free(ptr);
                return 1;
            }

            total_allocated += (unsigned long)kb;
            if (total_allocated > peak_allocated)
                peak_allocated = total_allocated;

            free(ptr);
            total_allocated -= (unsigned long)kb;
        }

        if (i % 10 == 0 && i > 0) {
            printf("[stress_memory] Iteration %d/%d: peak %lu KB\n",
                   i, iterations, peak_allocated);
        }
    }

    printf("[stress_memory] PASS: %d iterations completed, "
           "peak %lu KB allocated\n", iterations, peak_allocated);
    return 0;
}
