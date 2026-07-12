#ifndef IRQ_AFFINITY_H
#define IRQ_AFFINITY_H
#include "types.h"
#include "cpu_bitmask.h"
void irq_affinity_init(void);
int irq_set_affinity(int irq, uint64_t cpu_mask);
uint64_t irq_get_affinity(int irq);
int irq_affinity_set(int irq, const struct cpumask *cpus);
int irq_affinity_get(int irq, struct cpumask *cpus);
#endif
