#ifndef TASKSTATS_H
#define TASKSTATS_H

#include "types.h"

/*
 * Per-task statistics structure (similar to Linux's struct taskstats).
 *
 * Accumulates CPU time, run-delay, voluntary/involuntary context switch
 * counts, and I/O accounting.
 */

struct taskstats {
    /* ── Version / command ────────────────────────────────── */
    uint16_t  version;           /* structure version (set to 1) */
    uint32_t  ac_exitcode;       /* exit code (if applicable) */

    /* ── CPU time accounting (ticks) ───────────────────────── */
    uint64_t  cpu_delay_total;   /* total time (ticks) spent waiting for CPU */
    uint64_t  cpu_time_user;     /* ticks in user mode */
    uint64_t  cpu_time_system;   /* ticks in kernel mode */
    uint64_t  cpu_time_virtual;  /* virtual time (user+system) */

    /* ── Context switch accounting ─────────────────────────── */
    uint64_t  nvcsw;             /* voluntary context switches (yield, sleep) */
    uint64_t  nivcsw;            /* involuntary context switches (preemption) */

    /* ── I/O accounting (byte counts) ──────────────────────── */
    uint64_t  read_bytes;        /* bytes read */
    uint64_t  write_bytes;       /* bytes written */
    uint64_t  read_syscalls;     /* number of read() calls */
    uint64_t  write_syscalls;    /* number of write() calls */

    /* ── Memory / fault accounting ─────────────────────────── */
    uint64_t  minflt;            /* minor page faults */
    uint64_t  majflt;            /* major page faults */
    uint64_t  vsize;             /* virtual memory size (bytes) */
    uint64_t  rss;               /* resident set size (pages) */

    /* ── Timestamps ────────────────────────────────────────── */
    uint64_t  ac_btime;          /* start time (ticks since boot) */
    uint64_t  ac_etime;          /* elapsed time (ticks since start) */
};

/*
 * taskstats_get  - Read the current task's statistics into @stats.
 * Returns 0 on success.
 */
int taskstats_get(struct taskstats *stats);

/*
 * taskstats_accumulate  - Add the counters from @src into @dst.
 * Used to aggregate per-task stats into per-process or per-group totals.
 */
void taskstats_accumulate(struct taskstats *dst, const struct taskstats *src);

/*
 * taskstats_account_process  - Update the taskstats for @stats based
 * on the given process's current accounting fields.
 */
void taskstats_account_process(struct taskstats *stats, uint64_t utime,
                                uint64_t stime, uint64_t nvcsw,
                                uint64_t nivcsw, uint64_t minflt,
                                uint64_t majflt);

void taskstats_init(void);

#endif /* TASKSTATS_H */
