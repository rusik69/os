#include "cpuset.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "kernel.h"

/* Per-process CPU affinity table. */
#define CPUSET_TABLE_SIZE  64

static cpuset_t cpuset_table[CPUSET_TABLE_SIZE];
static int      cpuset_used[CPUSET_TABLE_SIZE];
static int      cpuset_initialised;

/* Global cpuset representing all available CPUs */
static cpuset_t cpuset_all;

/* ── CPU cgroup controller state ──────────────────────────────────── */
static struct cpu_cgroup cpu_cgroups[CPU_CGROUP_MAX];
static int cpu_cgroup_initialised = 0;

void cpuset_init(void)
{
    if (cpuset_initialised)
        return;

    memset(cpuset_table, 0, sizeof(cpuset_table));
    memset(cpuset_used, 0, sizeof(cpuset_used));

    /* Set all CPUs in the global "all" set */
#if CPUSET_MAX_CPUS >= 64
    cpuset_all.bits = ~0ULL;
#else
    cpuset_all.bits = (1ULL << CPUSET_MAX_CPUS) - 1;
#endif

    cpuset_initialised = 1;

    kprintf("[OK] cpuset: CPU affinity initialised (%d CPUs max)\n", CPUSET_MAX_CPUS);

    /* Initialize CPU cgroup controller */
    memset(cpu_cgroups, 0, sizeof(cpu_cgroups));
    cpu_cgroup_initialised = 1;
    kprintf("[OK] cpu_cgroup: %d slots available\n", CPU_CGROUP_MAX - 1);
}

/* Return a reference to the "all CPUs" cpuset */
const cpuset_t *cpuset_get_all(void)
{
    return &cpuset_all;
}

static int cpuset_index(uint32_t pid)
{
    return pid % CPUSET_TABLE_SIZE;
}

int sched_setaffinity(uint32_t pid, const cpuset_t *cpuset)
{
    if (!cpuset_initialised)
        return -ENOSYS;
    if (!cpuset)
        return -EFAULT;

    /* Must set at least one CPU */
    if (cpuset_empty(cpuset))
        return -EINVAL;

    /* Validate no bits beyond max CPUs */
    if (CPUSET_MAX_CPUS < 64 && (cpuset->bits & ~((1ULL << CPUSET_MAX_CPUS) - 1)))
        return -EINVAL;

    struct process *proc = process_get_by_pid(pid);
    if (!proc)
        return -ESRCH;

    int idx = cpuset_index(pid);

    cpuset_table[idx] = *cpuset;
    cpuset_used[idx] = 1;

    /* Store a compact representation in the process struct */
    proc->cpu_affinity = (uint8_t)(cpuset->bits & 0xFF);

    return 0;
}

int sched_getaffinity(uint32_t pid, cpuset_t *cpuset)
{
    if (!cpuset_initialised)
        return -ENOSYS;
    if (!cpuset)
        return -EFAULT;

    struct process *proc = process_get_by_pid(pid);
    if (!proc)
        return -ESRCH;

    int idx = cpuset_index(pid);

    if (cpuset_used[idx]) {
        *cpuset = cpuset_table[idx];
    } else {
        /* Default: all CPUs */
        *cpuset = cpuset_all;
    }

    return 0;
}

/* ── CPU cgroup controller (Item 40) ────────────────────────────────
 *
 * A minimal per-group CPU usage tracker with throttling.  Groups are
 * identified by a small integer ID.  Group 0 is the implicit default
 * (unlimited, never throttled).  Allocatable groups are 1..CPU_CGROUP_MAX-1.
 *
 * The scheduler calls cpu_cgroup_account() on each tick for the running
 * process's cgroup.  If the group's usage exceeds its per-window limit,
 * the group is marked throttled and its processes are skipped in
 * pick_next_task() until cpu_cgroup_reset_window() is called (e.g. by
 * a timer-based daemon or the periodic scheduler_age() hook).
 */

/* Return the cgroup structure for the given ID, or NULL if invalid. */
struct cpu_cgroup *cpu_cgroup_get(int cg_id)
{
    if (cg_id < 0 || cg_id >= CPU_CGROUP_MAX)
        return NULL;
    if (cg_id > 0 && !cpu_cgroups[cg_id].in_use)
        return NULL;
    return &cpu_cgroups[cg_id];
}

/* Allocate a new CPU cgroup slot, returning its ID (1..CPU_CGROUP_MAX-1)
 * or -1 if all slots are exhausted. */
int cpu_cgroup_alloc(void)
{
    if (!cpu_cgroup_initialised)
        return -1;

    for (int i = 1; i < CPU_CGROUP_MAX; i++) {
        if (!cpu_cgroups[i].in_use) {
            cpu_cgroups[i].in_use    = 1;
            cpu_cgroups[i].cg_id     = i;
            cpu_cgroups[i].usage_ticks   = 0;
            cpu_cgroups[i].limit_ticks   = 0;  /* unlimited */
            cpu_cgroups[i].throttled      = 0;
            cpu_cgroups[i].member_count   = 0;
            return i;
        }
    }
    return -1;
}

/* Set the per-window CPU limit for a cgroup (in timer ticks).
 * A limit of 0 means unlimited. */
int cpu_cgroup_set_limit(int cg_id, uint64_t limit_ticks)
{
    struct cpu_cgroup *cg = cpu_cgroup_get(cg_id);
    if (!cg)
        return -EINVAL;
    cg->limit_ticks = limit_ticks;
    /* If the new limit is already exceeded, throttle immediately. */
    if (limit_ticks > 0 && cg->usage_ticks >= limit_ticks)
        cg->throttled = 1;
    else
        cg->throttled = 0;
    return 0;
}

/* Account 'ticks' of CPU time to the given cgroup.  If the group now
 * exceeds its limit, mark it throttled. */
void cpu_cgroup_account(int cg_id, uint64_t ticks)
{
    struct cpu_cgroup *cg = cpu_cgroup_get(cg_id);
    if (!cg)
        return;

    cg->usage_ticks += ticks;

    /* Check throttling (only if a limit is set). */
    if (cg->limit_ticks > 0 && cg->usage_ticks >= cg->limit_ticks)
        cg->throttled = 1;
}

/* Return 1 if the cgroup is currently throttled, 0 otherwise.
 * Group 0 (the default) is never throttled. */
int cpu_cgroup_is_throttled(int cg_id)
{
    if (cg_id <= 0 || cg_id >= CPU_CGROUP_MAX)
        return 0;
    struct cpu_cgroup *cg = cpu_cgroup_get(cg_id);
    if (!cg)
        return 0;
    return cg->throttled;
}

/* Reset usage counters for all cgroups (end of accounting window).
 * Clears throttled status so that groups may run again. */
void cpu_cgroup_reset_window(void)
{
    for (int i = 1; i < CPU_CGROUP_MAX; i++) {
        if (cpu_cgroups[i].in_use) {
            cpu_cgroups[i].usage_ticks = 0;
            cpu_cgroups[i].throttled   = 0;
        }
    }
}
