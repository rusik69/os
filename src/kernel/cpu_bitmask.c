#include "cpu_bitmask.h"
#include "printf.h"
#include "kernel.h"

/*
 * cpu_bitmask.c – runtime initialisation.
 * All bitmask operations are inlines in the header; this file exists
 * to register the subsystem.
 */

void cpu_bitmask_init(void)
{
    kprintf("[OK] cpu_bitmask: CPU bitmask operations initialised (max %d CPUs)\n",
            CPUMASK_MAX_CPUS);
}

/* ── Stub: cpumask_set_cpu ─────────────────────────────── */
int cpumask_set_cpu(int cpu, void *mask)
{
    (void)cpu;
    (void)mask;
    kprintf("[cpumask] cpumask_set_cpu: not yet implemented\n");
    return 0;
}
/* ── Stub: cpumask_clear_cpu ─────────────────────────────── */
int cpumask_clear_cpu(int cpu, void *mask)
{
    (void)cpu;
    (void)mask;
    kprintf("[cpumask] cpumask_clear_cpu: not yet implemented\n");
    return 0;
}
/* ── Stub: cpumask_test_cpu ─────────────────────────────── */
int cpumask_test_cpu(int cpu, const void *mask)
{
    (void)cpu;
    (void)mask;
    kprintf("[cpumask] cpumask_test_cpu: not yet implemented\n");
    return 0;
}
/* ── Stub: cpumask_weight ─────────────────────────────── */
int cpumask_weight(const void *mask)
{
    (void)mask;
    kprintf("[cpumask] cpumask_weight: not yet implemented\n");
    return 0;
}
