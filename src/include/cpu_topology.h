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

/* ── NUMA topology ─────────────────────────────────────────────────────
 *
 * Maximum number of NUMA nodes we support.  Most desktop / server systems
 * have at most 8 NUMA nodes; the value 16 provides headroom for large
 * AMD EPYC / Intel Xeon systems.
 */
#define NUMA_MAX_NODES 16

/*
 * numa_init() - Discover NUMA topology.
 *
 * Defaults to a single NUMA node (node 0) containing all online CPUs.
 * In the future this should parse ACPI SRAT (System Resource Affinity
 * Table) to discover the real node topology.
 */
void numa_init(void);

/*
 * numa_node_count - Number of NUMA nodes detected.
 */
extern int numa_node_count;

/*
 * numa_node_cpus[node] - Bitmask of CPUs belonging to NUMA @node.
 * CPU i is in node @node if (numa_node_cpus[@node] & (1ULL << i)) != 0.
 */
extern uint64_t numa_node_cpus[NUMA_MAX_NODES];

/*
 * cpu_to_numa_node[cpu] - NUMA node ID for a given CPU.
 */
extern int cpu_to_numa_node[64];

/*
 * numa_node_of_cpu(cpu) - Return the NUMA node that @cpu belongs to.
 * Returns 0 if @cpu is out of range or mapping is not initialized.
 */
int numa_node_of_cpu(int cpu);

/*
 * numa_cpus_of_node(node) - Return the bitmask of CPUs in NUMA @node.
 * Returns 0 if @node is out of range.
 */
uint64_t numa_cpus_of_node(int node);

/*
 * numa_home_node() - Return the NUMA node of the current CPU.
 */
int numa_home_node(void);

/*
 * numa_distance(node_a, node_b) - Return relative distance between two
 * NUMA nodes.  For a uniform-memory system this is 10 for same node and
 * 20 for remote nodes (matching ACPI SLIT conventions).  Returns 20
 * if either node is out of range.
 */
unsigned int numa_distance(int node_a, int node_b);

/*
 * numa_cpu_is_on_node(cpu, node) - Check whether a CPU belongs to a
 * specific NUMA node.  Returns 1 if yes, 0 if no.
 */
int numa_cpu_is_on_node(int cpu, int node);

/*
 * numa_first_cpu_on_node(node) - Return the first (lowest-index) CPU
 * belonging to @node, or -1 if the node has no CPUs.
 */
int numa_first_cpu_on_node(int node);

#endif /* CPU_TOPOLOGY_H */
