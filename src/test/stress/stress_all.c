/*
 * src/test/stress/stress_all.c — Combined / soak stress orchestrator
 *
 * Covers D256 tasks:
 *   9.  Mixed load: memory pressure + disk I/O + network simultaneously
 *  10.  Runtime: long soak — runs the combined load for an extended
 *      period (default 60s; override STRESS_DURATION / argv for 24h).
 *
 * It spawns the dedicated stress ELFs as children and runs them
 * concurrently, then reports aggregate pass/fail.
 *
 * Usage:
 *   stress_all [duration_seconds] [concurrency]
 * Defaults: duration=60, concurrency=4
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

static int run_elf(const char *path, const char *name, int duration, int conc)
{
    int pid = fork();
    if (pid == 0) {
        char d[16], c[16];
        snprintf(d, sizeof(d), "%d", duration);
        snprintf(c, sizeof(c), "%d", conc);
        char *argv[] = { (char *)name, d, c, NULL };
        execve(path, argv, NULL);
        printf("[stress_all] execve(%s) failed\n", path);
        exit(127);
    } else if (pid > 0) {
        return pid;
    }
    return -1;
}

int main(int argc, char *argv[])
{
    int duration = (argc > 1) ? atoi(argv[1]) : 60;
    int conc = (argc > 2) ? atoi(argv[2]) : 4;
    if (duration < 1) duration = 1;
    if (conc < 1) conc = 1;

    printf("\n=== Hermes OS Combined / Soak Stress ===\n");
    printf("  duration=%ds concurrency=%d\n", duration, conc);
    printf("  tasks: mixed mem+disk+net load, soak\n");
    printf("======================================\n\n");

    int pids[8];
    int n = 0;

    pids[n++] = run_elf("/bin/stress_mem",   "stress_mem",   duration, conc);
    pids[n++] = run_elf("/bin/stress_sched", "stress_sched", duration, conc);
    pids[n++] = run_elf("/bin/stress_fs",    "stress_fs",    duration, conc);
    if (net_present() != 0)
        pids[n++] = run_elf("/bin/stress_net", "stress_net", duration, conc);
    else
        printf("[stress_all] skipping stress_net (no network)\n");

    int fail = 0;
    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0) { fail++; continue; }
        int st;
        waitpid(pids[i], &st, 0);
        if (st != 0) fail++;
    }

    printf("\n=== COMBINED / SOAK STRESS SUMMARY ===\n");
    printf("  children failed: %d / %d\n", fail, n);
    printf("  RESULT: %s\n", (fail == 0) ? "PASS" : "FAIL");
    printf("======================================\n");

    /* Halt the system after a soak run (used when launched as init). */
    sync();
    reboot();
    for (;;) __asm__ volatile("hlt");
    return (fail == 0) ? 0 : 1;
}
