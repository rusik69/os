#ifndef CPUHP_H
#define CPUHP_H

#include "types.h"

/*
 * CPU hotplug stub interface.
 *
 * Simple header-only stubs that allow the kernel to register and
 * manage CPU hotplug events. In a full implementation these would
 * interact with APIC, ACPI, and the scheduler.
 */

/* Maximum supported CPU count */
#define CPUHP_MAX_CPUS 16

/* CPU hotplug states */
enum cpuhp_state {
    CPUHP_STATE_OFFLINE = 0,
    CPUHP_STATE_ONLINE  = 1,
    CPUHP_STATE_DEAD    = 2,
};

/* Per-CPU hotplug state table */
extern enum cpuhp_state cpuhp_cpu_state[CPUHP_MAX_CPUS];

/* Initialize the CPU hotplug subsystem */
static inline void cpuhp_init(void) {
    for (int i = 0; i < CPUHP_MAX_CPUS; i++)
        cpuhp_cpu_state[i] = CPUHP_STATE_OFFLINE;
    cpuhp_cpu_state[0] = CPUHP_STATE_ONLINE; /* BSP is always online */
}

/* Bring a CPU online (stub — does nothing in UP configuration) */
static inline int cpuhp_bring_cpu(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS)
        return -1;
    if (cpuhp_cpu_state[cpu_id] == CPUHP_STATE_ONLINE)
        return 0; /* already online */
    cpuhp_cpu_state[cpu_id] = CPUHP_STATE_ONLINE;
    return 0;
}

/* Take a CPU offline (stub) */
static inline int cpuhp_take_cpu_offline(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS)
        return -1;
    if (cpu_id == 0)
        return -1; /* cannot offline BSP */
    cpuhp_cpu_state[cpu_id] = CPUHP_STATE_OFFLINE;
    return 0;
}

/* Query online/offline state */
static inline int cpuhp_is_online(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS)
        return 0;
    return cpuhp_cpu_state[cpu_id] == CPUHP_STATE_ONLINE;
}

#endif /* CPUHP_H */
