#define KERNEL_INTERNAL
#include "irq_affinity.h"
#include "printf.h"
#include "types.h"
#include "string.h"
#include "smp.h"
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
    __atomic_store_n(&irq_affinity[irq], cpu_mask, __ATOMIC_SEQ_CST);
    return 0;
}

uint64_t irq_get_affinity(int irq) {
    if (irq < 0 || irq >= MAX_IRQ) return 0;
    return __atomic_load_n(&irq_affinity[irq], __ATOMIC_SEQ_CST);
}

/* ── Stub: irq_affinity_set ─────────────────────────────── */
int irq_affinity_set(int irq, const void *cpus)
{
    (void)irq;
    (void)cpus;
    kprintf("[irq] irq_affinity_set: not yet implemented\n");
    return 0;
}
/* ── Stub: irq_affinity_get ─────────────────────────────── */
int irq_affinity_get(int irq, void *cpus)
{
    (void)irq;
    (void)cpus;
    kprintf("[irq] irq_affinity_get: not yet implemented\n");
    return 0;
}
