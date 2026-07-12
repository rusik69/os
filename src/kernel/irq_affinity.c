#define KERNEL_INTERNAL
#include "irq_affinity.h"
#include "printf.h"
#include "types.h"
#include "string.h"
#include "smp.h"
#include "apic.h"
#include "cpu_bitmask.h"

#define MAX_IRQ 256

/* irq_affinity[] is accessed from multiple CPUs (or potentially from IRQ
 * context).  Use __atomic_* builtins to guarantee atomic 64-bit loads and
 * stores with full sequential consistency, preventing torn reads/writes and
 * compiler reordering on SMP systems. */
static uint64_t irq_affinity[MAX_IRQ];

void irq_affinity_init(void) {
    for (int i = 0; i < MAX_IRQ; i++)
        __atomic_store_n(&irq_affinity[i], 1UL, __ATOMIC_SEQ_CST);
    kprintf("[OK] IRQ affinity subsystem initialized\n");
}

int irq_set_affinity(int irq, uint64_t cpu_mask) {
    if (irq < 0 || irq >= MAX_IRQ) return -1;
    if (cpu_mask == 0) return -1;          /* at least one CPU required */

    __atomic_store_n(&irq_affinity[irq], cpu_mask, __ATOMIC_SEQ_CST);

    /* ── Make the affinity take effect at the hardware level ──────────
     * Pick the first (lowest-numbered) CPU in the mask and program the
     * I/O APIC destination to that CPU's APIC ID.  Without this step the
     * stored mask is purely cosmetic — interrupts would keep arriving on
     * whatever CPU the IOAPIC was originally programmed for. */
    int target_cpu = __builtin_ctzll(cpu_mask);
    if (target_cpu < SMP_MAX_CPUS) {
        uint32_t apic_id = cpu_info_array[target_cpu].apic_id;
        ioapic_set_irq_destination((uint8_t)irq, apic_id);
    }

    return 0;
}

uint64_t irq_get_affinity(int irq) {
    if (irq < 0 || irq >= MAX_IRQ) return 0;
    return __atomic_load_n(&irq_affinity[irq], __ATOMIC_SEQ_CST);
}

int irq_affinity_set(int irq, const struct cpumask *cpus)
{
    if (!cpus) return -1;
    return irq_set_affinity(irq, cpus->bits);
}

int irq_affinity_get(int irq, struct cpumask *cpus)
{
    if (!cpus) return -1;
    cpus->bits = irq_get_affinity(irq);
    return 0;
}
