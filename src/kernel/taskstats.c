#include "taskstats.h"
#include "printf.h"
#include "kernel.h"
#include "string.h"
#include "process.h"
#include "smp.h"

int taskstats_get(struct taskstats *stats)
{
    struct process *cur;

    if (!stats)
        return -1;

    memset(stats, 0, sizeof(*stats));
    stats->version = 1;

    cur = get_current_process();
    if (!cur)
        return 0;

    /* Fill from process accounting fields */
    stats->cpu_time_user   = cur->cpu_user;
    stats->cpu_time_system = cur->cpu_system;
    stats->cpu_time_virtual = cur->cpu_user + cur->cpu_system;
    stats->nvcsw           = cur->nvcsw;
    stats->nivcsw          = cur->nivcsw;
    stats->minflt          = cur->minflt;
    stats->majflt          = cur->majflt;
    stats->ac_btime        = cur->start_time_tick;
    stats->ac_etime        = 0; /* would need timer_get_ticks() - start */

    /* The process struct doesn't track I/O bytes at this level,
     * so read_bytes/write_bytes remain 0. */

    return 0;
}

void taskstats_accumulate(struct taskstats *dst, const struct taskstats *src)
{
    if (!dst || !src)
        return;

    dst->cpu_delay_total       += src->cpu_delay_total;
    dst->cpu_time_user         += src->cpu_time_user;
    dst->cpu_time_system       += src->cpu_time_system;
    dst->cpu_time_virtual      += src->cpu_time_virtual;
    dst->nvcsw                 += src->nvcsw;
    dst->nivcsw                += src->nivcsw;
    dst->read_bytes            += src->read_bytes;
    dst->write_bytes           += src->write_bytes;
    dst->read_syscalls         += src->read_syscalls;
    dst->write_syscalls        += src->write_syscalls;
    dst->minflt                += src->minflt;
    dst->majflt                += src->majflt;
    /* ac_exitcode, ac_btime, ac_etime, vsize, rss not accumulated */
}

void taskstats_account_process(struct taskstats *stats, uint64_t utime,
                                uint64_t stime, uint64_t nvcsw,
                                uint64_t nivcsw, uint64_t minflt,
                                uint64_t majflt)
{
    if (!stats)
        return;

    stats->cpu_time_user   += utime;
    stats->cpu_time_system += stime;
    stats->cpu_time_virtual = stats->cpu_time_user + stats->cpu_time_system;
    stats->nvcsw           += nvcsw;
    stats->nivcsw          += nivcsw;
    stats->minflt          += minflt;
    stats->majflt          += majflt;
}

void taskstats_init(void)
{
    kprintf("[OK] taskstats: Per-task statistics initialised\n");
}

/* ── Stub: taskstats_collect ─────────────────────────────── */
int taskstats_collect(int pid, void *stats)
{
    (void)pid;
    (void)stats;
    kprintf("[taskstats] taskstats_collect: not yet implemented\n");
    return 0;
}
/* ── Stub: taskstats_clear ─────────────────────────────── */
int taskstats_clear(int pid)
{
    (void)pid;
    kprintf("[taskstats] taskstats_clear: not yet implemented\n");
    return 0;
}
/* ── Stub: taskstats_register ─────────────────────────────── */
int taskstats_register(void *listener)
{
    (void)listener;
    kprintf("[taskstats] taskstats_register: not yet implemented\n");
    return 0;
}
