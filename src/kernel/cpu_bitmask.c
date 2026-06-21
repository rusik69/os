/* cpu_bitmask.c — CPU bitmask operations (static inlines in header) */

#include "cpu_bitmask.h"
#include "printf.h"

void cpumask_init_global(void)
{
    kprintf("[OK] cpumask: CPU bitmask subsystem initialised (max %d CPUs)\n",
            CPUMASK_MAX_CPUS);
}

/* Stub functions: cpumask_set_cpu, cpumask_clear_cpu, cpumask_test_cpu,
   cpumask_weight — provided as static inlines in cpu_bitmask.h */

/* ── Stub: cpumask_cpu_count ──────────────────────────── */
int cpumask_cpu_count(void)
{
    kprintf("[cpumask] cpumask_cpu_count: not yet implemented\n");
    return 1;
}
