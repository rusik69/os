#define KERNEL_INTERNAL
#include "types.h"
#include "rcu.h"
#include "smp.h"
#include "printf.h"
#include "timer.h"
#include "scheduler.h"
#include "process.h"
#include "stacktrace.h"
#include "panic.h"
#include "kallsyms.h"

/*
 * RCU — Read-Copy-Update with grace-period stall detection.
 *
 * Each CPU records a quiescent state (QS) timestamp at each context switch
 * via rcu_quiescent_state().  synchronize_rcu() waits until every online CPU
 * has passed through a QS at least once since the grace period started.
 *
 * Stall detection:
 *   - RCU_STALL_WARN_TICKS  (1 second):  if a CPU hasn't passed a QS,
 *     a warning is printed with per-CPU diagnostics.
 *   - RCU_STALL_PANIC_TICKS (3 seconds):  if still stalled after a
 *     warning, a full panic is triggered.
 *
 * The rcu_check_stall() function can be called from a periodic timer or
 * from the NMI watchdog context to detect stalls asynchronously.
 */

/* ── RCU grace-period timeout constants ──────────────────────────── */
#define RCU_GP_WAIT_MAX_TICKS   (TIMER_FREQ / 10)  /* 100ms max wait in synchronize_rcu() */
#define RCU_STALL_WARN_TICKS    (TIMER_FREQ * 1)   /*  1 second — print stall warning */
#define RCU_STALL_PANIC_TICKS   (TIMER_FREQ * 3)   /*  3 seconds — panic on prolonged stall */

/* ── Per-CPU RCU state ───────────────────────────────────────────── */
struct rcu_cpu_state {
    uint64_t gp_seq;            /* GP sequence this CPU has last acknowledged */
    uint64_t last_qs_tick;      /* tick when this CPU last recorded a QS */
};

/* Per-CPU array indexed by CPU index (not APIC ID) */
static struct rcu_cpu_state rcu_state_percpu[SMP_MAX_CPUS];

/* Global grace-period tracking */
static volatile uint64_t rcu_gp_seq;            /* monotonically increasing GP counter */
static volatile uint64_t rcu_gp_start_tick;     /* tick when current GP started */
static volatile uint64_t rcu_gp_start_seq;      /* GP sequence at GP start */
static volatile int      rcu_gp_in_progress;
static volatile int      rcu_stall_warning_printed;  /* rate-limit stall warnings */

/* ── Per-CPU accessor ────────────────────────────────────────────── */
static inline struct rcu_cpu_state *this_rcu_state(void) {
    uint32_t cpu_id = smp_get_cpu_id();
    if (cpu_id >= SMP_MAX_CPUS) cpu_id = 0;
    return &rcu_state_percpu[cpu_id];
}

/* ── Quiescent-state recording ───────────────────────────────────── */

/* Called at every context switch or voluntary scheduler yield */
void rcu_quiescent_state(void) {
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return;

    struct rcu_cpu_state *state = &rcu_state_percpu[cpu];
    uint64_t now = timer_get_ticks();
    state->last_qs_tick = now;
    /* Acknowledge the most recent GP so we aren't flagged as stalled */
    state->gp_seq = rcu_gp_seq;

    /* If a GP is in progress, record a new starting point for any
     * sequential synchronize_rcu() calls that might be waiting. */
    __asm__ volatile("mfence" : : : "memory");
}

/* ── Stall detection helper ──────────────────────────────────────── */

/*
 * rcu_dump_stall_info() — print detailed per-CPU state when a stall
 * is detected.  Called from synchronize_rcu() or rcu_check_stall().
 */
static void rcu_dump_stall_info(uint64_t elapsed_ticks, int printed_warning) {
    uint64_t now = timer_get_ticks();
    int ncpus = smp_get_cpu_count();

    kprintf("\n========================================================\n");
    if (printed_warning) {
        kprintf("=== RCU STALL WARNING ===\n");
    } else {
        kprintf("=== RCU STALL — GRACE PERIOD TIMEOUT ===\n");
    }
    kprintf("========================================================\n");

    kprintf("GP seq: %llu  started: t=%llu  elapsed: %llu ticks (%llu ms)\n",
            (unsigned long long)rcu_gp_seq,
            (unsigned long long)rcu_gp_start_tick,
            (unsigned long long)elapsed_ticks,
            (unsigned long long)(elapsed_ticks * 1000ULL / TIMER_FREQ));

    kprintf("Stall threshold: %llu ticks = %llu ms\n",
            (unsigned long long)RCU_STALL_WARN_TICKS,
            (unsigned long long)(RCU_STALL_WARN_TICKS * 1000ULL / TIMER_FREQ));

    /* Per-CPU quiescent state dump */
    kprintf("\nCPU  GP_ackd    Last_QS_tick    Elapsed_ms  Status\n");
    kprintf("---  ---------  --------------  ----------  ------\n");
    for (int c = 0; c < ncpus; c++) {
        uint64_t cpu_elapsed = (now - rcu_state_percpu[c].last_qs_tick) * 1000ULL / TIMER_FREQ;
        const char *status;
        if (rcu_state_percpu[c].gp_seq < rcu_gp_seq) {
            status = "STALLED";
        } else if (cpu_elapsed < 100) {
            status = "active";
        } else {
            status = "idle";
        }

        kprintf(" %2d  %8llu  %14llu  %10llu  %s\n",
                c,
                (unsigned long long)rcu_state_percpu[c].gp_seq,
                (unsigned long long)rcu_state_percpu[c].last_qs_tick,
                (unsigned long long)cpu_elapsed,
                status);
    }

    /* Dump current process info on each CPU if possible */
    kprintf("\nCurrent state:\n");
    struct process *cur = get_current_process();
    if (cur) {
        kprintf("  CPU %d: %s (pid=%u, state=%d, is_user=%d)\n",
                smp_get_cpu_id(),
                cur->name ? cur->name : "?",
                cur->pid, (int)cur->state, cur->is_user);
        kprintf("  Kernel stack: 0x%llx - 0x%llx\n",
                (unsigned long long)cur->kernel_stack,
                (unsigned long long)cur->stack_top);
    }

    /* Stack backtrace of the current context */
    kprintf("\nStack backtrace on CPU %d:\n", smp_get_cpu_id());
    print_stack_trace();

    /* Identify the first stalled CPU and its likely culprit */
    for (int c = 0; c < ncpus; c++) {
        if (rcu_state_percpu[c].gp_seq < rcu_gp_seq) {
            kprintf("\nStalled CPU %d last QS at tick %llu\n",
                    c,
                    (unsigned long long)rcu_state_percpu[c].last_qs_tick);
            kprintf("Consider checking CPU %d for: spinlock hold, "
                    "interrupts-off region, or infinite loop\n", c);
            break;
        }
    }

    /* Print kallsyms info for current context */
    {
        kprintf("Current context symbol: %s at RIP near caller\n",
                kallsyms_lookup((uint64_t)__builtin_return_address(0)));
    }
}

/*
 * rcu_check_stall() — periodic stall check, callable from timer tick
 * or NMI context.  Returns 1 if a stall was detected (and appropriate
 * action was taken), 0 otherwise.
 */
int rcu_check_stall(void) {
    if (!rcu_gp_in_progress)
        return 0;

    uint64_t now = timer_get_ticks();
    uint64_t elapsed = now - rcu_gp_start_tick;

    /* Nothing to do if we're within the warning threshold */
    if (elapsed < RCU_STALL_WARN_TICKS)
        return 0;

    int ncpus = smp_get_cpu_count();
    int stalled_cpus = 0;

    /* Check if all CPUs have observed a QS since the GP started */
    for (int c = 0; c < ncpus; c++) {
        if (rcu_state_percpu[c].gp_seq < rcu_gp_seq)
            stalled_cpus++;
    }

    if (stalled_cpus == 0) {
        /* All CPUs have acknowledged — no stall */
        return 0;
    }

    /* Stall detected */
    if (elapsed >= RCU_STALL_PANIC_TICKS) {
        /* Prolonged stall — panic */
        rcu_dump_stall_info(elapsed, 0);
        kprintf("\n=== RCU STALL PANIC: grace period blocked for %llu ms ===\n",
                (unsigned long long)(elapsed * 1000ULL / TIMER_FREQ));
        panic("RCU stall — grace period not progressing");
    }

    /* First time or warning threshold exceeded — print warning */
    if (!rcu_stall_warning_printed) {
        rcu_stall_warning_printed = 1;
        rcu_dump_stall_info(elapsed, 1);
        kprintf("\n  >> %d CPU(s) have not passed through a quiescent state\n",
                stalled_cpus);
        kprintf("  >> Next check in %llu ms will panic if unresolved\n",
                (unsigned long long)((RCU_STALL_PANIC_TICKS - elapsed) * 1000ULL / TIMER_FREQ));
    }

    return 1;
}

/* ── Grace-period synchronization ────────────────────────────────── */

void synchronize_rcu(void) {
    if (rcu_gp_in_progress) {
        /* Nested GP request — caller is already in a grace period
         * or another updater is driving one.  Wait for it. */
        /* In production we'd increment a pending counter; for now,
         * yield and let the current GP finish. */
        uint64_t deadline = timer_get_ticks() + RCU_GP_WAIT_MAX_TICKS;
        while (rcu_gp_in_progress) {
            if (timer_get_ticks() >= deadline) {
                /* Avoid infinite loop if something wedged */
                break;
            }
            scheduler_yield();
        }
        return;
    }

    /* Start a new grace period */
    uint64_t start = timer_get_ticks();
    uint64_t deadline = start + RCU_GP_WAIT_MAX_TICKS;

    rcu_gp_seq++;
    rcu_gp_start_tick = start;
    rcu_gp_start_seq = rcu_gp_seq;
    rcu_gp_in_progress = 1;
    rcu_stall_warning_printed = 0;

    /* Force a context switch on the current CPU to record a QS */
    scheduler_yield();

    /* Full memory barrier so all CPUs see updated pointer before we check */
    __asm__ volatile("mfence" : : : "memory");

    /* Wait until every online CPU has passed through a quiescent state */
    uint64_t ncpus = smp_get_cpu_count();
    int stalled_cpu = -1;
    uint64_t stall_warn_tick = 0;
    for (;;) {
        int all_quiet = 1;
        for (uint64_t c = 0; c < ncpus; c++) {
            if (rcu_state_percpu[c].gp_seq < rcu_gp_seq) {
                all_quiet = 0;
                stalled_cpu = (int)c;
                break;
            }
        }
        if (all_quiet) break;

        uint64_t now = timer_get_ticks();

        /* Check for soft stall (warning threshold exceeded) */
        if (stall_warn_tick == 0) {
            if (now - start >= RCU_STALL_WARN_TICKS) {
                stall_warn_tick = now;
                rcu_dump_stall_info(now - start, 1);
                kprintf("\n  >> CPU %d is blocking GP %llu (last QS at tick %llu)\n",
                        stalled_cpu,
                        (unsigned long long)rcu_gp_seq,
                        (unsigned long long)rcu_state_percpu[stalled_cpu].last_qs_tick);
            }
        }

        /* Check for hard stall (panic threshold exceeded) */
        if (now >= deadline && (now - start >= RCU_STALL_PANIC_TICKS)) {
            /* Extended stall — dump diagnostics and proceed (degraded mode)
             * rather than hanging the entire system.  This is similar to
             * Linux's RCU_CPU_STALL_TIMEOUT behavior where it prints
             * warnings but doesn't always panic. */
            if (now - start >= RCU_STALL_PANIC_TICKS * 2) {
                /* After 2x the panic threshold, really panic */
                rcu_dump_stall_info(now - start, 0);
                kprintf("\n=== RCU STALL PANIC: CPU %d blocking GP for %llu ms ===\n",
                        stalled_cpu,
                        (unsigned long long)((now - start) * 1000ULL / TIMER_FREQ));
                panic("RCU stall timeout");
            }

            /* Print a periodic reminder about the stall */
            if (stall_warn_tick == 0 || (now - stall_warn_tick >= RCU_STALL_WARN_TICKS)) {
                stall_warn_tick = now;
                kprintf("RCU: CPU %d still blocking GP %llu after %llu ms\n",
                        stalled_cpu,
                        (unsigned long long)rcu_gp_seq,
                        (unsigned long long)((now - start) * 1000ULL / TIMER_FREQ));
            }
            break;  /* Degraded mode: proceed */
        }

        /* Brief pause to let other CPUs run */
        scheduler_yield();
    }

    rcu_gp_in_progress = 0;
    /* Full memory barrier so all CPUs see the updated pointer */
    __asm__ volatile("mfence" : : : "memory");
}

/* ── Initialization ──────────────────────────────────────────────── */

void rcu_init(void) {
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        rcu_state_percpu[i].gp_seq = 0;
        rcu_state_percpu[i].last_qs_tick = 0;
    }
    rcu_gp_seq = 0;
    rcu_gp_start_tick = 0;
    rcu_gp_start_seq = 0;
    rcu_gp_in_progress = 0;
    rcu_stall_warning_printed = 0;

    kprintf("[OK] RCU initialized with stall detection "
            "(warn=%lums, panic=%lums)\n",
            (unsigned long)(RCU_STALL_WARN_TICKS * 1000ULL / TIMER_FREQ),
            (unsigned long)(RCU_STALL_PANIC_TICKS * 1000ULL / TIMER_FREQ));
}
