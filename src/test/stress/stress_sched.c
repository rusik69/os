/*
 * src/test/stress/stress_sched.c — Scheduler stress: fork bomb + mixed loads
 *
 * Covers D256 tasks:
 *   3. Fork bomb with memory pressure (many short-lived children)
 *   4. CPU-bound + IO-bound mixed loads (compute + fs writes)
 *  12. SMP: concurrent processes on all CPUs (kernel schedules across APs)
 *
 * Usage:
 *   stress_sched [duration_seconds] [max_children]
 * Defaults: duration=30, max_children=64
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "sys/stat.h"

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

static double elapsed(void)
{
    static struct timespec ts0;
    static int init = 0;
    struct timespec now;
    if (!init) { clock_gettime(CLOCK_REALTIME, &ts0); init = 1; return 0.0; }
    clock_gettime(CLOCK_REALTIME, &now);
    return (double)(now.tv_sec - ts0.tv_sec) + (double)(now.tv_nsec - ts0.tv_nsec) / 1e9;
}

/* CPU-bound: sum of primes up to limit. */
static unsigned long cpu_work(unsigned long limit)
{
    unsigned long total = 0;
    for (unsigned long i = 2; i <= limit; i++) {
        int prime = 1;
        for (unsigned long j = 2; j * j <= i; j++)
            if (i % j == 0) { prime = 0; break; }
        if (prime) total += i;
    }
    return total;
}

/* IO-bound: write+read a temp file repeatedly. */
static int io_work(const char *path, int iters)
{
    int err = 0;
    for (int k = 0; k < iters; k++) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { err++; continue; }
        char buf[512];
        for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = (char)(k + i);
        if (write(fd, buf, sizeof(buf)) != (int)sizeof(buf)) err++;
        close(fd);
        fd = open(path, O_RDONLY, 0);
        if (fd < 0) { err++; continue; }
        char rbuf[512];
        if (read(fd, rbuf, sizeof(rbuf)) != (int)sizeof(rbuf)) err++;
        else if (memcmp(buf, rbuf, sizeof(buf)) != 0) err++;
        close(fd);
    }
    unlink(path);
    return err;
}

/* One mixed worker: periodic CPU + IO bursts. */
static int mixed_worker(int id, int seconds)
{
    unsigned long cpu_total = 0;
    int io_err = 0;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/sched_io_%d.tmp", id);
    int phase = 0;
    double start = elapsed();
    while (elapsed() - start < (double)seconds) {
        if (phase & 1) cpu_total += cpu_work(4000);
        else          io_err  += io_work(path, 4);
        phase++;
        yield();
    }
    printf("[stress_sched] worker %d: cpu_sum=%lu io_err=%d\n", id, cpu_total, io_err);
    return io_err ? -1 : 0;
}

/* Fork bomb: spawn children in waves, each does a little work, then exits.
 * Bounded by max_children to avoid permanently wedging the kernel. */
static int fork_bomb(int seconds, int max_children)
{
    int spawned = 0, reaped = 0, failures = 0;
    double start = elapsed();
    while (elapsed() - start < (double)seconds) {
        int live = 0;
        /* estimate live by forking in controlled bursts */
        for (int i = 0; i < 8 && live < max_children; i++) {
            int pid = fork();
            if (pid == 0) {
                /* child: tiny CPU work then exit */
                cpu_work(500);
                exit(0);
            } else if (pid > 0) {
                spawned++;
                live++;
            } else {
                failures++;
                break;
            }
        }
        /* reap any finished children */
        int wpid;
        do { wpid = waitpid(-1, NULL, 1 /* WNOHANG */); if (wpid > 0) reaped++; }
        while (wpid > 0);
        yield();
    }
    /* final reap */
    int wpid;
    do { wpid = waitpid(-1, NULL, 0); if (wpid > 0) reaped++; }
    while (wpid > 0);
    printf("[stress_sched] fork_bomb: spawned=%d reaped=%d fork_failures=%d\n",
           spawned, reaped, failures);
    return failures ? -1 : 0;
}

int main(int argc, char *argv[])
{
    int duration = (argc > 1) ? atoi(argv[1]) : 30;
    int max_children = (argc > 2) ? atoi(argv[2]) : 64;
    if (duration < 1) duration = 1;
    if (max_children < 1) max_children = 1;
    if (max_children > 256) max_children = 256;

    printf("\n=== Hermes OS Scheduler Stress ===\n");
    printf("  duration=%ds max_children=%d\n", duration, max_children);
    printf("  tasks: fork bomb + mixed CPU/IO loads\n");
    printf("=====================================\n\n");


    /* Launch mixed-load workers (also spreads across SMP CPUs). */
    int pids[16];
    int n = 0;
    for (int i = 0; i < 8; i++) {
        int pid = fork();
        if (pid == 0) { mixed_worker(i, duration); exit(0); }
        else if (pid > 0) pids[n++] = pid;
    }

    int bomb_err = fork_bomb(duration, max_children);

    int fail = 0;
    for (int i = 0; i < n; i++) {
        int st; waitpid(pids[i], &st, 0);
        if (st != 0) fail++;
    }

    printf("\n=== SCHEDULER STRESS SUMMARY ===\n");
    printf("  mixed workers failed: %d\n", fail);
    printf("  fork_bomb failure:    %s\n", bomb_err ? "FAIL" : "ok");
    int ok = (fail == 0 && bomb_err == 0);
    printf("  RESULT: %s\n", ok ? "PASS" : "FAIL");
    printf("=================================\n");
    return ok ? 0 : 1;
}
