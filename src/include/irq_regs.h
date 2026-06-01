#ifndef IRQ_REGS_H
#define IRQ_REGS_H

#include "types.h"

/*
 * IRQ register save/restore infrastructure.
 *
 * Provides a per-CPU structure for saving processor registers at IRQ
 * entry time, allowing nested interrupt handling and reliable stack
 * unwinding.  Based on the Linux irq_regs concept.
 *
 * The saved register state is a snapshot of the interrupted context
 * (general-purpose registers, segment selectors, RFLAGS, RIP, RSP).
 */

/* Maximum number of saved register frames per CPU */
#define IRQ_REGS_MAX_FRAMES    8

/* Saved x86-64 register context */
struct pt_regs {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/* Per-CPU IRQ register stack */
struct irq_regs_cpu {
    struct pt_regs *frames[IRQ_REGS_MAX_FRAMES];
    int             depth;
};

/*
 * set_irq_regs  - Save current register state @regs onto the per-CPU
 *                 register stack.  Returns the previously saved set
 *                 (or NULL if this is the first level).
 */
struct pt_regs *set_irq_regs(struct pt_regs *regs);

/*
 * get_irq_regs  - Return the most recently saved register state on
 *                 the current CPU, or NULL if not in IRQ context.
 */
struct pt_regs *get_irq_regs(void);

/*
 * irq_regs_init  - Initialise the IRQ register infrastructure.
 */
void irq_regs_init(void);

#endif /* IRQ_REGS_H */
