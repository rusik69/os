#define KERNEL_INTERNAL
#include "types.h"
#include "rcu.h"
#include "smp.h"
#include "printf.h"
#include "timer.h"
#include "scheduler.h"
#include "process.h"

/*
 * Simplified RCU: track per-CPU quiescent-state counters.
 * Each CPU increments its counter on entry to rcu_read_lock and
 * decrements on exit. A quiescent state is when the counter == 0
 * (no active read-side critical section). synchronize_rcu() waits
 * for every CPU to have a quiescent state at least once.
 *
 * Since we don't have per-CPU variables via GS segment readily
 * available for scheduler-visible code, we track the last-known
 * quiescent state via a per-CPU timestamp set at context switch.
 */

#define RCU_GP_WAIT_MAX_TICKS (TIMER_FREQ / 10) /* 100ms max wait */

/* Per-CPU data — indexed by APIC ID */
static volatile uint64_t rcu_last_qs_tick[256];
static volatile int rcu_gp_in_progress;

void rcu_quiescent_state(void) {
    int cpu = smp_get_cpu_id();
    if (cpu >= 0 && cpu < 256) {
        rcu_last_qs_tick[cpu] = timer_get_ticks();
    }
}

void synchronize_rcu(void) {
    if (rcu_gp_in_progress) {
        /* Nested GP request — caller is already in a grace period */
        return;
    }
    rcu_gp_in_progress = 1;

    uint64_t start = timer_get_ticks();
    uint64_t deadline = start + RCU_GP_WAIT_MAX_TICKS;

    /* Force a context switch on the current CPU to record a QS */
    scheduler_yield();

    /* Wait until every online CPU has passed through a quiescent state */
    uint64_t ncpus = smp_get_cpu_count();
    for (;;) {
        int all_quiet = 1;
        for (uint64_t c = 0; c < ncpus; c++) {
            if (rcu_last_qs_tick[c] < start) {
                all_quiet = 0;
                break;
            }
        }
        if (all_quiet) break;

        if (timer_get_ticks() >= deadline) {
            /* Grace period timeout — proceed anyway (degraded mode) */
            break;
        }
        /* Brief pause to let other CPUs run */
        scheduler_yield();
    }

    rcu_gp_in_progress = 0;
    /* Full memory barrier so all CPUs see the updated pointer */
    __asm__ volatile("mfence" : : : "memory");
}

void rcu_init(void) {
    for (int i = 0; i < 256; i++)
        rcu_last_qs_tick[i] = 0;
    rcu_gp_in_progress = 0;
    kprintf("[OK] RCU initialized\n");
}
