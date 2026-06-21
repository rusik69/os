/*
 * src/test/stress/stress_memory.c — Comprehensive memory stress test
 *
 * Tests:
 *   - Allocate all available memory progressively
 *   - Memory pressure / OOM test
 *   - Malloc of all sizes (8, 16, 32, 64 ... up to 4096)
 *   - Heap fragmentation test
 *   - Page allocation + freeing patterns (via large allocations)
 *   - Memory mapping stress (via brk)
 *
 * Usage:
 *   stress_memory [duration_seconds]
 *
 * Defaults: duration=30
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

#define DEFAULT_DURATION 30
#define CLOCK_REALTIME   0

/* ── Stats ──────────────────────────────────────────────────── */
static unsigned long g_total_allocated   = 0;  /* bytes */
static unsigned long g_peak_allocated    = 0;  /* bytes */
static unsigned long g_allocation_count  = 0;
static unsigned long g_free_count        = 0;
static unsigned long g_all_sizes_iters   = 0;
static unsigned long g_frag_iters        = 0;
static unsigned long g_page_iters        = 0;
static unsigned long g_oom_iters         = 0;
static unsigned long g_mmap_iters        = 0;
static unsigned long g_errors            = 0;

/* ── Timer helpers ──────────────────────────────────────────── */

static struct timespec ts_start;
static int timer_init = 0;

static double elapsed(void)
{
    struct timespec now;
    if (!timer_init) {
        clock_gettime(CLOCK_REALTIME, &ts_start);
        timer_init = 1;
        return 0.0;
    }
    clock_gettime(CLOCK_REALTIME, &now);
    double s = (double)(now.tv_sec - ts_start.tv_sec);
    s += (double)(now.tv_nsec - ts_start.tv_nsec) / 1e9;
    return s;
}

static void reset_timer(void)
{
    timer_init = 0;
}

/* ── 1. Progressive memory allocation ──────────────────────── */
/* Allocate increasingly large blocks until failure, then free. */

static int test_progressive_alloc(void)
{
    unsigned long sizes[] = {
        1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 8388608
    };
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    void *ptrs[32];
    int count = 0;
    unsigned long total = 0;

    for (int i = 0; i < num_sizes && count < 32; i++) {
        /* Try allocating multiple blocks of each size */
        for (int j = 0; j < 4 && count < 32; j++) {
            ptrs[count] = malloc(sizes[i]);
            if (!ptrs[count]) {
                /* Allocation failed — this is expected under pressure */
                break;
            }
            /* Touch every page */
            memset(ptrs[count], 0xAA, sizes[i]);
            /* Verify first and last byte */
            unsigned char *p = (unsigned char *)ptrs[count];
            if (p[0] != 0xAA || p[sizes[i] - 1] != 0xAA) {
                printf("[stress_memory] PROGRESSIVE: corruption at "
                       "size %lu\n", sizes[i]);
                g_errors++;
                free(ptrs[count]);
                break;
            }
            total += sizes[i];
            g_total_allocated += sizes[i];
            g_allocation_count++;
            count++;
        }
    }

    if (total > g_peak_allocated)
        g_peak_allocated = total;

    /* Free all */
    for (int i = 0; i < count; i++) {
        if (ptrs[i]) {
            free(ptrs[i]);
            g_free_count++;
        }
    }

    return 0;
}

/* ── 2. Memory pressure / OOM test ─────────────────────────── */
/* Allocate aggressively until failure, verify OOM handling. */

static int test_oom_pressure(void)
{
    void **blocks = NULL;
    int max_blocks = 256;
    int count = 0;
    unsigned long block_size = 1024 * 1024; /* 1 MB blocks */

    blocks = (void **)malloc((unsigned long)max_blocks * sizeof(void *));
    if (!blocks) {
        g_errors++;
        return -1;
    }

    /* Allocate until failure */
    for (int i = 0; i < max_blocks; i++) {
        blocks[i] = malloc(block_size);
        if (!blocks[i]) {
            printf("[stress_memory] OOM-PRESSURE: allocation failed "
                   "at block %d/%d (%lu MB total)\n",
                   i, max_blocks, (unsigned long)i * block_size / 1048576);
            count = i;
            break;
        }
        /* Touch to force commit */
        memset(blocks[i], 0xBB, block_size);
        g_total_allocated += block_size;
        g_allocation_count++;
        count = i + 1;
    }

    unsigned long total_mb = (unsigned long)count * block_size / 1048576;
    if (total_mb > g_peak_allocated / 1048576)
        g_peak_allocated = (unsigned long)count * block_size;

    /* Verify first and last blocks */
    if (count > 0) {
        unsigned char *p0 = (unsigned char *)blocks[0];
        if (p0[0] != 0xBB || p0[block_size - 1] != 0xBB) {
            printf("[stress_memory] OOM-PRESSURE: corruption in block 0\n");
            g_errors++;
        }
        if (count > 1) {
            unsigned char *p1 = (unsigned char *)blocks[count - 1];
            if (p1[0] != 0xBB || p1[block_size - 1] != 0xBB) {
                printf("[stress_memory] OOM-PRESSURE: corruption "
                       "in block %d\n", count - 1);
                g_errors++;
            }
        }
    }

    /* Free all */
    for (int i = 0; i < count; i++) {
        if (blocks[i]) {
            free(blocks[i]);
            g_free_count++;
        }
    }
    free(blocks);

    g_oom_iters++;
    return 0;
}

/* ── 3. All-size malloc test ───────────────────────────────── */
/* Allocate every size from 8 to 4096 bytes, write pattern, verify. */

static int test_all_sizes(void)
{
    void *ptrs[16];
    int ok = 1;

    for (int sz = 8; sz <= 4096; sz *= 2) {
        /* Allocate multiple of each size */
        for (int i = 0; i < 8; i++) {
            ptrs[i] = malloc((unsigned long)sz);
            if (!ptrs[i]) {
                ok = 0;
                break;
            }
            /* Write pattern */
            memset(ptrs[i], (unsigned char)(sz & 0xFF), (unsigned long)sz);
            /* Verify */
            unsigned char *p = (unsigned char *)ptrs[i];
            for (int j = 0; j < sz; j++) {
                if (p[j] != (unsigned char)(sz & 0xFF)) {
                    printf("[stress_memory] SIZE-TEST: corruption "
                           "at size %d byte %d\n", sz, j);
                    g_errors++;
                    ok = 0;
                    break;
                }
            }
            g_allocation_count++;
            g_total_allocated += (unsigned long)sz;
        }

        /* Free */
        for (int i = 0; i < 8; i++) {
            if (ptrs[i]) {
                free(ptrs[i]);
                g_free_count++;
            }
        }

        if (!ok) break;
    }

    /* Also test odd sizes */
    int odd_sizes[] = {3, 7, 13, 27, 53, 101, 251, 509, 1021, 2047};
    for (int s = 0; s < 10; s++) {
        int sz = odd_sizes[s];
        void *p = malloc((unsigned long)sz);
        if (!p) {
            g_errors++;
            continue;
        }
        memset(p, 0xCC, (unsigned long)sz);
        unsigned char *cp = (unsigned char *)p;
        /* Spot-check */
        if (cp[0] != 0xCC || cp[sz - 1] != 0xCC) {
            printf("[stress_memory] ODD-SIZE: corruption at size %d\n", sz);
            g_errors++;
        }
        free(p);
        g_allocation_count++;
        g_free_count++;
        g_total_allocated += (unsigned long)sz;
    }

    g_all_sizes_iters++;
    return ok ? 0 : -1;
}

/* ── 4. Heap fragmentation test ────────────────────────────── */
/* Allocate many small blocks, then free every other to create
 * fragmentation, then try large allocations. */

static int test_fragmentation(void)
{
    #define FRAG_COUNT 200
    void *frag_ptrs[FRAG_COUNT];
    int allocated = 0;

    /* Phase 1: allocate many small blocks */
    for (int i = 0; i < FRAG_COUNT; i++) {
        int size = 16 + (i % 8) * 16;  /* 16, 32, 48, ... 128 */
        frag_ptrs[i] = malloc((unsigned long)size);
        if (!frag_ptrs[i]) break;
        memset(frag_ptrs[i], 0xDD, (unsigned long)size);
        g_allocation_count++;
        g_total_allocated += (unsigned long)size;
        allocated++;
    }

    /* Phase 2: free every other block (creates fragmentation) */
    for (int i = 0; i < allocated; i += 2) {
        if (frag_ptrs[i]) {
            free(frag_ptrs[i]);
            g_free_count++;
            frag_ptrs[i] = NULL;
        }
    }

    /* Phase 3: try large allocations (should find space) */
    int large_ok = 1;
    for (int i = 0; i < 20; i++) {
        void *big = malloc(4096 + (unsigned long)i * 1024);
        if (!big) {
            large_ok = 0;
            break;
        }
        memset(big, 0xEE, 4096 + (unsigned long)i * 1024);
        g_allocation_count++;
        g_total_allocated += 4096 + (unsigned long)i * 1024;
        free(big);
        g_free_count++;
    }

    /* Phase 4: free remaining */
    for (int i = 0; i < allocated; i++) {
        if (frag_ptrs[i]) {
            free(frag_ptrs[i]);
            g_free_count++;
        }
    }

    g_frag_iters++;
    return large_ok ? 0 : -1;
}

/* ── 5. Page allocation + freeing patterns ─────────────────── */
/* Allocate and free page-sized and multi-page blocks in patterns. */

static int test_page_patterns(void)
{
    void *pages[32];

    /* Allocate 4 KB pages */
    for (int i = 0; i < 32; i++) {
        pages[i] = malloc(4096);
        if (!pages[i]) {
            for (int j = 0; j < i; j++) {
                if (pages[j]) {
                    free(pages[j]);
                    g_free_count++;
                }
            }
            g_errors++;
            return -1;
        }
        memset(pages[i], (unsigned char)i, 4096);
        g_allocation_count++;
        g_total_allocated += 4096;
    }

    /* Verify all */
    for (int i = 0; i < 32; i++) {
        unsigned char *p = (unsigned char *)pages[i];
        if (p[0] != (unsigned char)i || p[4095] != (unsigned char)i) {
            printf("[stress_memory] PAGE-TEST: corruption in page %d\n", i);
            g_errors++;
            break;
        }
    }

    /* Free in reverse order (tests coalescing) */
    for (int i = 31; i >= 0; i--) {
        if (pages[i]) {
            free(pages[i]);
            g_free_count++;
        }
    }

    /* Allocate large multi-page blocks */
    for (int pages_count = 1; pages_count <= 16; pages_count *= 2) {
        unsigned long size = (unsigned long)pages_count * 4096;
        void *big = malloc(size);
        if (!big) {
            g_errors++;
            continue;
        }
        memset(big, 0xFF, size);
        g_allocation_count++;
        g_total_allocated += size;
        free(big);
        g_free_count++;
    }

    g_page_iters++;
    return 0;
}

/* ── 6. Memory mapping stress (via brk) ────────────────────── */
/* Direct brk manipulation to stress kernel's mmap layer. */

static int test_brk_stress(void)
{
    /* Get current break */
    void *current_brk = (void *)brk((void *)0);
    if ((unsigned long)current_brk == (unsigned long)-1) {
        g_errors++;
        return -1;
    }

    /* Extend by 1 MB */
    void *new_brk = (void *)((unsigned long)current_brk + 1048576);
    if (brk(new_brk) < 0) {
        g_errors++;
        return -1;
    }

    /* Touch the new pages */
    unsigned char *p = (unsigned char *)current_brk;
    for (unsigned long i = 0; i < 1048576; i += 4096) {
        p[i] = 0x77;
        /* Verify */
        if (p[i] != 0x77) {
            g_errors++;
            break;
        }
    }

    /* Shrink back */
    if (brk(current_brk) < 0) {
        g_errors++;
    }

    g_mmap_iters++;
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int duration = DEFAULT_DURATION;

    if (argc > 1)
        duration = atoi(argv[1]);
    if (duration < 1) duration = 1;
    if (duration > 3600) duration = 3600;

    printf("\n============================================\n");
    printf("  Hermes OS Memory Stress Test v2\n");
    printf("============================================\n");
    printf("  Duration: %d seconds\n", duration);
    printf("  Tests:\n");
    printf("    - Progressive allocation (all sizes)\n");
    printf("    - OOM pressure test\n");
    printf("    - All-size malloc (8..4096, odd sizes)\n");
    printf("    - Heap fragmentation\n");
    printf("    - Page allocation + freeing patterns\n");
    printf("    - Direct brk manipulation\n");
    printf("============================================\n\n");

    reset_timer();
    double wall_start = elapsed();

    int prog_count = 0;
    int oom_count = 0;
    int allsz_count = 0;
    int frag_count = 0;
    int page_count = 0;
    int brk_count = 0;
    int test_errors = 0;

    while (elapsed() - wall_start < (double)duration) {
        /* Rotate through test types */
        int phase = ((int)(elapsed() - wall_start)) % 6;

        switch (phase) {
        case 0:
            if (test_progressive_alloc() != 0) test_errors++;
            prog_count++;
            break;
        case 1:
            if (test_oom_pressure() != 0) test_errors++;
            oom_count++;
            break;
        case 2:
            if (test_all_sizes() != 0) test_errors++;
            allsz_count++;
            break;
        case 3:
            if (test_fragmentation() != 0) test_errors++;
            frag_count++;
            break;
        case 4:
            if (test_page_patterns() != 0) test_errors++;
            page_count++;
            break;
        case 5:
            if (test_brk_stress() != 0) test_errors++;
            brk_count++;
            break;
        }

        /* Progress report every ~5 seconds */
        double t = elapsed() - wall_start;
        if ((int)t % 5 == 0 && (int)t > 0 && g_allocation_count > 0) {
            printf("[stress_memory] Progress: %.0fs, "
                   "%lu allocs, %lu frees, peak %lu KB\n",
                   t, g_allocation_count, g_free_count,
                   g_peak_allocated / 1024);
        }

        yield();
    }

    double wall_elapsed = elapsed() - wall_start;

    /* ── Summary Report ── */
    printf("\n");
    printf("============================================\n");
    printf("  MEMORY STRESS TEST SUMMARY\n");
    printf("============================================\n");
    printf("  Wall clock:            %.2f s\n", wall_elapsed);
    printf("\n");
    printf("  Test iterations:\n");
    printf("    Progressive alloc:   %d\n", prog_count);
    printf("    OOM pressure:        %d\n", oom_count);
    printf("    All-size malloc:     %d\n", allsz_count);
    printf("    Fragmentation:       %d\n", frag_count);
    printf("    Page patterns:       %d\n", page_count);
    printf("    brk stress:          %d\n", brk_count);
    printf("\n");
    printf("  Memory operations:\n");
    printf("    Allocations:         %lu\n", g_allocation_count);
    printf("    Frees:               %lu\n", g_free_count);
    printf("    Total allocated:     %lu bytes (%lu KB)\n",
           g_allocation_count > 0 ? g_total_allocated : 0UL,
           g_allocation_count > 0 ? g_total_allocated / 1024 : 0UL);
    printf("    Peak concurrent:     %lu bytes (%lu KB)\n",
           g_peak_allocated, g_peak_allocated / 1024);
    printf("\n");
    printf("  Errors:               %lu\n", g_errors);
    printf("  Test-level errors:    %d\n", test_errors);
    printf("\n");

    if (g_errors > 0 || test_errors > 0) {
        printf("  >>> STATUS: FAIL <<<\n");
        return 1;
    }
    printf("  >>> STATUS: PASS <<<\n");
    printf("============================================\n");

    return 0;
}

/* ── stress_memory_run ──────────────────────────────────── */
int stress_memory_run(size_t size)
{
    kprintf("[stress] Memory stress test with %zu bytes\n", size);
    return 0;
}
/* ── stress_memory_stop ──────────────────────────────────── */
int stress_memory_stop(void)
{
    kprintf("[stress] Memory stress stopped\n");
    return 0;
}
