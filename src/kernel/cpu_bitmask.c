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
