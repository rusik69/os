#include "cpu_topology.h"
#include "printf.h"
#include "kernel.h"
#include "smp.h"

/*
 * CPUID leaf 0xB (Intel x2APIC topology enumeration).
 *
 * We probe levels 0 (SMT/thread) and 1 (core) to derive:
 *   thread_id = x2APIC ID & ((1U << level0_width) - 1)
 *   core_id   = (x2APIC ID >> level0_width) & ((1U << level1_width) - 1)
 *   package_id = x2APIC ID >> (level0_width + level1_width)
 */

static int get_cpuid_leaf_b(int level, int *eax, int *ebx)
{
    int unused_edx;
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(unused_edx), "=d"(unused_edx)
                     : "a"(0xB), "c"(level)
                     : "memory");
    return unused_edx;  /* EDX = x2APIC ID */
}

struct cpu_topology cpu_topology_get(void)
{
    struct cpu_topology topo;

    /*
     * Probe level 0 (SMT / thread).
     * EAX[4:0] = number of bits to shift for next level
     * EBX[15:0] = number of logical processors at this level
     */
    int eax, ebx;
    get_cpuid_leaf_b(0, &eax, &ebx);
    int level0_width = eax & 0x1F;

    /*
     * Probe level 1 (core).
     */
    int x2apic_id = get_cpuid_leaf_b(1, &eax, &ebx);
    int level1_width = eax & 0x1F;

    /*
     * Derive topology IDs from the x2APIC ID.
     *   thread_id    = bits [0 .. level0_width-1]
     *   core_id      = bits [level0_width .. level0_width+level1_width-1]
     *   package_id   = bits above
     */
    int shift = level0_width;
    int core_mask = (1U << level1_width) - 1;

    topo.thread_id   = x2apic_id & ((1U << level0_width) - 1);
    topo.core_id     = (x2apic_id >> shift) & core_mask;
    topo.package_id  = x2apic_id >> (level0_width + level1_width);

    return topo;
}

uint64_t cpu_topology_siblings(void)
{
    uint64_t mask = 0;

    for (int i = 0; i < smp_cpu_count && i < 64; i++) {
        if (i == smp_get_cpu_id()) {
            mask |= (1ULL << i);
            continue;
        }
        /* For simplicity, all online CPUs are package siblings
         * in a single-package system. */
        mask |= (1ULL << i);
    }
    return mask;
}

uint64_t cpu_topology_cores(void)
{
    uint64_t mask = 0;

    for (int i = 0; i < smp_cpu_count && i < 64; i++) {
        if (i == smp_get_cpu_id()) {
            mask |= (1ULL << i);
            continue;
        }
        /* In a single-socket system with SMT, core siblings share
         * the same package+core.  For now treat all as core siblings. */
        mask |= (1ULL << i);
    }
    return mask;
}

void cpu_topology_detect(void)
{
    struct cpu_topology topo = cpu_topology_get();
    (void)topo;
}

void cpu_topology_init(void)
{
    cpu_topology_detect();
    kprintf("[OK] cpu_topology: x2APIC CPUID 0B topology initialised\n");
}

/* ── NUMA topology ─────────────────────────────────────────────────── */

/*
 * NUMA topology state.
 *
 * On most systems without ACPI SRAT (or a vendor-specific NUMA interface),
 * we default to a single NUMA node (node 0) that contains all online CPUs.
 * This is correct for all single-socket consumer hardware, and still
 * functional (though not NUMA-optimised) on multi-socket servers.
 *
 * When ACPI table parsing is added, numa_init() should walk the SRAT
 * and SLIT to populate these structures with real platform data.
 */

/* Number of NUMA nodes detected */
int numa_node_count = 0;

/* Bitmask of CPUs per NUMA node */
uint64_t numa_node_cpus[NUMA_MAX_NODES] = {0};

/* CPU-to-NUMA-node mapping */
int cpu_to_numa_node[64] = {0};

/*
 * Distribute a CPU across all existing NUMA nodes (round-robin).
 * For instance, CPU 0 → node 0, CPU 1 → node 1, CPU 2 → node 0, etc.
 * This is a reasonable fallback when ACPI SRAT is unavailable but the
 * system has multiple packages detected via CPUID leaf 0xB.
 */
static void numa_round_robin(void)
{
    if (numa_node_count < 2) {
        /* Single node — all CPUs go to node 0 (already default) */
        for (int c = 0; c < smp_cpu_count; c++) {
            numa_node_cpus[0] |= (1ULL << c);
            cpu_to_numa_node[c] = 0;
        }
        return;
    }

    /* Round-robin: CPU i → node (i % numa_node_count) */
    for (int c = 0; c < smp_cpu_count; c++) {
        int node = c % numa_node_count;
        numa_node_cpus[node] |= (1ULL << c);
        cpu_to_numa_node[c] = node;
    }

    kprintf("[NUMA] Round-robin CPU→node distribution (%d nodes, %d CPUs)\n",
            numa_node_count, smp_cpu_count);
}

void numa_init(void)
{
    if (numa_node_count > 0) {
        kprintf("[NUMA] already initialised (%d nodes)\n", numa_node_count);
        return;
    }

    /* ── NUMA node discovery ───────────────────────────────────────
     *
     * For now we determine the node count from the package topology.
     * Each physical package (socket) is assigned to a separate NUMA
     * node on most x86-64 systems.  CPUID leaf 0xB exposes the package
     * ID; we count unique package IDs among online CPUs.
     *
     * In the future, this should be replaced by ACPI SRAT parsing for
     * accurate node boundaries and memory affinity information.
     */

    /* Try to detect number of packages from CPU topology */
    int packages_seen = 0;

    /*
     * Walk all online CPUs.  On the BSP we can only probe our own
     * topology; for a simple heuristic we assume the BSP's topology
     * is representative and that package_id uniquely identifies a NUMA
     * node.  On SMP systems each AP would report its own package_id;
     * here we simulate by scanning all CPUs known to smp.
     *
     * Note: In the current implementation, cpu_topology_get() always
     * queries the local CPU via CPUID, so it returns the BSP's values
     * on a pre-SMP boot.  For a real NUMA-capable kernel, this loop
     * would IPI each AP to retrieve its topology.  We fall back to a
     * simple default for now.
     */
    struct cpu_topology topo = cpu_topology_get();

    /* Each unique package_id → one NUMA node */
    if (topo.package_id >= 0 && topo.package_id < 64) {
        packages_seen++;
    }

    /* Ensure at least 1 NUMA node */
    if (packages_seen < 1)
        packages_seen = 1;

    /* Clamp to NUMA_MAX_NODES */
    if (packages_seen > NUMA_MAX_NODES)
        packages_seen = NUMA_MAX_NODES;

    numa_node_count = packages_seen;

    /* ── Assign CPUs to NUMA nodes ───────────────────────────────── */
    if (numa_node_count == 1) {
        /* Single-node: all CPUs belong to node 0 */
        for (int c = 0; c < smp_cpu_count; c++) {
            numa_node_cpus[0] |= (1ULL << c);
            cpu_to_numa_node[c] = 0;
        }
    } else {
        /* Multi-node: distribute CPUs round-robin across nodes */
        numa_round_robin();
    }

    /* ── Log NUMA topology ────────────────────────────────────────── */
    kprintf("[NUMA] Initialised: %d node(s), %d CPU(s)\n",
            numa_node_count, smp_cpu_count);
    for (int n = 0; n < numa_node_count; n++) {
        kprintf("[NUMA]   Node %d:", n);
        uint64_t mask = numa_node_cpus[n];
        int first = 1;
        for (int c = 0; c < 64 && c < smp_cpu_count; c++) {
            if (mask & (1ULL << c)) {
                kprintf("%s CPU%d", first ? "" : ",", c);
                first = 0;
            }
        }
        kprintf("\n");
    }
}

/*
 * Given a CPU index, return its NUMA node.
 * Returns 0 on invalid input.
 */
int numa_node_of_cpu(int cpu)
{
    if (cpu < 0 || cpu >= 64) return 0;
    if (numa_node_count == 0) {
        /* Not initialised — default to node 0 */
        return 0;
    }
    return cpu_to_numa_node[cpu];
}

/*
 * Return the bitmask of CPUs belonging to a NUMA node.
 * Returns 0 on invalid input.
 */
uint64_t numa_cpus_of_node(int node)
{
    if (node < 0 || node >= NUMA_MAX_NODES) return 0;
    if (numa_node_count == 0) {
        /* Not initialised — return all CPUs as node 0 */
        uint64_t all = 0;
        for (int c = 0; c < smp_cpu_count; c++)
            all |= (1ULL << c);
        return all;
    }
    return numa_node_cpus[node];
}

/*
 * Check if a given CPU belongs to the specified NUMA node.
 */
int numa_cpu_is_on_node(int cpu, int node)
{
    if (node < 0 || node >= NUMA_MAX_NODES) return 0;
    if (cpu < 0 || cpu >= 64) return 0;
    if (numa_node_count == 0) {
        /* Not initialised — all CPUs are on node 0 */
        return (node == 0);
    }
    return (cpu_to_numa_node[cpu] == node);
}

/*
 * Find the first online CPU on the given NUMA node, or -1 if none.
 * This is used by the scheduler to pick a home-node CPU for a task.
 */
int numa_first_cpu_on_node(int node)
{
    if (node < 0 || node >= NUMA_MAX_NODES) return -EINVAL;
    uint64_t mask = numa_node_cpus[node];
    if (mask == 0) return -EINVAL;

    /* Find lowest set bit */
    int cpu = 0;
    while ((mask & 1ULL) == 0 && cpu < 64) {
        mask >>= 1;
        cpu++;
    }
    return (cpu < smp_cpu_count) ? cpu : -1;
}

/*
 * Return the NUMA node of the current CPU.
 * Defaults to 0 if NUMA is not initialised.
 */
int numa_home_node(void)
{
    extern int smp_get_cpu_id(void);
    int cpu = smp_get_cpu_id();
    return numa_node_of_cpu(cpu);
}

/*
 * Return the relative distance between two NUMA nodes.
 * Uses ACPI SLIT conventions: 10 = same node, 20 = remote node.
 * If either node is out of range, returns 20 (maximum distance).
 */
unsigned int numa_distance(int node_a, int node_b)
{
    if (node_a < 0 || node_a >= NUMA_MAX_NODES ||
        node_b < 0 || node_b >= NUMA_MAX_NODES)
        return 20;
    if (node_a == node_b)
        return 10;  /* fast local access */
    return 20;      /* remote — default distance */
}

/* ── Stub: cpu_topology_get_package ─────────────────────────────── */
int cpu_topology_get_package(int cpu)
{
    (void)cpu;
    kprintf("[cpu_topology] cpu_topology_get_package: not yet implemented\n");
    return 0;
}
/* ── Stub: cpu_topology_get_core ─────────────────────────────── */
int cpu_topology_get_core(int cpu)
{
    (void)cpu;
    kprintf("[cpu_topology] cpu_topology_get_core: not yet implemented\n");
    return 0;
}
/* ── Stub: cpu_topology_get_numa ─────────────────────────────── */
int cpu_topology_get_numa(int cpu)
{
    (void)cpu;
    kprintf("[cpu_topology] cpu_topology_get_numa: not yet implemented\n");
    return 0;
}
