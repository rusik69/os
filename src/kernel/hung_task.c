/*
 * hung_task.c — Hung task detection
 *
 * Detects tasks that have been blocked (D state) for longer than a
 * configurable timeout.  When a task is detected as hung, a warning
 * is printed and optionally the task is killed or a panic is triggered.
 *
 * Based on the Linux hung_task_panic mechanism.
 */

#define KERNEL_INTERNAL
#include "hung_task.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "scheduler.h"
#include "process.h"
#include "spinlock.h"

/* ── Configuration ─────────────────────────────────────────────────── */

/* Default timeout: 120 seconds (in timer ticks) */
static uint64_t g_hung_task_timeout_ticks = 0;  /* 0 = disabled */
static int      g_hung_task_panic  = 0;          /* panic on detected hung task */
static int      g_hung_task_check_count = 0;     /* number of checks performed */

/* How often to check (every N seconds) */
#define HUNG_TASK_CHECK_INTERVAL (TIMER_FREQ * 5)  /* every 5 seconds */

/* Last check tick */
static uint64_t g_last_check_tick = 0;

/* ── Detection ──────────────────────────────────────────────────────── */

void hung_task_set_timeout(int seconds)
{
    if (seconds <= 0) {
        g_hung_task_timeout_ticks = 0;
        kprintf("[hung_task] Detection disabled\n");
    } else {
        g_hung_task_timeout_ticks = (uint64_t)seconds * TIMER_FREQ;
        kprintf("[hung_task] Timeout set to %d seconds (%llu ticks)\n",
                seconds, (unsigned long long)g_hung_task_timeout_ticks);
    }
}

void hung_task_set_panic(int enable)
{
    g_hung_task_panic = enable;
    kprintf("[hung_task] Panic on hung task: %s\n", enable ? "enabled" : "disabled");
}

/* Check all tasks for hung state.
 * Called periodically from the timer tick. */
void hung_task_check(void)
{
    if (g_hung_task_timeout_ticks == 0)
        return;

    uint64_t now = timer_get_ticks();

    /* Rate-limit checks */
    if (now - g_last_check_tick < HUNG_TASK_CHECK_INTERVAL)
        return;
    g_last_check_tick = now;

    g_hung_task_check_count++;

    /* Iterate all processes looking for blocked tasks */
    int hung_found = 0;
    for (uint32_t pid = 1; pid < 4096; pid++) {
        struct process *p = process_get_by_pid(pid);
        if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_ZOMBIE)
            continue;

        /* Check for D-state (uninterruptible sleep) tasks */
        if (p->state == PROCESS_BLOCKED && !p->is_suspended) {
            uint64_t blocked_since = p->sleep_until;
            if (blocked_since > 0 && now > blocked_since + g_hung_task_timeout_ticks) {
                /* Task has been blocked longer than timeout */
                kprintf("\n=== HUNG TASK DETECTED ===\n");
                kprintf("PID: %u  NAME: %s\n", pid, p->name ? p->name : "?");
                kprintf("Blocked since tick: %llu (now: %llu, elapsed: %llu ticks)\n",
                        (unsigned long long)blocked_since,
                        (unsigned long long)now,
                        (unsigned long long)(now - blocked_since));
                kprintf("Timeout: %llu ticks\n",
                        (unsigned long long)g_hung_task_timeout_ticks);
                kprintf("State: %d  KSTACK: 0x%llx\n",
                        (int)p->state, (unsigned long long)p->kernel_stack);

                hung_found++;

                if (g_hung_task_panic) {
                    /* Dump and panic */
                    kprintf("Hung task panic triggered!\n");
                    panic("Hung task detected");
                }
            }
        }
    }

    if (hung_found > 0) {
        kprintf("[hung_task] %d hung task(s) detected in check #%d\n",
                hung_found, g_hung_task_check_count);
    }
}

/* Get hung task statistics */
void hung_task_get_stats(uint64_t *timeout_ticks, int *panic_mode, int *check_count)
{
    if (timeout_ticks) *timeout_ticks = g_hung_task_timeout_ticks;
    if (panic_mode)    *panic_mode    = g_hung_task_panic;
    if (check_count)   *check_count   = g_hung_task_check_count;
}

/* ── Initialization ──────────────────────────────────────────────────── */

void hung_task_init(void)
{
    g_hung_task_timeout_ticks = TIMER_FREQ * 120;  /* 120 seconds default */
    g_hung_task_panic = 0;
    g_hung_task_check_count = 0;
    g_last_check_tick = 0;

    kprintf("[OK] hung_task: detection initialized (timeout=%d seconds, panic=%s)\n",
            120, "disabled");
}

/* ── Stub: hung_task_detect ────────────────────────────────────────── */
int hung_task_detect(void)
{
    kprintf("[HUNG_TASK] hung_task_detect: not yet implemented\n");
    return 0;
}

/* ── Stub: hung_task_panic ─────────────────────────────────────────── */
void hung_task_panic(void)
{
    kprintf("[HUNG_TASK] hung_task_panic: not yet implemented\n");
}

/* ── Stub: hung_task_check_count ───────────────────────────────────── */
int hung_task_check_count(void)
{
    kprintf("[HUNG_TASK] hung_task_check_count: not yet implemented\n");
    return 0;
}

/* ── Stub: hung_task_timeout_secs ──────────────────────────────────── */
uint64_t hung_task_timeout_secs(void)
{
    kprintf("[HUNG_TASK] hung_task_timeout_secs: not yet implemented\n");
    return 0;
}
