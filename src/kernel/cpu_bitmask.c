/* cpu_bitmask.c — CPU bitmask operations (static inlines in header) */

#include "cpu_bitmask.h"
#include "printf.h"

/**
 * cpumask_init_global - Initialise the CPU bitmask subsystem
 *
 * Called once during boot to announce that the CPU bitmask operations
 * are available.  The actual inline functions (cpumask_set_cpu,
 * cpumask_clear_cpu, cpumask_test_cpu) are defined in cpu_bitmask.h.
 */
static void cpumask_init_global(void)
{
    kprintf("[OK] cpumask: CPU bitmask subsystem initialised (max %d CPUs)\n",
            CPUMASK_MAX_CPUS);
}

/* Stub functions: cpumask_set_cpu, cpumask_clear_cpu, cpumask_test_cpu,
   cpumask_weight — provided as static inlines in cpu_bitmask.h */

/* ── Stub: cpumask_cpu_count ──────────────────────────── */
/**
 * cpumask_cpu_count - Return the number of available CPUs
 *
 * Not yet implemented — returns 1 as a placeholder.
 *
 * Return: 1 (placeholder; real implementation pending)
 */
static int cpumask_cpu_count(void)
{
    kprintf("[cpumask] cpumask_cpu_count: not yet implemented\n");
    return 1;
}
