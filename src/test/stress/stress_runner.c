/*
 * src/test/stress/stress_runner.c — Stress test orchestrator for Hermes OS
 *
 * This program is spawned as PID 1 by the kernel when the "init" kernel
 * command-line parameter points to it.  It runs all three stress tests
 * sequentially, reports results, and shuts down the system.
 *
 * Usage (via kernel cmdline):
 *   init=/stress_runner
 *
 * The duration per test can be overridden at build time by defining
 * STRESS_DURATION (default: 20 seconds).
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

#ifndef STRESS_DURATION
#define STRESS_DURATION 20
#endif

static double elapsed_seconds(void)
{
    static struct timespec ts_start;
    static int initialized = 0;
    struct timespec now;

    if (!initialized) {
        clock_gettime(0, &ts_start);
        initialized = 1;
        return 0.0;
    }
    clock_gettime(0, &now);
    double s = (double)(now.tv_sec - ts_start.tv_sec);
    s += (double)(now.tv_nsec - ts_start.tv_nsec) / 1e9;
    return s;
}

/* Run a test program.  Since we are PID 1, we must fork and wait. */
static int run_test(const char *path, char *const argv[])
{
    int pid = fork();
    if (pid < 0) {
        printf("[stress_runner] fork failed for %s\n", path);
        return -1;
    }

    if (pid == 0) {
        /* Child: exec the test */
        execve(path, argv, NULL);
        /* If exec fails */
        printf("[stress_runner] execve(%s) failed\n", path);
        exit(127);
    }

    /* Parent: wait for child */
    int status;
    while (waitpid(pid, &status, 0) != pid) {
        /* Reap any other orphans (init's duty) */
        int orphan_pid;
        do {
            orphan_pid = waitpid(-1, NULL, WNOHANG);
        } while (orphan_pid > 0);
    }

    if (status == 0)
        return 0;
    else if (status == 127)
        return -1;
    else
        return status;
}

int main(void)
{
    int duration = STRESS_DURATION;

    printf("\n");
    printf("============================================\n");
    printf("  Hermes OS — Stress Test Suite Runner\n");
    printf("============================================\n");
    printf("  Each test runs for %d seconds\n", duration);
    printf("============================================\n");
    printf("\n");

    double wall_start = elapsed_seconds();
    int pass_count = 0;
    int fail_count = 0;
    int error_count = 0;

    /* ── Test 1: CPU stress ── */
    printf("\n--- Test 1/3: CPU Stress ---\n");
    {
        char *cpu_argv[] = {"stress_cpu", NULL};
        double t0 = elapsed_seconds();
        int rc = run_test("/bin/stress_cpu", cpu_argv);
        double t = elapsed_seconds() - t0;
        if (rc == 0) {
            printf("[stress_runner] CPU STRESS: PASS (%.1f s)\n", t);
            pass_count++;
        } else {
            printf("[stress_runner] CPU STRESS: FAIL (rc=%d, %.1f s)\n", rc, t);
            fail_count++;
        }
    }

    /* ── Test 2: Memory stress ── */
    printf("\n--- Test 2/3: Memory Stress ---\n");
    {
        char *mem_argv[] = {"stress_memory", NULL};
        double t0 = elapsed_seconds();
        int rc = run_test("/bin/stress_memory", mem_argv);
        double t = elapsed_seconds() - t0;
        if (rc == 0) {
            printf("[stress_runner] MEMORY STRESS: PASS (%.1f s)\n", t);
            pass_count++;
        } else {
            printf("[stress_runner] MEMORY STRESS: FAIL (rc=%d, %.1f s)\n", rc, t);
            fail_count++;
        }
    }

    /* ── Test 3: Disk stress ── */
    printf("\n--- Test 3/3: Disk I/O Stress ---\n");
    {
        char *disk_argv[] = {"stress_disk", NULL};
        double t0 = elapsed_seconds();
        int rc = run_test("/bin/stress_disk", disk_argv);
        double t = elapsed_seconds() - t0;
        if (rc == 0) {
            printf("[stress_runner] DISK STRESS: PASS (%.1f s)\n", t);
            pass_count++;
        } else {
            printf("[stress_runner] DISK STRESS: FAIL (rc=%d, %.1f s)\n", rc, t);
            fail_count++;
        }
    }

    double wall_elapsed = elapsed_seconds() - wall_start;

    /* ── Final Summary ── */
    printf("\n");
    printf("============================================\n");
    printf("  STRESS TEST SUITE — FINAL SUMMARY\n");
    printf("============================================\n");
    printf("  Total time:    %.1f seconds\n", wall_elapsed);
    printf("  Tests passed:  %d\n", pass_count);
    printf("  Tests failed:  %d\n", fail_count);
    printf("  Errors:        %d\n", error_count);
    printf("--------------------------------------------\n");
    if (fail_count == 0 && error_count == 0) {
        printf("  >>> OVERALL: ALL STRESS TESTS PASSED <<<\n");
    } else {
        printf("  >>> OVERALL: SOME STRESS TESTS FAILED <<<\n");
    }
    printf("============================================\n");
    printf("\n");

    /* Shut down the system */
    printf("[stress_runner] Halting system.\n");
    sync();
    reboot();

    /* Should not reach here */
    for (;;) __asm__ volatile("hlt");
    return 0;
}
