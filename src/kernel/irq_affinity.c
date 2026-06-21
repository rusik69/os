#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "irq_affinity.h"
#include "string.h"
#include "smp.h"
#define MAX_IRQ 256
static uint64_t irq_affinity[MAX_IRQ];
void irq_affinity_init(void) {
    for (int i = 0; i < MAX_IRQ; i++) irq_affinity[i] = 1; /* CPU 0 only */
    kprintf("[OK] IRQ affinity subsystem initialized\n");
}
int irq_set_affinity(int irq, uint64_t cpu_mask) {
    if (irq < 0 || irq >= MAX_IRQ) return -1;
    irq_affinity[irq] = cpu_mask;
    return 0;
}
uint64_t irq_get_affinity(int irq) {
    if (irq < 0 || irq >= MAX_IRQ) return 0;
    return irq_affinity[irq];
}

/* ── Stub: irq_affinity_set ─────────────────────────────── */
int irq_affinity_set(int irq, const void *cpus)
{
    (void)irq;
    (void)cpus;
    kprintf("[irq] irq_affinity_set: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: irq_affinity_get ─────────────────────────────── */
int irq_affinity_get(int irq, void *cpus)
{
    (void)irq;
    (void)cpus;
    kprintf("[irq] irq_affinity_get: not yet implemented\n");
    return -ENOSYS;
}
