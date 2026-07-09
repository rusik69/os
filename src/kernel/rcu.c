#define KERNEL_INTERNAL
#include "rcu.h"
#include "types.h"
#include "smp.h"
#include "printf.h"
#include "timer.h"
#include "scheduler.h"
#include "process.h"
#include "stacktrace.h"
#include "panic.h"
#include "kallsyms.h"
#include "apic.h"
#include "export.h"
#include "spinlock.h"

/*
 * RCU — Read-Copy-Update with grace-period stall detection and
 *       asynchronous callback (call_rcu) support.
 *
 * Each CPU records a quiescent state (QS) timestamp at each context switch
 * via rcu_quiescent_state().  synchronize_rcu() waits until every online CPU
 * has passed through a QS at least once since the grace period started.
 *
 * call_rcu() callbacks are batched per grace period.  A global callback
 * list (rcu_cblist) collects pending callbacks.  When all CPUs have passed
 * a QS, the list is moved to a done list and the callbacks are invoked
 * during the next timer tick (from rcu_check_stall()).
 *
 * Stall detection:
 *   - RCU_STALL_WARN_TICKS  (1 second):  if a CPU hasn't passed a QS,
 *     a warning is printed with per-CPU diagnostics and an IPI backtrace
 *     is sent to the stalled CPU.
 *   - RCU_STALL_PANIC_TICKS (3 seconds):  if still stalled after a
 *     warning, a full panic is triggered.
 *
 * The rcu_check_stall() function is called from the system timer tick
 * once per second.  It drives both stall detection and GP completion.
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

/* ── Global grace-period tracking ────────────────────────────────── */
static volatile uint64_t rcu_gp_seq;            /* monotonically increasing GP counter */
static volatile uint64_t rcu_gp_start_tick;     /* tick when current GP started */
static volatile uint64_t rcu_gp_start_seq;      /* GP sequence at GP start */
static volatile int      rcu_gp_in_progress;
static volatile int      rcu_stall_warning_printed;  /* rate-limit stall warnings */

/* Lock protecting grace-period state (rcu_gp_seq, rcu_gp_in_progress, etc.) */
static spinlock_t rcu_gp_lock;

/* ── call_rcu() callback lists ───────────────────────────────────── */

/*
 * We maintain two global callback lists protected by a simple flag:
 *
 *   rcu_pending_list  — callbacks awaiting a grace period.
 *   rcu_done_list     — callbacks whose GP has completed, ready to invoke.
 *
 * A new GP is started whenever rcu_pending_list becomes non-empty and
 * no GP is currently in progress.  When rcu_check_stall() detects that
 * all CPUs have acknowledged the current GP, it moves the pending list
 * to the done list and invokes the done callbacks.
 */
static struct rcu_head *rcu_pending_list;
static struct rcu_head *rcu_pending_tail;
static struct rcu_head *rcu_done_list;
static volatile int      rcu_cb_lock;   /* simple spinlock for list ops */

/*
 * Counters for rcu_barrier() — total callbacks queued and total
 * invoked so far.  rcu_barrier() polls until they match.
 */
static volatile uint64_t rcu_n_cbs_queued;
static volatile uint64_t rcu_n_cbs_invoked;

/* ── Per-CPU accessor ────────────────────────────────────────────── */
static inline struct rcu_cpu_state *this_rcu_state(void) {
    uint32_t cpu_id = smp_get_cpu_id();
    if (cpu_id >= SMP_MAX_CPUS) cpu_id = 0;
    return &rcu_state_percpu[cpu_id];
}

/* ── Lock/unlock for callback list manipulation ──────────────────── */
static inline void rcu_cb_lock_acquire(void) {
    for (;;) {
        if (!__atomic_test_and_set(&rcu_cb_lock, __ATOMIC_ACQUIRE))
            break;
        __asm__ volatile("pause");
    }
}

static inline void rcu_cb_lock_release(void) {
    __atomic_clear(&rcu_cb_lock, __ATOMIC_RELEASE);
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
EXPORT_SYMBOL(rcu_quiescent_state);

/* ── Stall detection helper ──────────────────────────────────────── */

/*
 * Send an IPI backtrace to a specific stalled CPU to get its stack dump.
 * Uses the existing IPI_VECTOR_BACKTRACE mechanism.
 */
static void rcu_ipi_stalled_cpu(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= smp_get_cpu_count())
        return;

    uint32_t apic_id = cpu_info_array[cpu_id].apic_id;
    kprintf("\n[RCU] Sending backtrace IPI to stalled CPU %d (APIC %u)...\n",
            cpu_id, (unsigned int)apic_id);

    /* Send IPI to the specific CPU using its APIC ID */
    apic_send_ipi(apic_id, IPI_VECTOR_BACKTRACE);

    /* Brief delay to let the IPI arrive and the handler run */
    for (volatile int d = 0; d < 100000; d++)
        __asm__ volatile("pause");
}

/*
 * rcu_dump_stall_info() — print detailed per-CPU state when a stall
 * is detected.  Called from synchronize_rcu() or rcu_check_stall().
 * If @send_ipi is non-zero, IPI backtraces are sent to stalled CPUs.
 */
static void rcu_dump_stall_info(uint64_t elapsed_ticks, int printed_warning,
                                int send_ipi) {
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
    int first_stalled = -1;
    for (int c = 0; c < ncpus; c++) {
        uint64_t cpu_elapsed = (now - rcu_state_percpu[c].last_qs_tick) * 1000ULL / TIMER_FREQ;
        const char *status;
        if (rcu_state_percpu[c].gp_seq < rcu_gp_seq) {
            status = "STALLED";
            if (first_stalled < 0)
                first_stalled = c;
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
    if (first_stalled >= 0) {
        kprintf("\nStalled CPU %d last QS at tick %llu\n",
                first_stalled,
                (unsigned long long)rcu_state_percpu[first_stalled].last_qs_tick);
        kprintf("Consider checking CPU %d for: spinlock hold, "
                "interrupts-off region, or infinite loop\n", first_stalled);
    }

    /* Send IPI backtrace to stalled CPUs for detailed diagnostics */
    if (send_ipi) {
        for (int c = 0; c < ncpus; c++) {
            if (rcu_state_percpu[c].gp_seq < rcu_gp_seq) {
                rcu_ipi_stalled_cpu(c);
            }
        }
    }

    /* Print kallsyms info for current context */
    {
        kprintf("Current context symbol: %s at RIP near caller\n",
                kallsyms_lookup((uint64_t)__builtin_return_address(0)));
    }
}

/* ── Invoke completed RCU callbacks ──────────────────────────────── */

/*
 * rcu_invoke_callbacks() — invoke all callbacks on the done list.
 * Called from rcu_check_stall() once per second when a GP completes.
 */
static void rcu_invoke_callbacks(void) {
    struct rcu_head *list;

    /* Atomically take the entire done list */
    rcu_cb_lock_acquire();
    list = rcu_done_list;
    rcu_done_list = NULL;
    rcu_cb_lock_release();

    if (!list)
        return;

    /* Invoke each callback.  We track the count for rcu_barrier(). */
    struct rcu_head *cb = list;
    while (cb) {
        struct rcu_head *next = cb->next;
        if (cb->func) {
            cb->func(cb);
        }
        __atomic_add_fetch(&rcu_n_cbs_invoked, 1, __ATOMIC_RELEASE);
        cb = next;
    }
}

/*
 * rcu_try_complete_gp() — check if all CPUs have acknowledged the
 * current GP.  If yes, move pending callbacks to the done list and
 * end the GP.  Returns 1 if a GP was completed, 0 otherwise.
 */
static int rcu_try_complete_gp(void) {
    if (!rcu_gp_in_progress)
        return 0;

    int ncpus = smp_get_cpu_count();

    /* Check whether every online CPU has acknowledged this GP */
    for (int c = 0; c < ncpus; c++) {
        if (rcu_state_percpu[c].gp_seq < rcu_gp_seq)
            return 0;  /* at least one CPU still pending */
    }

    /* All CPUs have passed through a QS — GP complete */
    rcu_gp_in_progress = 0;

    /* Move pending callbacks to the done list */
    rcu_cb_lock_acquire();
    if (rcu_pending_list) {
        /* Append the entire pending list to the done list */
        if (rcu_done_list) {
            /* Find the tail of the current done list */
            struct rcu_head *tail = rcu_done_list;
            while (tail->next)
                tail = tail->next;
            tail->next = rcu_pending_list;
        } else {
            rcu_done_list = rcu_pending_list;
        }
        rcu_pending_list = NULL;
        rcu_pending_tail = NULL;
    }
    rcu_cb_lock_release();

    return 1;
}

/*
 * rcu_start_gp() — begin a new grace period if there are pending
 * callbacks and no GP is currently in progress.
 */
static void rcu_start_gp(void) {
    if (rcu_gp_in_progress)
        return;

    rcu_cb_lock_acquire();
    int has_pending = (rcu_pending_list != NULL);
    rcu_cb_lock_release();

    if (!has_pending)
        return;

    /* Start a new grace period */
    rcu_gp_seq++;
    rcu_gp_start_tick = timer_get_ticks();
    rcu_gp_start_seq = rcu_gp_seq;
    rcu_gp_in_progress = 1;
    rcu_stall_warning_printed = 0;

    /* Full memory barrier so all CPUs see updated GP sequence */
    __asm__ volatile("mfence" : : : "memory");
}

/* ── Periodic stall check + GP advancement ───────────────────────── */

/*
 * rcu_check_stall() — periodic stall check, callable from timer tick
 * or NMI context.  Returns 1 if a stall was detected (and appropriate
 * action was taken), 0 otherwise.
 *
 * Also drives grace-period advancement: if a GP has completed, invoke
 * done callbacks; if there are pending callbacks and no GP, start one.
 */
int rcu_check_stall(void) {
    /* First: advance the GP machinery if possible */

    /* Try to complete any in-progress grace period */
    if (rcu_try_complete_gp()) {
        /* GP completed — invoke the done callbacks */
        rcu_invoke_callbacks();
    }

    /* If there are pending callbacks and no GP in progress, start one */
    rcu_start_gp();

    /* ── Stall detection below this point ── */

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
        /* Prolonged stall — panic with full diagnostics including IPI backtrace */
        rcu_dump_stall_info(elapsed, 0, 1);  /* send_ipi = 1 */
        kprintf("\n=== RCU STALL PANIC: grace period blocked for %llu ms ===\n",
                (unsigned long long)(elapsed * 1000ULL / TIMER_FREQ));
        panic("RCU stall — grace period not progressing");
    }

    /* First time or warning threshold exceeded — print warning */
    if (!rcu_stall_warning_printed) {
        rcu_stall_warning_printed = 1;
        rcu_dump_stall_info(elapsed, 1, 1);  /* send_ipi = 1 */
        kprintf("\n  >> %d CPU(s) have not passed through a quiescent state\n",
                stalled_cpus);
        kprintf("  >> Next check in %llu ms will panic if unresolved\n",
                (unsigned long long)((RCU_STALL_PANIC_TICKS - elapsed) * 1000ULL / TIMER_FREQ));
    }

    return 1;
}
EXPORT_SYMBOL(rcu_check_stall);

/* ── call_rcu() ──────────────────────────────────────────────────── */

void call_rcu(struct rcu_head *head, rcu_callback_t func) {
    if (!head)
        return;

    /* Initialise the callback entry */
    head->next = NULL;
    head->func = func;

    /* Enqueue to the pending list */
    rcu_cb_lock_acquire();
    if (rcu_pending_tail) {
        rcu_pending_tail->next = head;
        rcu_pending_tail = head;
    } else {
        rcu_pending_list = head;
        rcu_pending_tail = head;
    }
    rcu_n_cbs_queued++;
    rcu_cb_lock_release();

    /* If no GP is in progress, start one now */
    rcu_start_gp();
}
EXPORT_SYMBOL(call_rcu);

/* ── rcu_barrier() ───────────────────────────────────────────────── */

void rcu_barrier(void) {
    /*
     * Wait until all RCU callbacks that were queued before this call
     * have been invoked.  We snapshot the current queued count and
     * poll until invoked count reaches or exceeds it.
     *
     * This is a simple polling barrier; a production kernel would use
     * a waitqueue, but for our purposes polling is sufficient since
     * callbacks are invoked within 1 second (next timer tick).
     */
    uint64_t queued_at_start = __atomic_load_n(&rcu_n_cbs_queued, __ATOMIC_ACQUIRE);
    uint64_t deadline = timer_get_ticks() + (TIMER_FREQ * 10); /* 10 second timeout */

    while (__atomic_load_n(&rcu_n_cbs_invoked, __ATOMIC_ACQUIRE) < queued_at_start) {
        if (timer_get_ticks() >= deadline) {
            kprintf("[RCU] rcu_barrier timeout waiting for callbacks "
                    "(queued=%llu, invoked=%llu)\n",
                    (unsigned long long)queued_at_start,
                    (unsigned long long)rcu_n_cbs_invoked);
            break;
        }
        /* Yield to let timer tick process callbacks */
        scheduler_yield();
    }
}
EXPORT_SYMBOL(rcu_barrier);

/* ── synchronize_rcu() ────────────────────────────────────────────────
 *
 * Enhanced version that also processes any pending callbacks when the
 * grace period completes, ensuring call_rcu() users also make progress.
 */
void synchronize_rcu(void) {
    /*
     * If a GP is already in progress, wait for it.
     * Otherwise start one and wait.
     */
    spinlock_acquire(&rcu_gp_lock);
    if (rcu_gp_in_progress) {
        /* Nested GP request — yield and let the current GP finish */
        uint64_t deadline = timer_get_ticks() + RCU_GP_WAIT_MAX_TICKS;
        while (rcu_gp_in_progress) {
            if (timer_get_ticks() >= deadline) {
                /* Avoid infinite loop if something wedged */
                break;
            }
            spinlock_release(&rcu_gp_lock);
            scheduler_yield();
            spinlock_acquire(&rcu_gp_lock);
        }
        spinlock_release(&rcu_gp_lock);
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
    spinlock_release(&rcu_gp_lock);

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
                rcu_dump_stall_info(now - start, 1, 1);  /* send_ipi = 1 */
                kprintf("\n  >> CPU %d is blocking GP %llu (last QS at tick %llu)\n",
                        stalled_cpu,
                        (unsigned long long)rcu_gp_seq,
                        (unsigned long long)rcu_state_percpu[stalled_cpu].last_qs_tick);
            }
        }

        /* Check for hard stall (panic threshold exceeded) */
        if (now >= deadline && (now - start >= RCU_STALL_PANIC_TICKS)) {
            if (now - start >= RCU_STALL_PANIC_TICKS * 2) {
                /* After 2x the panic threshold, really panic */
                rcu_dump_stall_info(now - start, 0, 1);  /* send_ipi = 1 */
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

    /* GP complete — advance callback machinery */
    spinlock_acquire(&rcu_gp_lock);
    rcu_gp_in_progress = 0;
    spinlock_release(&rcu_gp_lock);

    /* Move pending callbacks to done list and invoke them */
    rcu_cb_lock_acquire();
    if (rcu_pending_list) {
        if (rcu_done_list) {
            struct rcu_head *tail = rcu_done_list;
            while (tail->next)
                tail = tail->next;
            tail->next = rcu_pending_list;
        } else {
            rcu_done_list = rcu_pending_list;
        }
        rcu_pending_list = NULL;
        rcu_pending_tail = NULL;
    }
    rcu_cb_lock_release();

    /* Invoke the done callbacks now */
    rcu_invoke_callbacks();

    /* Full memory barrier so all CPUs see the updated pointer */
    __asm__ volatile("mfence" : : : "memory");
}
EXPORT_SYMBOL(synchronize_rcu);

/* ── rcu_nocbs — Offload RCU callbacks to specific CPUs ────────────── */

/* Bitmask of CPUs that have RCU callbacks offloaded (nocb CPUs).
 * On nocb CPUs, call_rcu() callbacks are NOT processed by the main
 * grace-period kthread. Instead, they are handled by a separate
 * nocb kthread that runs on a different CPU.
 * By default, no CPUs are offloaded (bitmask = 0). */
static uint64_t __read_mostly rcu_nocb_cpumask;

/* Set the nocb CPU mask: CPUs in this mask have their RCU callbacks
 * offloaded to a dedicated kthread rather than being handled by the
 * main RCU grace-period machinery.
 * @mask: bitmask of CPU IDs to offload (bit N set = CPU N is nocb). */
static void rcu_nocbs_set_cpumask(uint64_t mask)
{
    rcu_nocb_cpumask = mask;
    kprintf("[RCU] nocbs: CPUs 0x%llX set for callback offloading\n",
            (unsigned long long)mask);
}

/* Check if a given CPU has nocb offloading. */
static int rcu_nocbs_is_enabled(int cpu_id)
{
    if (cpu_id < 0 || cpu_id >= 64) return 0;
    return (rcu_nocb_cpumask >> cpu_id) & 1;
}

/* ── rcu_boost — RCU priority boosting for real-time ───────────────── */

/* When a grace period is blocked by a low-priority CPU that hasn't
 * passed through a quiescent state, rcu_boost can temporarily raise
 * the priority of the blocking task to ensure it makes progress.
 *
 * Boost parameters:
 *   - rcu_boost_priority: RT priority to boost to (default 1)
 *   - rcu_boost_duration: max ticks to hold boosted priority (default = 5 ticks)
 */

/* Current boost priority level (0 = disabled, 1-99 = RT priority) */
static int __read_mostly rcu_boost_priority = 1;

/* Duration to hold boosted priority (in timer ticks) */
static uint64_t rcu_boost_duration = 5;

/* Grace periods that are currently being boosted */
#define RCU_BOOST_MAX_GPS 4
static struct {
    uint64_t gp_seq;
    int      boosting;
    uint64_t start_tick;
} rcu_boost_gps[RCU_BOOST_MAX_GPS];

/* Set RCU priority boosting parameters.
 * @priority: RT priority (1..99) to boost blocking tasks to.
 *            0 disables boosting.
 * @duration_ticks: how many timer ticks to hold the boosted priority. */
static void rcu_boost_set_params(int priority, uint64_t duration_ticks)
{
    rcu_boost_priority = priority;
    rcu_boost_duration = duration_ticks;
    kprintf("[RCU] boost: priority=%d duration=%llu ticks\n",
            priority, (unsigned long long)duration_ticks);
}

/* Initiate boosting for a specific grace period.
 * Called when a stall is detected — we boost any blocked CPU's
 * current process to help it complete its RCU read-side critical section. */
static void rcu_boost_gp(uint64_t gp_seq)
{
    if (rcu_boost_priority <= 0) return; /* boosting disabled */

    for (int i = 0; i < RCU_BOOST_MAX_GPS; i++) {
        if (!rcu_boost_gps[i].boosting || rcu_boost_gps[i].gp_seq == gp_seq) {
            rcu_boost_gps[i].gp_seq = gp_seq;
            rcu_boost_gps[i].boosting = 1;
            rcu_boost_gps[i].start_tick = timer_get_ticks();
            kprintf("[RCU] boost: boosting GP %llu (prio=%d)\n",
                    (unsigned long long)gp_seq, rcu_boost_priority);
            break;
        }
    }
}

/* Check if a specific grace period is currently being boosted.
 * Used by the scheduler to determine if a process should get
 * a priority boost. */
static int rcu_boost_is_active(uint64_t gp_seq)
{
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < RCU_BOOST_MAX_GPS; i++) {
        if (rcu_boost_gps[i].boosting && rcu_boost_gps[i].gp_seq == gp_seq) {
            /* Check if boost duration has expired */
            if (now - rcu_boost_gps[i].start_tick > rcu_boost_duration) {
                rcu_boost_gps[i].boosting = 0;
                return 0;
            }
            return 1;
        }
    }
    return 0;
}

/* Boost the priority of a task that's blocking a grace period.
 * Returns the boosted priority, or 0 if no boost needed. */
static int rcu_boost_get_priority(uint64_t gp_seq)
{
    if (rcu_boost_is_active(gp_seq))
        return rcu_boost_priority;
    return 0;
}

/* ── Initialization ──────────────────────────────────────────────── */

void __init rcu_init(void) {
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        rcu_state_percpu[i].gp_seq = 0;
        rcu_state_percpu[i].last_qs_tick = 0;
    }
    rcu_gp_seq = 0;
    rcu_gp_start_tick = 0;
    rcu_gp_start_seq = 0;
    rcu_gp_in_progress = 0;
    rcu_stall_warning_printed = 0;
    rcu_pending_list = NULL;
    rcu_pending_tail = NULL;
    rcu_done_list = NULL;
    rcu_cb_lock = 0;
    rcu_n_cbs_queued = 0;
    rcu_n_cbs_invoked = 0;

    kprintf("[OK] RCU initialized with call_rcu + stall detection "
            "(warn=%lums, panic=%lums)\n",
            (unsigned long)(RCU_STALL_WARN_TICKS * 1000ULL / TIMER_FREQ),
            (unsigned long)(RCU_STALL_PANIC_TICKS * 1000ULL / TIMER_FREQ));
}

/* ── rcu_synchronize: Wait for all RCU readers to complete ──────────── */
static int rcu_synchronize(void)
{
    /* Call the existing synchronize_rcu() implementation */
    synchronize_rcu();
    kprintf("[rcu] rcu_synchronize: done\n");
    return 0;
}
