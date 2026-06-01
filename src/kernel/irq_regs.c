#include "irq_regs.h"
#include "printf.h"
#include "kernel.h"
#include "smp.h"
#include "spinlock.h"

/*
 * Per-CPU IRQ register save/restore.
 *
 * Each CPU maintains a small stack of saved pt_regs pointers so that
 * nested interrupts can be handled without losing context.
 */

#define SMP_MAX_CPUS  16

static struct irq_regs_cpu irq_regs_per_cpu[SMP_MAX_CPUS];
static spinlock_t irq_regs_lock;

static int current_cpu(void)
{
    /* In a real system this would read the local APIC ID or MSR.
     * For now we treat it as CPU 0. */
    return 0;
}

struct pt_regs *set_irq_regs(struct pt_regs *regs)
{
    int cpu = current_cpu();
    struct irq_regs_cpu *cpu_state = &irq_regs_per_cpu[cpu];
    struct pt_regs *previous = NULL;

    spinlock_acquire(&irq_regs_lock);

    if (cpu_state->depth > 0)
        previous = cpu_state->frames[cpu_state->depth - 1];

    if (cpu_state->depth < IRQ_REGS_MAX_FRAMES) {
        cpu_state->frames[cpu_state->depth++] = regs;
    }

    spinlock_release(&irq_regs_lock);
    return previous;
}

struct pt_regs *get_irq_regs(void)
{
    int cpu = current_cpu();
    struct irq_regs_cpu *cpu_state = &irq_regs_per_cpu[cpu];
    struct pt_regs *regs = NULL;

    spinlock_acquire(&irq_regs_lock);
    if (cpu_state->depth > 0)
        regs = cpu_state->frames[cpu_state->depth - 1];
    spinlock_release(&irq_regs_lock);

    return regs;
}

void irq_regs_init(void)
{
    spinlock_init(&irq_regs_lock);

    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        irq_regs_per_cpu[i].depth = 0;
        for (int j = 0; j < IRQ_REGS_MAX_FRAMES; j++)
            irq_regs_per_cpu[i].frames[j] = NULL;
    }

    kprintf("[OK] irq_regs: IRQ register save/restore initialised\n");
}
