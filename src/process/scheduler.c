/*
 * Per-CPU scheduler with SMP load balancing
 *
 * Each CPU maintains its own multilevel priority queue (4 levels).
 * Idle CPUs pull tasks from busy CPUs via load_balance().
 * Cross-CPU wakeups use IPI reschedule.
 */
#include "scheduler.h"
#include "process.h"
#include "signal.h"
#include "syscall.h"
#include "io.h"
#include "gdt.h"
#include "vmm.h"
#include "timer.h"
#include "smp.h"
#include "spinlock.h"
#include "apic.h"
#include "string.h"
#include "rtc.h"
#include "nmi_watchdog.h"
#include "cpuhp.h"
#include "cpuidle.h"
#include "sched_deadline.h"
#include "preempt.h"
#include "sysctl.h"
#include "pelt.h"
#include "psi.h"
#include "cpuset.h"
#include "cpu_topology.h"
#include "kpti.h"
#include "page_cache.h"
#include "rseq.h"
#include "process_rlimit.h"
#include "core_sched.h"
#include "nohz.h"

/* 4-level multilevel priority queue: 0 = highest, 3 = lowest */

/* Global lock for cross-CPU operations (load balancing, process table walks) */
static spinlock_t sched_lock = SPINLOCK_INIT;

/* ── Scheduler latency and granularity tunables ────────────────────────
 *
 * These values serve a function analogous to Linux's
 * /proc/sys/kernel/sched_{latency,min_granularity}_ns.  They control the
 * target scheduling latency (the maximum time a runnable task may wait
 * before being scheduled) and the minimum time slice (the smallest
 * quantum any task receives before being preempted).
 *
 * The time_slices[] array is computed from these values so that higher
 * priority levels get larger slices while preserving the target latency
 * bound.  Both knobs are adjustable at runtime via sysctl.
 */
static uint64_t sched_latency_ns      = 6000000ULL;   /*  6 ms target latency */
static uint64_t sched_min_granularity_ns = 1000000ULL; /*  1 ms minimum slice  */

/* Convert nanoseconds to ticks at the timer's frequency.
 * Uses 64-bit arithmetic to avoid overflow: ns * freq / 1e9.
 * freq defaults to 100 (Hz), so 1 tick = 10 ms = 10,000,000 ns. */
static inline uint64_t ns_to_ticks(uint64_t ns) {
    return (ns * (uint64_t)TIMER_FREQ + 999999999ULL) / 1000000000ULL;
}

/* Recompute the per-priority time slices from sched_latency_ns and
 * sched_min_granularity_ns.  Called whenever a tunable is updated. */
static uint16_t computed_slices[SCHED_LEVELS];

static void recompute_time_slices(void) {
    uint64_t lat_ticks = ns_to_ticks(sched_latency_ns);
    uint64_t min_ticks = ns_to_ticks(sched_min_granularity_ns);

    /* Clamp: minimum slice must be at least 1 tick, latency at least 2 ticks */
    if (min_ticks < 1)   min_ticks = 1;
    if (lat_ticks < min_ticks * SCHED_LEVELS)
        lat_ticks = min_ticks * SCHED_LEVELS;

    /* Distribute: higher priority (lower index) gets larger share.
     * Level 0 (highest prio) gets ~40% of the window,
     * level 1 gets ~30%, level 2 gets ~20%, level 3 gets ~10%. */
    static const uint8_t weight[SCHED_LEVELS] = { 40, 30, 20, 10 };
    uint64_t total_weight = 0;
    for (int i = 0; i < SCHED_LEVELS; i++) total_weight += weight[i];

    uint64_t flags;
    spinlock_irqsave_acquire(&sched_lock, &flags);

    for (int i = 0; i < SCHED_LEVELS; i++) {
        uint64_t slice = (lat_ticks * weight[i]) / total_weight;
        if (slice < min_ticks) slice = min_ticks;
        if (slice > 0xFFFF)    slice = 0xFFFF;
        computed_slices[i] = (uint16_t)slice;
    }

    spinlock_irqsave_release(&sched_lock, flags);
}

/* Accessor used throughout the scheduler; replaces direct use of the
 * old static time_slices[] array.  Falls back to a reasonable default
 * if the computed table hasn't been initialised yet. */
static inline uint16_t slice_for_prio(int lvl) {
    uint64_t flags;
    uint16_t val;

    spinlock_irqsave_acquire(&sched_lock, &flags);
    if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
    val = computed_slices[lvl];
    spinlock_irqsave_release(&sched_lock, flags);

    return val;
}

/* ── Sysctl handlers for scheduler latency/granularity ──────────── */

static int sysctl_read_sched_latency(char *buf, int max) {
    int p = 0;
    uint64_t v = sched_latency_ns;
    char tmp[24]; int ti = 0;
    if (v == 0) { tmp[ti++] = '0'; }
    else { while (v) { tmp[ti++] = '0' + (char)(v % 10); v /= 10; } }
    for (int i = ti - 1; i >= 0 && p < max - 1; i--) buf[p++] = tmp[i];
    if (p < max - 1) buf[p++] = '\n';
    buf[p] = '\0';
    return p;
}

static int sysctl_write_sched_latency(const char *buf, int len) {
    uint64_t v = 0;
    for (int i = 0; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (uint64_t)(buf[i] - '0');
    if (v < 100000ULL)    v = 100000ULL;       /* floor: 100 µs */
    if (v > 1000000000ULL) v = 1000000000ULL;   /* ceil:  1 s   */
    sched_latency_ns = v;
    recompute_time_slices();
    return 0;
}

static int sysctl_read_sched_min_granularity(char *buf, int max) {
    int p = 0;
    uint64_t v = sched_min_granularity_ns;
    char tmp[24]; int ti = 0;
    if (v == 0) { tmp[ti++] = '0'; }
    else { while (v) { tmp[ti++] = '0' + (char)(v % 10); v /= 10; } }
    for (int i = ti - 1; i >= 0 && p < max - 1; i--) buf[p++] = tmp[i];
    if (p < max - 1) buf[p++] = '\n';
    buf[p] = '\0';
    return p;
}

static int sysctl_write_sched_min_granularity(const char *buf, int len) {
    uint64_t v = 0;
    for (int i = 0; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (uint64_t)(buf[i] - '0');
    if (v < 50000ULL)     v = 50000ULL;        /* floor: 50 µs  */
    if (v > 100000000ULL)  v = 100000000ULL;    /* ceil: 100 ms  */
    sched_min_granularity_ns = v;
    recompute_time_slices();
    return 0;
}

/* CFS constants */
#define CFS_NICE_0_WEIGHT 1024
#define CFS_WEIGHT_SHIFT 10  /* 1024 = 1<<10 */
#define CFS_VRUNTIME_MAX_DIFF 100000000ULL /* 100ms */

/*
 * Nice-to-weight conversion table (Linux-compatible scale).
 *
 * Each nice level differs from the next by a factor of ~1.25 (5%).
 * Nice 0 has weight 1024; nice -20 has weight ~88761 (~86× CPU share
 * of nice 0); nice +19 has weight 15 (~1/68 of nice 0).
 *
 * The mapping formula: weight = 1024 / (1.25^nice), so that
 * the CPU time ratio between two tasks is weight_a / weight_b.
 */
static const int sched_prio_to_weight[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */  9548,  7620,  6100,  4904,  3906,
    /*  -5 */  3121,  2501,  1991,  1586,  1277,
    /*   0 */  1024,   820,   655,   526,   423,
    /*   5 */   335,   272,   215,   172,   137,
    /*  10 */   110,    87,    70,    56,    45,
    /*  15 */    36,    29,    23,    18,    15,
};

/* Convert POSIX nice value (-20..+19) to CFS scheduling weight.
 * Returns 1024 for nice 0, larger for lower nice (higher priority). */
static inline int nice_to_weight(int nice) {
    if (nice < NICE_MIN) nice = NICE_MIN;
    if (nice > NICE_MAX) nice = NICE_MAX;
    return sched_prio_to_weight[nice - NICE_MIN];
}

/* Return the nice value whose weight is closest to @weight.
 * Used by getpriority / procfs when only the weight is available. */
static inline int weight_to_nice(int weight) {
    if (weight >= sched_prio_to_weight[0])
        return NICE_MIN;
    if (weight <= sched_prio_to_weight[39])
        return NICE_MAX;
    for (int i = 0; i < 39; i++) {
        if (weight >= sched_prio_to_weight[i + 1] &&
            weight <= sched_prio_to_weight[i])
            return NICE_MIN + i;
    }
    return 0;
}

/* ── Autogroup state ──────────────────────────────────────── */
static struct sched_autogroup autogroups[SCHED_AUTOGROUP_MAX];
/* autogroup_count tracks total groups — kept for future use */
static int autogroup_count __attribute__((unused)) = 0;

/* ── Per-CPU helpers ────────────────────────────────────────────────── */
static inline struct cpu_info *this_cpu(void) {
    return get_cpu_info();
}

static inline int cpu_queue_empty(struct cpu_info *ci, int lvl) {
    return ci->queue_head[lvl] == NULL;
}

static inline int cpu_queues_empty(struct cpu_info *ci) {
    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        if (ci->queue_head[lvl]) return 0;
    }
    return 1;
}

/* Count runnable tasks on a given CPU (approximate, no lock needed for stats) */
static __attribute__((unused)) int cpu_nr_runnable(struct cpu_info *ci) {
    int count = 0;
    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        struct process *p = ci->queue_head[lvl];
        while (p) { count++; p = p->next; }
    }
    return count;
}

/* ── Initialization ─────────────────────────────────────────────────── */
void scheduler_init(void) {
    /* Per-CPU queues are zero-initialized by smp_init_bsp.
     * scheduler_enabled is kept 0 until the boot sequence finishes —
     * kernel_main enables it just before entering the idle loop. */
    struct cpu_info *ci = this_cpu();

    /* Initialize the per-CPU deadline runqueue */
    sched_deadline_init_cpu(get_cpu_id());

    /* Sync the per-CPU current_process pointer.
     * process_init() set the global 'current_process' to &process_table[0]
     * (the boot/idle process), but smp_init_bsp() then cleared the
     * per-CPU copy to NULL.  Without this, scheduler_tick() sees NULL
     * via ci->current_process and returns immediately — no quantum is
     * assigned and schedule() is never called, so test tasks (and any
     * other process) never get to run. */
    if (!ci->current_process) {
        ci->current_process = process_get_current();
    }

    /* Pre-compute time slices from the default latency/min-granularity */
    recompute_time_slices();

    /* Register scheduler sysctl tunables */
    sysctl_register("sched_latency_ns",
                    sysctl_read_sched_latency,
                    sysctl_write_sched_latency);
    sysctl_register("sched_min_granularity_ns",
                    sysctl_read_sched_min_granularity,
                    sysctl_write_sched_min_granularity);
}

/* ── Add process to its CPU's runqueue (caller must hold sched_lock) ── */
static void scheduler_add_locked(struct process *proc) {
    if (proc->on_queue) return;

    /* SCHED_DEADLINE tasks are managed by the deadline runqueue, not the
     * general priority queues. */
    if (proc->sched_policy == SCHED_DEADLINE)
        return;

    /* ── NUMA-aware target CPU selection ─────────────────────────────
     *
     * If the process has a NUMA home node (assigned at creation),
     * prefer placing it on a CPU belonging to that node.  This keeps
     * memory accesses local and reduces cross-node traffic.
     *
     * Selection priority:
     *   1. Current CPU, if it belongs to the process's home node
     *   2. Another CPU on the home node if the current CPU is remote
     *   3. Current CPU (fallback — always safe)
     */
    uint32_t cpu_id = get_cpu_id();
    int home = proc->home_node;

    if (home >= 0 && home < NUMA_MAX_NODES) {
        /* Check whether the current CPU is on the desired NUMA node */
        if (!numa_cpu_is_on_node((int)cpu_id, home)) {
            /* Current CPU is remote — try to find a home-node CPU */
            int home_cpu = numa_first_cpu_on_node(home);
            if (home_cpu >= 0 && home_cpu < smp_cpu_count &&
                cpuhp_is_online(home_cpu)) {
                cpu_id = (uint32_t)home_cpu;
            }
            /* If no home-node CPU is online, fall through to current CPU */
        }
    }

    struct cpu_info *ci = &cpu_info_array[cpu_id];

    int lvl = (int)proc->priority;
    if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;

    /* SCHED_IDLE tasks always go to the lowest queue level */
    if (proc->sched_policy == SCHED_IDLE)
        lvl = SCHED_LEVELS - 1;

    proc->next = NULL;
    proc->on_queue = 1;

    /* ── NO_HZ: if the tick was stopped and we're adding a task,
     * restart the tick so this new task gets properly accounted. */
    if (nohz_tick_is_stopped((int)cpu_id)) {
        nohz_tick_restart((int)cpu_id);
    }

    if (!ci->queue_tail[lvl]) {
        ci->queue_head[lvl] = proc;
        ci->queue_tail[lvl] = proc;
    } else {
        ci->queue_tail[lvl]->next = proc;
        ci->queue_tail[lvl] = proc;
    }
}

/* ── Add process to its CPU's runqueue (public API, acquires lock) ─── */
void scheduler_add(struct process *proc) {
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&sched_lock, &irq_flags);
    scheduler_add_locked(proc);
    spinlock_irqsave_release(&sched_lock, irq_flags);
}

/* ── Remove process from its queue ──────────────────────────────────── */
void scheduler_remove(struct process *proc) {
    if (!proc->on_queue) return;

    int lvl = (int)proc->priority;
    if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&sched_lock, &irq_flags);

    /* Search all CPUs for this process (it could be on any queue) */
    for (int cpu = 0; cpu < smp_cpu_count; cpu++) {
        struct cpu_info *ci = &cpu_info_array[cpu];
        struct process *prev = NULL;
        struct process *cur = ci->queue_head[lvl];

        while (cur) {
            if (cur == proc) {
                if (prev) prev->next = cur->next;
                else ci->queue_head[lvl] = cur->next;
                if (cur == ci->queue_tail[lvl])
                    ci->queue_tail[lvl] = prev;
                cur->next = NULL;
                proc->on_queue = 0;
                spinlock_irqsave_release(&sched_lock, irq_flags);
                return;
            }
            prev = cur;
            cur = cur->next;
        }
    }

    proc->on_queue = 0;
    spinlock_irqsave_release(&sched_lock, irq_flags);
}

/* ── Priority change ────────────────────────────────────────────────── */
int scheduler_set_priority(struct process *proc, uint8_t priority) {
    if (!proc || priority >= SCHED_LEVELS) return -1;

    if (proc->state == PROCESS_READY) {
        scheduler_remove(proc);
        proc->priority = priority;
        scheduler_add(proc);
        return 0;
    }

    proc->priority = priority;
    return 0;
}

/* ── Set process nice value and update CFS weight ─────────────────────
 *
 * POSIX setpriority(which, who, prio) should call this function
 * instead of manually updating fields.  It:
 *   1. Clamps @nice to the valid range [-20, +19]
 *   2. Updates proc->nice and proc->sched_weight (CFS weight from table)
 *   3. Maps the nice value to the legacy priority (0-3) for the
 *      multilevel queue (used by RT scheduling and aging)
 *   4. If the process is on the runqueue, re-inserts it so the new
 *      weight takes effect immediately.
 *
 * Returns 0 on success, -1 on error.
 */
int scheduler_set_nice(struct process *proc, int nice) {
    if (!proc) return -1;

    if (nice < NICE_MIN) nice = NICE_MIN;
    if (nice > NICE_MAX) nice = NICE_MAX;

    proc->nice = nice;
    proc->sched_weight = (uint64_t)nice_to_weight(nice);

    /* Map nice → legacy priority level (0..3) for backward compat */
    int new_prio;
    if (nice <= -11)      new_prio = 0;
    else if (nice <= -1)  new_prio = 1;
    else if (nice <=  9)  new_prio = 2;
    else                  new_prio = 3;

    if (proc->state == PROCESS_READY) {
        scheduler_remove(proc);
        proc->priority = (uint8_t)new_prio;
        scheduler_add(proc);
        return 0;
    }

    proc->priority = (uint8_t)new_prio;
    return 0;
}

/* ── Dequeue next runnable process from current CPU ─────────────────── */
static struct process *dequeue_next(void) {
    struct cpu_info *ci = this_cpu();

    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        if (ci->queue_head[lvl]) {
            struct process *p = ci->queue_head[lvl];
            ci->queue_head[lvl] = p->next;
            if (!ci->queue_head[lvl]) ci->queue_tail[lvl] = NULL;
            p->next = NULL;
            p->on_queue = 0;
            return p;
        }
    }
    return NULL;
}

/* ── Load balancing: steal from the busiest CPU (weighted, NUMA-aware) ──── */
static int calculate_cpu_load(struct cpu_info *ci) {
    int load = 0;
    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        struct process *p = ci->queue_head[lvl];
        while (p) {
            load += (int)(p->sched_weight ? p->sched_weight : CFS_NICE_0_WEIGHT);
            p = p->next;
        }
    }
    return load;
}

/*
 * load_balance() - Steal a runnable task from a busy CPU to balance load.
 *
 * Selection strategy (NUMA-aware):
 *   1. Prefer stealing from CPUs on the same NUMA node (lower latency).
 *   2. If no same-node CPU is overloaded, fall back to the busiest CPU
 *      regardless of node.
 *   3. Steal the lowest-priority task (highest priority level number) to
 *      minimise disruption.
 */
static int load_balance(void) {
    struct cpu_info *ci = this_cpu();
    int this_cpu_id = (int)get_cpu_id();
    int this_load = calculate_cpu_load(ci);

    /* Don't steal if we already have significant load */
    if (this_load > CFS_NICE_0_WEIGHT * 2) return 0;

    int this_node = numa_node_of_cpu(this_cpu_id);

    /* Find the busiest CPU, preferring same-NUMA-node CPUs */
    int busiest_cpu = -1;
    int busiest_load = 0;
    int same_node_busiest_cpu = -1;
    int same_node_busiest_load = 0;

    for (int cpu = 0; cpu < smp_cpu_count; cpu++) {
        if (cpu == this_cpu_id) continue;
        struct cpu_info *other = &cpu_info_array[cpu];
        int load = calculate_cpu_load(other);
        int diff = load - this_load;

        if (diff > CFS_NICE_0_WEIGHT) {
            /* Track the busiest overall */
            if (load > busiest_load) {
                busiest_load = load;
                busiest_cpu = cpu;
            }
            /* Track the busiest on the same NUMA node */
            if (numa_node_of_cpu(cpu) == this_node && load > same_node_busiest_load) {
                same_node_busiest_load = load;
                same_node_busiest_cpu = cpu;
            }
        }
    }

    /* Prefer same-node CPU; fall back to busiest CPU on any node */
    if (same_node_busiest_cpu >= 0)
        busiest_cpu = same_node_busiest_cpu;

    if (busiest_cpu < 0) return 0;

    /* Steal the lowest-priority task from the busiest CPU */
    struct cpu_info *busy = &cpu_info_array[busiest_cpu];

    for (int lvl = SCHED_LEVELS - 1; lvl >= 0; lvl--) {
        if (!busy->queue_head[lvl]) continue;

        /* Find the last (tail) process in this priority level */
        struct process *prev = NULL;
        struct process *cur = busy->queue_head[lvl];
        while (cur->next) {
            prev = cur;
            cur = cur->next;
        }

        /* Unlink from busy CPU */
        if (prev) prev->next = NULL;
        else busy->queue_head[lvl] = NULL;
        busy->queue_tail[lvl] = prev;

        cur->next = NULL;
        cur->on_queue = 0;

        /* Add to our queue */
        {
            int our_lvl = (int)cur->priority;
            if (!ci->queue_tail[our_lvl]) {
                ci->queue_head[our_lvl] = cur;
                ci->queue_tail[our_lvl] = cur;
            } else {
                ci->queue_tail[our_lvl]->next = cur;
                ci->queue_tail[our_lvl] = cur;
            }
            cur->on_queue = 1;
        }

        return 1; /* stole one task */
    }

    return 0;
}

/* Forward declaration — defined below (line ~1108), called from scheduler_wake_sleepers */
static void scheduler_wakeup_locked(struct process *proc);
/* Forward declaration — defined below (line ~1174), called from scheduler_tick */
static void update_vruntime(struct process *p, int ticks);

/* ── Main scheduler entry: pick next process, context-switch ────────── */
void schedule(void) {
    struct cpu_info *ci = this_cpu();
    if (!ci->scheduler_enabled) return;

    /* ── Preempt count sanity check ──────────────────────────────────
     * schedule() should never be called while preemption is disabled
     * (preempt_count > 0).  This indicates a missing preempt_enable()
     * or a bug where schedule() was called from an atomic context. */
    if (ci->preempt_count > 0) {
        kprintf("*** BUG: schedule() called with preempt_count=%d ***\n",
                ci->preempt_count);
        kprintf("    CPU=%d, current_process=%s (pid=%u)\n",
                ci->cpu_id,
                ci->current_process && ci->current_process->name
                    ? ci->current_process->name : "?",
                ci->current_process ? (unsigned int)ci->current_process->pid : 0);
        arch_print_backtrace();
        /* Do not panic — this is a recoverable warning in debug mode */
        ci->preempt_count = 0;  /* Reset so we can continue */
    }

    __asm__ volatile("cli");

    /* Clear the reschedule request — we're handling it now. */
    ci->need_resched = 0;

    struct process *current = ci->current_process;

    uint64_t irq_flags;
    struct process *next = NULL;

    /* First, try to pick a SCHED_DEADLINE task (EDF) */
    next = sched_deadline_pick_next();

    if (!next) {
        /* No deadline task available — fall back to class-aware picker.
         * Selection order: RT (SCHED_FIFO/RR) > CFS (SCHED_OTHER/BATCH) > SCHED_IDLE
         * Core scheduling: verify the picked task is compatible with the
         * current CPU's sibling state (same-core cookie check). */
        spinlock_irqsave_acquire(&sched_lock, &irq_flags);
        next = dequeue_next();
        /* ── Core scheduling compatibility check ────────────────
         * If the picked task has a core scheduling cookie and a sibling
         * CPU is running a task with a different cookie, skip this task
         * and put it back.  Try the next task in the queue. */
        if (next && !sched_core_allow(next, (int)ci->cpu_id)) {
            scheduler_add_locked(next);
            next = dequeue_next();
            /* Try one more time with a fallback to load balancing */
        }
        spinlock_irqsave_release(&sched_lock, irq_flags);
    }

    if (!next) {
        /* No tasks — try load balancing */
        spinlock_irqsave_acquire(&sched_lock, &irq_flags);
        int stole = load_balance();
        spinlock_irqsave_release(&sched_lock, irq_flags);

        if (stole) {
            spinlock_irqsave_acquire(&sched_lock, &irq_flags);
            next = dequeue_next();
            /* ── Core scheduling check for stolen tasks ───── */
            if (next && !sched_core_allow(next, (int)ci->cpu_id)) {
                scheduler_add_locked(next);
                next = NULL;
            }
            spinlock_irqsave_release(&sched_lock, irq_flags);
        }

        if (!next) {
            ci->idle_ticks++;
            __asm__ volatile("sti");
            return;
        }
    }
    /* Put current back on queue if still runnable */
    if (current && current->state == PROCESS_RUNNING) {
        current->nivcsw++;  /* preempted — involuntary context switch */
        current->state = PROCESS_READY;
        current->on_cpu = 0; /* no longer executing on this CPU */
        scheduler_add(current);
    } else if (current) {
        current->nvcsw++;   /* yielded or blocked — voluntary context switch */
        current->on_cpu = 0; /* no longer executing */

        /* ── GRUB reclaim: if this is a SCHED_DEADLINE task blocking
         *     voluntarily, capture any unused budget for other deadline
         *     tasks to reclaim. */
        if (current->dl_active) {
            sched_deadline_task_blocked(current);
        }
    }

    next->state = PROCESS_RUNNING;
    next->on_cpu = 1;        /* about to execute on this CPU */
    next->ticks_remaining = slice_for_prio((int)next->priority);
    next->last_run_tick = timer_get_ticks();
    process_set_current(next);

    /* Set kernel stack for syscall/interrupt returns */
    tss_set_rsp0(next->stack_top);
    struct cpu_info *info = get_cpu_info();
    if (info) info->current_kernel_rsp = next->stack_top;

    /* Switch page tables if user process */
    if (next->is_user && next->pml4) {
        vmm_switch_pml4(next->pml4);
    } else {
        vmm_switch_pml4(vmm_get_pml4());
    }

    /* Update KPTI trampoline CR3 values for this process */
    if (kpti_is_active() && next->is_user && next->kpti_state.user_cr3) {
        kpti_trampoline_patch_cr3(0, next->kpti_state.kernel_cr3, next->kpti_state.user_cr3);
    }

    /* Pet the watchdog: we just context-switched, proving the scheduler
     * is making forward progress on this CPU. */
    nmi_watchdog_pet();

    /* ── rseq: update cpu_id for the incoming task ────────────────── */
    if (next->is_user) {
        rseq_update_cpu_id(next);
    }

    /* ── rseq: detect migration and abort critical sections ────────── */
    if (current && current->is_user && current != next) {
        int current_cpu = smp_get_cpu_id();
        rseq_migrate(current, current_cpu, current_cpu);
    }

    /* Per-task stack canary: save the current guard, load the incoming
     * process's canary so its functions check correctly, and restore the
     * saved canary after the context switch so that this schedule()'s own
     * stack frames (and its callers) verify against the process's canary. */
    {
        extern uint64_t __stack_chk_guard;
        volatile uint64_t saved_canary = __stack_chk_guard;
        __stack_chk_guard = next->stack_canary;

        context_switch(current ? &current->context : NULL, next->context);
        __asm__ volatile("sti");

        __stack_chk_guard = saved_canary;
    }
}

void scheduler_yield(void) {
    /* Priority boost: voluntarily yielding processes get a temporary
     * priority bump to discourage busy-waiting and improve interactivity */
    struct process *cur = process_get_current();
    if (cur && cur->priority > 0) {
        cur->priority--; /* boost by one level */
    }

    /* If preemption is disabled (e.g., holding a spinlock), yield is
     * dangerous and could lead to deadlock (nobody else can run to
     * release the lock we need).  Defer the reschedule instead. */
    if (preemptible()) {
        schedule();
    } else {
        set_need_resched();
    }
}

uint64_t scheduler_get_idle_ticks(void) {
    return this_cpu()->idle_ticks;
}

/* ── Timer tick handler ─────────────────────────────────────────────── */
void scheduler_tick(int was_user) {
    struct cpu_info *ci = this_cpu();
    if (!ci->scheduler_enabled) return;

    /* Pet the soft watchdog — this proves timer IRQs are reaching the
     * scheduler, which distinguishes soft lockups (scheduler stuck) from
     * hard lockups (IRQs disabled entirely). */
    nmi_watchdog_soft_pet();

    /* Check for soft lockup (scheduler not invoked despite timer ticks) */
    nmi_watchdog_check_soft();

    struct process *cur = ci->current_process;
    if (!cur || cur->state != PROCESS_RUNNING) return;

    /* Acquire sched_lock to protect process accounting fields that
     * may be read concurrently by other CPUs via schedule(),
     * sys_times(), /proc/self/stat, or the scheduler itself. */
    uint64_t __sched_flags;
    spinlock_irqsave_acquire(&sched_lock, &__sched_flags);

    /* Account CPU time: user time if we interrupted user code,
     * system time if we interrupted kernel code. */
    if (was_user && cur->is_user) {
        cur->utime_ticks++;
        cur->cpu_user++;
    } else {
        cur->stime_ticks++;
        cur->cpu_system++;
    }

    /* Update PELT load tracking: running = 1 (we are on CPU),
     * runnable = 1 (we're the current process and thus runnable). */
    pelt_update(&cur->pelt, 1, 1, timer_get_ticks());

    /* ── RLIMIT_CPU enforcement ──────────────────────────────── */
    {
        uint64_t cpu_sec = cur->rlim_cur[RLIMIT_CPU];
        if (cpu_sec != RLIM_INFINITY && cpu_sec > 0) {
            uint64_t total_ticks = cur->utime_ticks + cur->stime_ticks;
            uint64_t total_sec  = total_ticks / (uint64_t)TIMER_FREQ;
            if (total_sec > cpu_sec) {
                if (cur->cpu_limit_warned_tick == 0) {
                    cur->cpu_limit_warned_tick = timer_get_ticks();
                    signal_send(cur->pid, SIGXCPU);
                } else if (timer_get_ticks() - cur->cpu_limit_warned_tick >=
                           (uint64_t)TIMER_FREQ) {
                    /* Grace period expired — hard kill */
                    signal_send(cur->pid, SIGKILL);
                }
            }
        }
    }

    /* Update CFS vruntime (still under sched_lock) */
    update_vruntime(cur, 1);

    spinlock_irqsave_release(&sched_lock, __sched_flags);

    /* Update PSI CPU pressure: if there are tasks in the runqueue
     * waiting, the CPU is overcommitted and some tasks are stalled. */
    {
        struct runqueue_stats rq_stats;
        scheduler_get_runqueue_stats(ci->cpu_id, &rq_stats);
        /* "some": at least one task is waiting for CPU */
        uint64_t some_ticks = (rq_stats.nr_runnable > 0) ? 1 : 0;
        /* "full": all non-idle tasks on this CPU are waiting.
         * Approximate: if we're running a task and others are waiting,
         * then at least some tasks are stalled but not all.
         * Full stall requires the runqueue to have tasks while
         * nobody is running — that's the idle case which we handle
         * separately via idle_ticks tracking. */
        uint64_t full_ticks = 0;
        psi_update(PSI_RES_CPU, 1, some_ticks, full_ticks);
    }

    /* Account this tick to the process's CPU cgroup (if assigned). */
    if (cur->cpu_cgroup_id > 0)
        cpu_cgroup_account(cur->cpu_cgroup_id, 1);

    /* Handle SCHED_DEADLINE budget accounting */
    if (cur->sched_policy == SCHED_DEADLINE && cur->dl_active) {
        sched_deadline_tick(cur);
        /* If throttled, reschedule immediately to let another task run */
        if (cur->dl_throttled) {
            if (preemptible()) {
                schedule();
            } else {
                set_need_resched();
            }
            return;
        }
    }

    /* Check pending signals */
    if (cur->pending_signals) {
        signal_check();
        cur = ci->current_process;
        if (!cur || cur->state != PROCESS_RUNNING) {
            if (preemptible()) {
                schedule();
            } else {
                set_need_resched();
            }
            return;
        }
    }

    /* First tick: assign quantum without preempting (protected by sched_lock) */
    {
        uint64_t __schf;
        spinlock_irqsave_acquire(&sched_lock, &__schf);
        if (cur->ticks_remaining == 0) {
            if (cur->sched_policy == SCHED_OTHER || cur->sched_policy == SCHED_IDLE) {
                int lvl = (int)cur->priority;
                if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
                cur->ticks_remaining = slice_for_prio(lvl);
            } else if (cur->sched_policy == SCHED_BATCH) {
                /* SCHED_BATCH: longer timeslices, lower priority (level 3) */
                cur->ticks_remaining = slice_for_prio(3) * 3;
            } else if (cur->sched_policy == SCHED_DEADLINE) {
                /* SCHED_DEADLINE: managed by CBS budget, not time-slice ticks */
                cur->ticks_remaining = 1; /* don't trigger reschedule on expiry */
            } else {
                /* SCHED_FIFO / SCHED_RR: use maximum quantum for priority level */
                int lvl = (int)cur->priority;
                if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
                cur->ticks_remaining = slice_for_prio(lvl) * 2;
            }
            spinlock_irqsave_release(&sched_lock, __schf);
            return;
        }
        spinlock_irqsave_release(&sched_lock, __schf);
    }

    {
        uint64_t __schg;
        spinlock_irqsave_acquire(&sched_lock, &__schg);
        cur->ticks_remaining--;

        if (cur->ticks_remaining == 0) {
            if (cur->sched_policy == SCHED_FIFO) {
                /* SCHED_FIFO: replenish quantum, don't preempt */
                int lvl = (int)cur->priority;
                if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
                cur->ticks_remaining = slice_for_prio(lvl);
                spinlock_irqsave_release(&sched_lock, __schg);
            } else if (cur->sched_policy == SCHED_RR) {
                /* SCHED_RR: rotate to end of queue on slice expiry */
                int lvl = (int)cur->priority;
                if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
                cur->ticks_remaining = slice_for_prio(lvl);
                spinlock_irqsave_release(&sched_lock, __schg);
                if (preemptible()) {
                    schedule();
                } else {
                    set_need_resched();
                }
            } else if (cur->sched_policy == SCHED_DEADLINE) {
                /* SCHED_DEADLINE: never preempt based on time-slice expiry;
                 * budget is managed by sched_deadline_tick() above. */
                cur->ticks_remaining = 1; /* keep running unless throttled */
                spinlock_irqsave_release(&sched_lock, __schg);
            } else {
                /* SCHED_OTHER / SCHED_BATCH / SCHED_IDLE: preempt on quantum expiry.
                 * If the kernel is not preemptible right now (e.g., in a
                 * spinlock critical section), defer the reschedule by
                 * setting need_resched.  The actual context switch will
                 * happen at the next preempt_enable() or preemption point. */
                spinlock_irqsave_release(&sched_lock, __schg);
                if (preemptible()) {
                    schedule();
                } else {
                    set_need_resched();
                }
            }
        } else {
            spinlock_irqsave_release(&sched_lock, __schg);
        }
    }

    /* Periodically check for deadline task replenishment */
    if (cur->dl_active || (timer_get_ticks() & 0x3) == 0) {
        sched_deadline_replenish();
    }

    /* Periodic background writeback: flush dirty pages every ~1 second.
     * Only on CPU 0 to avoid thundering-herd flushes. */
    if (get_cpu_id() == 0) {
        page_cache_writeback_background();
    }
}

/* ── Aging: boost starved processes (using RTC for precision) ──── */
void scheduler_age(void) {
    extern uint64_t rtc_get_ticks(void);
    uint64_t now = rtc_get_ticks();
    struct process *table = process_get_table();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&sched_lock, &irq_flags);

    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process *p = &table[i];
        if (p->state != PROCESS_READY) continue;
        if (p->priority == 0) continue;
        if (p->last_run_tick == 0) continue;
        uint64_t age_threshold = 1000;
        if (now - p->last_run_tick > age_threshold) {
            /* Remove, boost, re-add */
            scheduler_remove(p);
            p->priority--;
            p->last_run_tick = now;
            scheduler_add(p);
        }
    }

    spinlock_irqsave_release(&sched_lock, irq_flags);
}

/* ── Wake blocked processes whose sleep timer expired ───────────────── */
void scheduler_wake_sleepers(void) {
    uint64_t now = timer_get_ticks();
    struct process *table = process_get_table();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&sched_lock, &irq_flags);

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_BLOCKED && table[i].sleep_until > 0 &&
            now >= table[i].sleep_until) {
            table[i].sleep_until = 0;
            table[i].state = PROCESS_READY;

            /* Apply CFS sleeper fairness before adding to runqueue */
            scheduler_wakeup_locked(&table[i]);
        }
    }

    spinlock_irqsave_release(&sched_lock, irq_flags);
}

/* ── Scheduler statistics ────────────────────────────────────── */

static struct sched_stats sched_stats_data;

void scheduler_stats_inc_ctx_switch(void) { sched_stats_data.context_switches++; }
void scheduler_stats_inc_preempt(void)    { sched_stats_data.preemptions++; }
void scheduler_stats_inc_yield(void)      { sched_stats_data.yields++; }

void scheduler_get_stats(struct sched_stats *stats) {
    if (!stats) return;
    stats->context_switches = sched_stats_data.context_switches;
    stats->preemptions = sched_stats_data.preemptions;
    stats->yields = sched_stats_data.yields;
    stats->idle_ticks_total = sched_stats_data.idle_ticks_total;
}

/* ── Per-CPU runqueue statistics ─────────────────────────────── */

void scheduler_get_runqueue_stats(int cpu, struct runqueue_stats *s) {
    if (!s || cpu < 0 || cpu >= smp_cpu_count) return;
    struct cpu_info *ci = &cpu_info_array[cpu];
    memset(s, 0, sizeof(*s));

    int total_load = 0;
    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        struct process *p = ci->queue_head[lvl];
        while (p) {
            s->nr_runnable++;
            s->prio_distribution[lvl]++;
            /* Load weight: each runnable process contributes (4 - priority) */
            total_load += (SCHED_LEVELS - lvl);
            p = p->next;
        }
    }
    s->load_weight = total_load;

    /* Count processes in various states from the process table */
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_RUNNING) s->nr_running++;
        if (table[i].state == PROCESS_BLOCKED) s->nr_uninterruptible++;
    }
}

/* ── Autogroup implementation ───────────────────────────────── */

int sched_autogroup_get(int session_id) {
    /* Session ID maps to autogroup index via simple hash */
    int target = session_id % SCHED_AUTOGROUP_MAX;
    if (autogroups[target].id == session_id) return target;

    /* Find existing group */
    for (int i = 0; i < SCHED_AUTOGROUP_MAX; i++) {
        if (autogroups[i].id == session_id) return i;
    }

    /* Create new group */
    for (int i = 0; i < SCHED_AUTOGROUP_MAX; i++) {
        if (autogroups[i].id == 0 || autogroups[i].member_count == 0) {
            autogroups[i].id = session_id;
            autogroups[i].vruntime = 0;
            autogroups[i].member_count = 0;
            return i;
        }
    }
    return -1;
}

void sched_autogroup_assign(struct process *proc, int group_id) {
    if (group_id < 0 || group_id >= SCHED_AUTOGROUP_MAX) return;
    if (proc->sched_autogroup_id >= 0 && proc->sched_autogroup_id < SCHED_AUTOGROUP_MAX) {
        if (autogroups[proc->sched_autogroup_id].member_count > 0)
            autogroups[proc->sched_autogroup_id].member_count--;
    }
    proc->sched_autogroup_id = group_id;
    if (group_id >= 0)
        autogroups[group_id].member_count++;
}

uint64_t sched_autogroup_max_vruntime(int group_id) {
    if (group_id < 0 || group_id >= SCHED_AUTOGROUP_MAX) return 0;
    uint64_t max = 0;
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED &&
            table[i].sched_autogroup_id == group_id &&
            table[i].vruntime > max)
            max = table[i].vruntime;
    }
    return max;
}

/* ── Scheduling class helpers ─────────────────────────────────
 *
 * Return 1 if the policy is a real-time class (SCHED_FIFO or SCHED_RR).
 * These tasks have priority over all non-RT tasks and are selected
 * by static priority level rather than vruntime.
 */
static inline int sched_rt_policy(uint8_t policy) {
    return policy == SCHED_FIFO || policy == SCHED_RR;
}

static inline int sched_cfs_policy(uint8_t policy) {
    return policy == SCHED_OTHER || policy == SCHED_BATCH;
}

static inline int sched_idle_policy(uint8_t policy) {
    return policy == SCHED_IDLE;
}

/* Return a numeric class rank for ordering: 0 = deadline (picked separately),
 * 1 = RT, 2 = CFS, 3 = Idle.  Lower number = higher priority. */
static inline int sched_class_rank(uint8_t policy) {
    if (sched_rt_policy(policy))  return 1;
    if (sched_cfs_policy(policy)) return 2;
    if (sched_idle_policy(policy)) return 3;
    return 4; /* unknown — treated as lowest */
}

/* ── CFS minimum vruntime tracking ────────────────────────────
 *
 * Track the minimum vruntime across all runnable tasks on this
 * CPU's runqueue.  Used by scheduler_wakeup() to apply sleeper
 * fairness: prevent long-sleeping tasks from getting an excessive
 * vruntime advantage that would let them monopolise the CPU.
 *
 * The offset below limits how far below min_vruntime a waking
 * task is allowed to go — this prevents CPU-bound bursts after
 * prolonged sleeps while still giving a modest advantage to
 * tasks that have been waiting a long time.
 */
#define CFS_MIN_VRUNTIME_OFFSET 2000000ULL  /* 2 ms worth of credit */

/* Update the minimum vruntime by scanning the current CPU's queue.
 * Called after adding/removing tasks and after context switches. */
static void cfs_update_min_vruntime(void) {
    struct cpu_info *ci = this_cpu();
    uint64_t min_vruntime = UINT64_MAX;

    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        struct process *p = ci->queue_head[lvl];
        while (p) {
            if (p->vruntime < min_vruntime)
                min_vruntime = p->vruntime;
            p = p->next;
        }
    }

    /* If there's a running process, also include its vruntime */
    struct process *cur = ci->current_process;
    if (cur && cur->state == PROCESS_RUNNING && cur->vruntime < min_vruntime)
        min_vruntime = cur->vruntime;

    ci->cfs_min_vruntime = (min_vruntime == UINT64_MAX) ? 0 : min_vruntime;
}

/* ── CFS sleeper fairness ─────────────────────────────────────
 *
 * Called when a blocked process is about to be woken up and added
 * to the runqueue.  Adjusts the task's vruntime to prevent it from
 * monopolising the CPU after a long sleep.
 *
 * The rule:
 *   - If the task's vruntime is >= the current minimum, keep it as-is
 *     (it wasn't at an advantage before sleeping).
 *   - If the task's vruntime is below the minimum by more than the
 *     offset, cap it so that it gets at most CFS_MIN_VRUNTIME_OFFSET
 *     of advantage.  This prevents CPU-bound bursts after prolonged
 *     sleeps while still providing a modest scheduling incentive for
 *     I/O-bound interactive tasks.
 *
 * NOTE: Caller must hold sched_lock (IRQs disabled).
 */
static void scheduler_wakeup_locked(struct process *proc) {
    if (!proc) return;

    /* ── Wakee flips heuristic ──────────────────────────────────────
     *
     * Detect one-sided communication patterns (e.g. a producer that
     * always wakes the same consumer).  When the waker keeps waking
     * the *same* task we prefer to keep them on the same CPU (affine
     * wakeup) for cache warmth and reduced migration cost.  When the
     * waker flips between many different tasks we fall back to a
     * regular wakeup.
     *
     * The flip counter is incremented whenever the waker's last_wakee
     * differs from the current wakee, and decays by half roughly every
     * 32 timer ticks (~320 ms at 100 Hz) so that transient patterns
     * don't skew the decision permanently.
     */
    struct process *waker = this_cpu()->current_process;
    if (waker && waker != proc) {
        uint64_t now = timer_get_ticks();

        /* Periodic decay: halve the counter every ~32 ticks */
        if (now - waker->wakee_flip_tick > 32) {
            waker->wakee_flip_cnt >>= 1;
            waker->wakee_flip_tick = now;
        }

        if (waker->last_wakee != proc) {
            /* The waker switched to a different wakee — record the flip */
            if (waker->wakee_flip_cnt < 65535)
                waker->wakee_flip_cnt++;
            waker->last_wakee = proc;
        }

        /* Low flip count (< 2 flips) means the waker keeps waking the
         * same task → place the wakee on the waker's CPU for cache
         * warmth.  High flip count means scattered communication →
         * leave the wakee where it is (no cross-CPU pull).
         */
        if (waker->wakee_flip_cnt < 2) {
            /* The current task (waker) is about to add the wakee to
             * this CPU's runqueue via scheduler_add() below, so the
             * wakeup is already affine.  No special action needed. */
        }
    }

    /* Ensure min_vruntime is up-to-date (sched_lock held by caller) */
    cfs_update_min_vruntime();
    uint64_t min_vr = this_cpu()->cfs_min_vruntime;

    /* If the task's vruntime is far behind, cap it */
    if (proc->vruntime + CFS_MIN_VRUNTIME_OFFSET < min_vr) {
        proc->vruntime = min_vr - CFS_MIN_VRUNTIME_OFFSET;
    }

    /* Add to the runqueue (caller holds sched_lock, use locked variant) */
    scheduler_add_locked(proc);
}

/* Public API: acquires sched_lock, applies CFS sleeper fairness, adds to runqueue. */
void scheduler_wakeup(struct process *proc) {
    uint64_t flags;
    spinlock_irqsave_acquire(&sched_lock, &flags);
    scheduler_wakeup_locked(proc);
    spinlock_irqsave_release(&sched_lock, flags);
}
void update_vruntime(struct process *p, int ticks) {
    if (!p) return;
    uint64_t weight = p->sched_weight ? p->sched_weight : CFS_NICE_0_WEIGHT;
    /* vruntime += time_slice * 1000000 / weight */
    uint64_t increment = (uint64_t)ticks * 1000000ULL * CFS_NICE_0_WEIGHT / weight;
    p->vruntime += increment;
}

/* ── Pick next process with scheduling class hierarchy ────────
 *
 * Selects the next process to run respecting the Linux scheduling
 * class hierarchy:
 *   1. SCHED_DEADLINE (EDF) — picked separately via sched_deadline_pick_next()
 *   2. RT (SCHED_FIFO / SCHED_RR) — highest static priority first,
 *      then by vruntime as tiebreaker
 *   3. CFS (SCHED_OTHER / SCHED_BATCH) — lowest vruntime
 *   4. SCHED_IDLE — lowest vruntime, but only when nothing RT/CFS wants CPU
 *
 * Returns the selected process (already unlinked from the queue)
 * or NULL if no runnable task exists.
 */
/* ── CPU hotplug: migrate all tasks away from a CPU ──────── */

/*
 * Migrate every runnable process from @from_cpu to other online CPUs.
 * This is called by cpuhp_migrate_tasks_away() while holding the hotplug
 * lock. It iterates the per-CPU runqueue of @from_cpu, removes each
 * process, and distributes them across the remaining online CPUs.
 *
 * Returns the number of tasks migrated (0 if none).
 *
 * NOTE: This function expects cpuhp_lock to already be held by the caller
 * and interrupts to be disabled. It does NOT acquire sched_lock itself
 * since the hotplug lock serialises all scheduling modifications during
 * the offline transition.
 */
int scheduler_migrate_tasks_from(int from_cpu)
{
    struct cpu_info *ci;
    int migrated = 0;

    if (from_cpu < 0 || from_cpu >= smp_cpu_count)
        return -1;

    ci = &cpu_info_array[from_cpu];

    /* ── Count available destination CPUs ────────────────────────── */
    int dst_cpus[SMP_MAX_CPUS];
    int num_dst = 0;

    for (int i = 0; i < smp_cpu_count; i++) {
        if (i != from_cpu && cpuhp_is_online(i)) {
            dst_cpus[num_dst++] = i;
        }
    }

    if (num_dst == 0)
        return 0; /* no targets — nothing to do */

    /* ── Migrate tasks from each priority level ──────────────────── */
    int spread = (int)timer_get_ticks(); /* pseudo-random spread seed */

    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        struct process *p = ci->queue_head[lvl];

        while (p) {
            struct process *next = p->next;

            /* Pick a destination CPU (round-robin) */
            int dst = dst_cpus[spread % num_dst];
            spread++;

            /* Unlink from source queue */
            p->next = NULL;
            p->on_queue = 0;

            /* Link into destination queue */
            struct cpu_info *dci = &cpu_info_array[dst];
            int dlvl = (int)p->priority;
            if (dlvl < 0 || dlvl >= SCHED_LEVELS)
                dlvl = 1;

            if (!dci->queue_tail[dlvl]) {
                dci->queue_head[dlvl] = p;
                dci->queue_tail[dlvl] = p;
            } else {
                dci->queue_tail[dlvl]->next = p;
                dci->queue_tail[dlvl] = p;
            }
            p->on_queue = 1;
            migrated++;

            p = next;
        }

        /* Clear the source queue for this level */
        ci->queue_head[lvl] = NULL;
        ci->queue_tail[lvl] = NULL;
    }

    return migrated;
}

/* ── Module exports ──────────────────────────────────────────────── */
#include "export.h"
EXPORT_SYMBOL(scheduler_yield);
