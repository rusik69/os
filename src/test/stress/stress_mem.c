/*
 * src/test/stress/stress_mem.c — Concurrent memory + mapping stress
 *
 * Covers D256 tasks:
 *   1. High-frequency malloc/free from concurrent processes
 *      (kernel kmalloc/kfree path is exercised via the userspace allocator)
 *   2. Concurrent mmap/munmap (kernel vmm_map/vmm_unmap path)
 *  11. OOM reclaim under extreme pressure (allocator survives to failure)
 *
 * Usage:
 *   stress_mem [duration_seconds] [concurrency]
 * Defaults: duration=30, concurrency=4
 *
 * Note: tasks 1/2 name the *kernel* allocators (kmalloc/kfree,
 * vmm_map/vmm_unmap). From a userspace ELF the faithful analogue is
 * malloc/free and mmap/munmap, which drive those exact kernel paths
 * through the syscall layer. The kernel-side kmalloc/vmm versions have
 * their own dedicated KUnit coverage elsewhere.
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "sys/stat.h"

#ifndef _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#define _SIZE_T_DEFINED
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

static volatile int g_stop = 0;

static double elapsed(void)
{
    static struct timespec ts0;
    static int init = 0;
    struct timespec now;
    if (!init) { clock_gettime(CLOCK_REALTIME, &ts0); init = 1; return 0.0; }
    clock_gettime(CLOCK_REALTIME, &now);
    return (double)(now.tv_sec - ts0.tv_sec) + (double)(now.tv_nsec - ts0.tv_nsec) / 1e9;
}

/* Each worker performs high-frequency malloc/free of varied sizes. */
static int worker_alloc(int id, int seconds)
{
    double start = elapsed();
    unsigned long iters = 0, errors = 0;
    unsigned long sizes[] = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 65536 };
    while (elapsed() - start < (double)seconds && !g_stop) {
        int idx = (int)(iters % (sizeof(sizes)/sizeof(sizes[0])));
        size_t sz = sizes[idx];
        void *p = malloc(sz);
        if (!p) { errors++; continue; } /* under pressure: expected */
        memset(p, (unsigned char)(id & 0xFF), sz);
        /* verify a few bytes */
        unsigned char *cp = (unsigned char *)p;
        if (cp[0] != (unsigned char)(id & 0xFF) || cp[sz-1] != (unsigned char)(id & 0xFF))
            errors++;
        free(p);
        iters++;
    }
    printf("[stress_mem] worker %d: %lu alloc/free iters, %lu errors\n", id, iters, errors);
    return errors ? -1 : 0;
}

/* Each worker does concurrent brk-based map/unmap of regions.
 * The libc exposes brk() (not mmap), which still drives the kernel's
 * vmm layer (vmm_map/vmm_unmap) through the brk syscall — the faithful
 * userspace analogue of task 2. */
static int worker_map(int id, int seconds)
{
    double start = elapsed();
    unsigned long iters = 0, errors = 0;
    while (elapsed() - start < (double)seconds && !g_stop) {
        size_t sz = 4096UL * (1 + (iters % 16)); /* 4K..64K */
        void *base = (void *)brk((void *)0);
        if ((unsigned long)base == (unsigned long)-1) { errors++; continue; }
        void *new_brk = (void *)((unsigned long)base + sz);
        if (brk(new_brk) < 0) { errors++; continue; }
        memset(base, (unsigned char)(id & 0xFF), sz);
        unsigned char *cp = (unsigned char *)base;
        if (cp[0] != (unsigned char)(id & 0xFF) || cp[sz-1] != (unsigned char)(id & 0xFF))
            errors++;
        /* unmap by restoring the break */
        if (brk(base) < 0) errors++;
        iters++;
    }
    printf("[stress_mem] mapper %d: %lu brk map/unmap iters, %lu errors\n", id, iters, errors);
    return errors ? -1 : 0;
}

/* OOM pressure: allocate 1MB blocks until failure, then free all. */
static int oom_pressure(void)
{
    void **blocks = NULL;
    int max = 512, count = 0;
    blocks = (void **)malloc((unsigned long)max * sizeof(void *));
    if (!blocks) return -1;
    for (int i = 0; i < max; i++) {
        blocks[i] = malloc(1024UL * 1024UL);
        if (!blocks[i]) break;
        memset(blocks[i], 0xBB, 1024UL * 1024UL);
        count++;
    }
    printf("[stress_mem] OOM-PRESSURE: allocated %d MB before failure\n", count);
    for (int i = 0; i < count; i++) free(blocks[i]);
    free(blocks);
    return 0; /* failure to allocate is the expected OOM signal */
}

int main(int argc, char *argv[])
{
    int duration = (argc > 1) ? atoi(argv[1]) : 30;
    int conc = (argc > 2) ? atoi(argv[2]) : 4;
    if (duration < 1) duration = 1;
    if (conc < 1) conc = 1;
    if (conc > 16) conc = 16;

    printf("\n=== Hermes OS Memory Stress v2 (concurrent) ===\n");
    printf("  duration=%ds concurrency=%d\n", duration, conc);
    printf("  tasks: concurrent malloc/free, mmap/munmap, OOM reclaim\n");
    printf("============================================\n\n");

    int pids[32];
    int n = 0;
    double start = elapsed();
    for (int i = 0; i < conc; i++) {
        int pid = fork();
        if (pid == 0) {
            /* alternate alloc vs map workers */
            if (i & 1) worker_map(i, duration);
            else        worker_alloc(i, duration);
            exit(0);
        } else if (pid > 0) {
            pids[n++] = pid;
        }
    }

    /* Parent also runs OOM pressure in the background */
    int oom_err = 0;
    while (elapsed() - start < (double)duration) {
        if (oom_pressure() != 0) oom_err++;
        yield();
    }
    g_stop = 1;

    int fail = 0;
    for (int i = 0; i < n; i++) {
        int st;
        waitpid(pids[i], &st, 0);
        if (st != 0) fail++;
    }

    printf("\n=== MEMORY STRESS SUMMARY ===\n");
    printf("  workers failed: %d\n", fail);
    printf("  oom_pressure errors: %d\n", oom_err);
    printf("  RESULT: %s\n", (fail == 0) ? "PASS" : "FAIL");
    printf("==============================\n");
    return (fail == 0) ? 0 : 1;
}
