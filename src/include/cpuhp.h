#ifndef CPUHP_H
#define CPUHP_H

#include "spinlock.h"
#include "types.h"

/*
 * CPU hotplug interface.
 *
 * Provides per-CPU state management with proper task migration.
 * The real implementation lives in src/kernel/cpu.c.
 */

/* Maximum supported CPU count */
#define CPUHP_MAX_CPUS 16

/* CPU hotplug states — in increasing order of "aliveness".
 *
 * Between the OFFLINE and ONLINE terminals live the intermediate states a CPU
 * passes through while coming up or going down.  Each intermediate state owns
 * one bring-up step and one tear-down step (see cpu.c: cpuhp_steps[]).
 * cpuhp_is_online()/cpuhp_online_count() treat only CPUHP_STATE_ONLINE as
 * fully "up and schedulable", so a CPU mid-transition is never counted.
 */
enum cpuhp_state {
    CPUHP_STATE_DEAD = 0,    /* CPU physically removed / not present */
    CPUHP_STATE_OFFLINE = 1, /* CPU present but not schedulable */
    CPUHP_STATE_PMM = 2,     /* per-CPU PMM hot cache rebuilt */
    CPUHP_STATE_SLAB = 3,    /* per-CPU slab cache rebuilt */
    CPUHP_STATE_IRQWORK = 4, /* per-CPU irq_work state ready */
    CPUHP_STATE_MIGRATE = 5, /* task migration pending (take-down) */
    CPUHP_STATE_DRAIN = 6,   /* pending ops drained (take-down) */
    CPUHP_STATE_SCHED = 7,   /* scheduler disabled (take-down) */
    CPUHP_STATE_ONLINE = 8,  /* CPU fully up and accepting tasks */
};

/* Hotplug return codes */
#define CPUHP_OK 0
#define CPUHP_ERR_INVAL -1 /* bad CPU id */
#define CPUHP_ERR_BUSY -2  /* cannot offline: tasks refused to migrate */
#define CPUHP_ERR_BSP -3   /* cannot offline the boot CPU */

/* Notifier callback type for CPU hotplug state changes */
typedef void (*cpuhp_notify_fn)(void);

/* Per-CPU hotplug state table (defined in smp.c) */
extern enum cpuhp_state cpuhp_cpu_state[CPUHP_MAX_CPUS];

/* Lock protecting hotplug state transitions */
extern spinlock_t __cacheline_aligned cpuhp_lock;

/*
 * ── Initialization ─────────────────────────────────────────────
 */

/* Initialize CPU hotplug subsystem. Called once during boot (from smp.c). */
void cpuhp_init(void);

/*
 * ── State transitions ──────────────────────────────────────────
 */

/*
 * Bring a CPU online.
 * If the CPU was previously offline, this transitions it to CPUHP_STATE_ONLINE.
 * Returns 0 on success, negative on error.
 */
int cpuhp_bring_cpu(int cpu_id);

/*
 * Take a CPU offline.
 * If the CPU is currently online, this migrates all runnable tasks away
 * and transitions it to CPUHP_STATE_OFFLINE. Cannot offline the BSP (CPU 0).
 * Returns 0 on success, negative on error.
 */
int cpuhp_take_cpu_offline(int cpu_id);

/*
 * ── Query helpers ──────────────────────────────────────────────
 */

/* Check whether a given CPU is online (schedulable). */
int cpuhp_is_online(int cpu_id);

/* Get the number of currently online CPUs (excluding offline ones). */
int cpuhp_online_count(void);

/*
 * ── SMP integration ────────────────────────────────────────────
 */

/*
 * Called by smp_cpu_disable() to begin taking a CPU offline.
 * Migrates all tasks from @cpu_id to other online CPUs.
 * Returns 0 on success, negative on error.
 */
int cpuhp_migrate_tasks_away(int cpu_id);

/*
 * Notify hotplug listeners that a CPU state changed.
 * Called internally after state transitions.
 */
void cpuhp_notify(void);

/*
 * Register a callback invoked on CPU hotplug state changes.
 * Maximum CPUHP_NOTIFIER_MAX callbacks can be registered.
 * Returns 0 on success, -1 if the table is full.
 */
int cpuhp_register_notify(cpuhp_notify_fn fn);

#endif /* CPUHP_H */
