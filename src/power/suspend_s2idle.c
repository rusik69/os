// SPDX-License-Identifier: GPL-2.0-only
/*
 * suspend_s2idle.c — Suspend-to-idle (s2idle) state
 *
 * Implements the s2idle (Suspend-to-Idle) power management state,
 * the shallowest system sleep state where CPUs enter deep idle
 * but system remains responsive to interrupts.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "timer.h"
#include "smp.h"
#include "wakeup.h"

#define S2IDLE_WAKEUP_TIMEOUT 500 /* 500ms default timeout */

struct s2idle_state {
    int suspended;
    uint64_t suspend_start;
    int wakeup_reason; /* 0=none, 1=timer, 2=interrupt, 3=wakeup device */
};

static struct s2idle_state s2idle_state;

/* Enter s2idle state */
int s2idle_enter(void)
{
    if (s2idle_state.suspended)
        return -EBUSY;

    kprintf("[S2IDLE] Entering suspend-to-idle...\n");

    s2idle_state.suspended = 1;
    s2idle_state.suspend_start = timer_get_ticks();
    s2idle_state.wakeup_reason = 0;

    /* Prepare CPUs for idle */
    int ncpus = smp_get_cpu_count();

    /* Disable scheduling on all CPUs (simplified) */
    /* Freeze processes and suspend devices */

    /* Enter idle loop (wait for interrupt) */
    /* On x86, this is typically MWAIT or HLT loop */
    kprintf("[S2IDLE] CPUs entering idle (ncpus=%d)\n", ncpus);

    /* Simulate idle wait with wakeup-source checking */
    uint64_t timeout = timer_get_ticks() + S2IDLE_WAKEUP_TIMEOUT;
    while (timer_get_ticks() < timeout) {
        /* Check for wakeup conditions: iterate registered wakeup sources */
        int pending_wakeups = 0;
        for (int i = 0; i < WAKEUP_SRC_MAX; i++) {
            if (wakeup_source_is_active(i)) {
                pending_wakeups = 1;
                break;
            }
        }

        if (pending_wakeups) {
            /* A wakeup source is active — exit idle */
            s2idle_state.wakeup_reason = 3; /* wakeup device */
            break;
        }

        /* Also check if an interrupt handler signalled s2idle_wakeup() */
        if (s2idle_state.wakeup_reason != 0)
            break;

        __asm__ volatile("pause");
    }

    /* Wakeup */
    s2idle_state.wakeup_reason = 1; /* timeout wakeup */

    kprintf("[S2IDLE] Woken up (reason=%d, elapsed=%llu ms)\n",
            s2idle_state.wakeup_reason,
            (unsigned long long)(timer_get_ticks() - s2idle_state.suspend_start) * 1000 / TIMER_FREQ);

    s2idle_state.suspended = 0;
    return 0;
}

/* Check if currently in s2idle */
int s2idle_is_suspended(void)
{
    return s2idle_state.suspended;
}

/* Get wakeup reason */
int s2idle_get_wakeup_reason(void)
{
    return s2idle_state.wakeup_reason;
}

/* Exit s2idle (called from interrupt handler) */
void s2idle_wakeup(void)
{
    if (s2idle_state.suspended) {
        s2idle_state.wakeup_reason = 2; /* interrupt */
    }
}

void s2idle_init(void)
{
    memset(&s2idle_state, 0, sizeof(s2idle_state));
    kprintf("[OK] s2idle — Suspend-to-Idle state\n");
}
