#ifndef CPU_TOPOLOGY_H
#define CPU_TOPOLOGY_H

#include "types.h"

/*
 * x2APIC CPU topology detection via CPUID leaf 0xB (Intel).
 *
 * CPUID leaf 0xB returns:
 *   EAX[4:0]  = number of bits to shift APIC ID for next level
 *   EBX[15:0] = number of logical processors at this level
 *   ECX[7:0]  = level (0 = SMT, 1 = core, 2+ = package)
 *   EDX       = x2APIC ID of current CPU
 *
 * We probe levels 0 and 1 to extract thread_id, core_id, and package_id.
 */

struct cpu_topology {
    int package_id;
    int core_id;
    int thread_id;
};

/*
 * cpu_topology_get() - Retrieve topology for the calling CPU.
 * Returns a fully populated cpu_topology structure.
 */
struct cpu_topology cpu_topology_get(void);

/*
 * cpu_topology_siblings() - Return a bitmask of all CPUs that share
 * at least the same package (i.e. are siblings / hyperthreads within a core,
 * or cores within a package).  Bit i is set if CPU i is a sibling.
 * Returns 0 if not available.
 */
uint64_t cpu_topology_siblings(void);

/*
 * cpu_topology_cores() - Return a bitmask of all CPUs that share
 * the same core (i.e. SMT siblings / hyperthreads).  Bit i is set
 * if CPU i is on the same physical core.
 * Returns 0 if not available.
 */
uint64_t cpu_topology_cores(void);

/*
 * cpu_topology_detect() - Probe CPUID 0xB on the current CPU and
 * cache the topology data.  Called once during boot on each CPU.
 */
void cpu_topology_detect(void);

/*
 * cpu_topology_init() - Global init.  Prints [OK] on completion.
 */
void cpu_topology_init(void);

#endif /* CPU_TOPOLOGY_H */
