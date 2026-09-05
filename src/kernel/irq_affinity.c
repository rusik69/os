#define KERNEL_INTERNAL
#include "irq_affinity.h"
#include "printf.h"
#include "types.h"
#include "string.h"
#include "smp.h"
#include "apic.h"
#include "cpu_bitmask.h"
#include "cpuhp.h"
#include "export.h"

#define MAX_IRQ 256

/* irq_affinity[] is accessed from multiple CPUs (or potentially from IRQ
 * context).  Use __atomic_* builtins to guarantee atomic 64-bit loads and
 * stores with full sequential consistency, preventing torn reads/writes and
 * compiler reordering on SMP systems. */
static uint64_t irq_affinity[MAX_IRQ];

/*
 * irq_pick_first_online() — the lowest-numbered ONLINE CPU in @mask.
 * Returns -1 if @mask is empty or contains no online (schedulable) CPU.
 * Used both when programming an IRQ's hardware destination and when
 * migrating IRQs off a CPU that is being taken offline.
 */
static int irq_pick_first_online(uint64_t mask) {
    if (mask == 0)
        return -1;
    for (int cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        if (mask & (1UL << cpu)) {
            if (cpuhp_is_online(cpu))
                return cpu;
        }
    }
    return -1;
}

/* Forward decl — defined below, referenced from irq_affinity_init(). */
static void irq_hotplug_notify(int cpu_id, enum cpuhp_state old_state, enum cpuhp_state new_state);

void irq_affinity_init(void) {
    for (int i = 0; i < MAX_IRQ; i++)
        __atomic_store_n(&irq_affinity[i], 1UL, __ATOMIC_SEQ_CST);

    /* Mirror CPU hotplug so that when a CPU goes offline its IRQs are
     * re-targeted to a CPU still online (see irq_migrate_offline). */
    cpuhp_register_notify(irq_hotplug_notify);

    kprintf("[OK] IRQ affinity subsystem initialized\n");
}

int irq_set_affinity(int irq, uint64_t cpu_mask) {
    if (irq < 0 || irq >= MAX_IRQ) return -1;
    if (cpu_mask == 0) return -1;          /* at least one CPU required */

    __atomic_store_n(&irq_affinity[irq], cpu_mask, __ATOMIC_SEQ_CST);

    /* ── Make the affinity take effect at the hardware level ──────────
     * Pick the first (lowest-numbered) ONLINE CPU in the mask and program
     * the I/O APIC destination to that CPU's APIC ID.  Without this step
     * the stored mask is purely cosmetic — interrupts would keep arriving
     * on whatever CPU the IOAPIC was originally programmed for.  Choosing
     * only an online CPU means an affinity bit pointing at an offlined CPU
     * never has an IRQ routed to hardware that cannot service it. */
    int target_cpu = irq_pick_first_online(cpu_mask);
    if (target_cpu >= 0) {
        uint32_t apic_id = cpu_info_array[target_cpu].apic_id;
        ioapic_set_irq_destination((uint8_t)irq, apic_id);
    }

    return 0;
}

/* ── CPU hotplug: migrate IRQs away from an offlined CPU ──────────── */

/*
 * irq_migrate_offline() — re-target every IRQ currently affinitised to
 * @offline_cpu onto a CPU that remains online.
 *
 * For each IRQ whose mask contains the offlining CPU: clear that bit and
 * reprogram the I/O APIC destination to the lowest-numbered online CPU.
 * If the mask becomes empty (no online CPUs left in it) the IRQ is fallen
 * back to the boot CPU (0) so the interrupt is never left undeliverable.
 * Returns the number of IRQs migrated.
 */
int irq_migrate_offline(int offline_cpu) {
    if (offline_cpu < 0 || offline_cpu >= SMP_MAX_CPUS)
        return 0;

    int migrated = 0;
    for (int irq = 0; irq < MAX_IRQ; irq++) {
        uint64_t mask = irq_get_affinity(irq);
        if (mask == 0)
            continue;
        if (!(mask & (1UL << offline_cpu)))
            continue; /* this IRQ does not target the offlining CPU */

        /* Drop the offlining CPU from the mask and re-target. */
        uint64_t new_mask = mask & ~(1UL << offline_cpu);
        int target = irq_pick_first_online(new_mask);
        if (target < 0) {
            /* No online CPU left in the mask — fall back to the BSP. */
            new_mask = 1UL;
            target = 0;
        }

        __atomic_store_n(&irq_affinity[irq], new_mask, __ATOMIC_SEQ_CST);
        ioapic_set_irq_destination((uint8_t)irq, cpu_info_array[target].apic_id);
        migrated++;
    }

    if (migrated)
        kprintf("[IRQ] migrated %d IRQ(s) off CPU %d to online CPUs\n", migrated, offline_cpu);
    return migrated;
}
EXPORT_SYMBOL(irq_migrate_offline);

/*
 * irq_hotplug_notify() — hotplug notifier: when a CPU transitions from
 * ONLINE to any non-ONLINE state, migrate its IRQs to an online CPU.
 * Runs in cpuhp_notify() context (cpuhp_lock held, interrupts disabled),
 * so it is kept to cheap bookkeeping. */
static void irq_hotplug_notify(int cpu_id, enum cpuhp_state old_state, enum cpuhp_state new_state) {
    if (old_state == CPUHP_STATE_ONLINE && new_state != CPUHP_STATE_ONLINE)
        irq_migrate_offline(cpu_id);
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
