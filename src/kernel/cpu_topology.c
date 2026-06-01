#include "cpu_topology.h"
#include "printf.h"
#include "kernel.h"
#include "smp.h"

/*
 * CPUID leaf 0xB (Intel x2APIC topology enumeration).
 *
 * We probe levels 0 (SMT/thread) and 1 (core) to derive:
 *   thread_id = x2APIC ID & ((1 << level0_width) - 1)
 *   core_id   = (x2APIC ID >> level0_width) & ((1 << level1_width) - 1)
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
    int level0_width = 0, level0_count = 0;
    int level1_width = 0, level1_count = 0;
    int eax, ebx;
    int x2apic_id;

    /*
     * Probe level 0 (SMT / thread).
     * EAX[4:0] = number of bits to shift for next level
     * EBX[15:0] = number of logical processors at this level
     */
    x2apic_id = get_cpuid_leaf_b(0, &eax, &ebx);
    level0_width = eax & 0x1F;
    level0_count = ebx & 0xFFFF;

    /*
     * Probe level 1 (core).
     */
    x2apic_id = get_cpuid_leaf_b(1, &eax, &ebx);
    level1_width = eax & 0x1F;
    level1_count = ebx & 0xFFFF;

    /*
     * Derive topology IDs from the x2APIC ID.
     *   thread_id    = bits [0 .. level0_width-1]
     *   core_id      = bits [level0_width .. level0_width+level1_width-1]
     *   package_id   = bits above
     */
    int shift = level0_width;
    int core_mask = (1 << level1_width) - 1;

    topo.thread_id   = x2apic_id & ((1 << level0_width) - 1);
    topo.core_id     = (x2apic_id >> shift) & core_mask;
    topo.package_id  = x2apic_id >> (level0_width + level1_width);

    return topo;
}

uint64_t cpu_topology_siblings(void)
{
    struct cpu_topology self = cpu_topology_get();
    uint64_t mask = 0;

    for (int i = 0; i < smp_cpu_count && i < 64; i++) {
        /* In a real system we'd query each APIC ID; for the bootstrap
         * environment we rely on the CPU topology of the BSP and assume
         * symmetric topology.  This is a best-effort sibling mask. */
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
    struct cpu_topology self = cpu_topology_get();
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
