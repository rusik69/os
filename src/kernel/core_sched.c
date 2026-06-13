/* core_sched.c — Core scheduling (SMT isolation) infrastructure
 *
 * Core scheduling tracks CPU core siblings (SMT threads) and provides
 * an interface for assigning tasks to share (or not share) a physical core.
 *
 * When SMT is enabled, sibling threads share execution resources.
 * Core scheduling ensures that tasks from different security domains
 * are not co-scheduled on the same core (HT-aware isolation).
 *
 * This is a basic stub providing:
 *   - sched_core_init() — initialise core tracking
 *   - sched_core_siblings(cpu) — return sibling mask for a given CPU
 *   - sched_core_share(cpu1, cpu2) — check if two CPUs share a core
 */

#include "core_sched.h"
#include "smp.h"
#include "printf.h"
#include "string.h"
#include "errno.h"

/* ── Static state ────────────────────────────────────────────────── */

/* Per-CPU sibling group: which logical CPUs share this physical core */
static uint64_t core_sibling_maps[SMP_MAX_CPUS];
static int core_sched_initialised = 0;
static int cpus_per_core = 1;   /* 1 = no SMT, 2 = SMT2, etc. */

/* ── Public API ──────────────────────────────────────────────────── */

void sched_core_init(void)
{
    if (core_sched_initialised)
        return;

    memset(core_sibling_maps, 0, sizeof(core_sibling_maps));

    /* Query SMT topology from SMP subsystem.
     * If smp_topology_detect() is available, use it to build
     * the core sibling maps. Otherwise fall back to identity maps. */
#if defined(CONFIG_SMP) && defined(SMP_TOPOLOGY)
    /* In a full implementation, we would walk ACPI PPTT or CPUID
     * leaf 0xB (Extended Topology Enumeration) to discover
     * the SMT / core / die hierarchy. */
    extern int smp_num_cpus;
    for (int cpu = 0; cpu < smp_num_cpus && cpu < SMP_MAX_CPUS; cpu++) {
        /* Placeholder: default sibling map = this CPU only.
         * ACPI or CPUID-walking code would set more bits. */
        core_sibling_maps[cpu] = (uint64_t)1 << cpu;
    }
    cpus_per_core = 1; /* assume no SMT unless detection says otherwise */
#else
    /* Uniprocessor: only one CPU, no SMT */
    core_sibling_maps[0] = 1;
    cpus_per_core = 1;
#endif

    core_sched_initialised = 1;
    kprintf("[SchedCore] Core scheduling initialised%s\n",
            cpus_per_core > 1 ? " (SMT detected)" : "");
}

/* Return the sibling mask for a given logical CPU.
 * Returns 0 if the CPU is invalid or not initialised. */
uint64_t sched_core_siblings(int cpu)
{
    if (!core_sched_initialised)
        return 0;
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return 0;
    return core_sibling_maps[cpu];
}

/* Check whether two logical CPUs share the same physical core.
 * Returns 1 if they are siblings (same core), 0 otherwise. */
int sched_core_share(int cpu1, int cpu2)
{
    if (!core_sched_initialised)
        return 0;
    if (cpu1 < 0 || cpu1 >= SMP_MAX_CPUS ||
        cpu2 < 0 || cpu2 >= SMP_MAX_CPUS)
        return 0;
    if (cpu1 == cpu2)
        return 1;
    return (core_sibling_maps[cpu1] & ((uint64_t)1 << cpu2)) != 0;
}

/* Mark two CPUs as siblings (for use by topology detection).
 * Returns 0 on success. */
int sched_core_set_sibling(int cpu1, int cpu2)
{
    if (!core_sched_initialised)
        return -EPERM;
    if (cpu1 < 0 || cpu1 >= SMP_MAX_CPUS ||
        cpu2 < 0 || cpu2 >= SMP_MAX_CPUS)
        return -EINVAL;

    core_sibling_maps[cpu1] |= (uint64_t)1 << cpu2;
    core_sibling_maps[cpu2] |= (uint64_t)1 << cpu1;
    /* Count SMT if siblings differ */
    if (cpu1 != cpu2 && cpus_per_core < 2)
        cpus_per_core = 2;
    return 0;
}

/* Return the number of CPUs per core (1 = no SMT, 2 = SMT2, etc.) */
int sched_core_cpus_per_core(void)
{
    return core_sched_initialised ? cpus_per_core : 1;
}

/* Check whether a task may run on a given CPU based on core scheduling.
 * For now, always returns 1 (allow). */
int sched_core_allow(int task_cpu, int target_cpu)
{
    (void)task_cpu;
    (void)target_cpu;
    return 1;
}
