#ifndef IRQ_REGS_H
#define IRQ_REGS_H

#include "types.h"

/*
 * IRQ register save/restore infrastructure with per-CPU IRQ stack
 * and stack overflow detection.
 *
 * Provides a per-CPU structure for saving processor registers at IRQ
 * entry time, allowing nested interrupt handling and reliable stack
 * unwinding.  Based on the Linux irq_regs concept.
 *
 * Each CPU has a dedicated IRQ stack (IRQ_STACK_SIZE bytes) so that
 * interrupt handlers do not run on the interrupted task's kernel stack.
 * This prevents stack overflow from interrupt nesting and guards
 * against corrupting the task's context.
 *
 * The saved register state is a snapshot of the interrupted context
 * (general-purpose registers, segment selectors, RFLAGS, RIP, RSP).
 */

/* Maximum number of saved register frames per CPU */
#define IRQ_REGS_MAX_FRAMES    8

/* Size of per-CPU IRQ stack (must be page-aligned) */
#define IRQ_STACK_SIZE        16384   /* 16 KB per CPU */

/* Magic value written at the bottom of each IRQ stack for overflow detection */
#define IRQ_STACK_MAGIC       0xD15AB1EDDEADC0DEULL

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
 * Per-CPU IRQ stack tracking structure.
 * Records the boundaries of the dedicated IRQ stack so that
 * stack-use checks can be performed at IRQ entry/exit.
 */
struct irq_stack_info {
    uint64_t stack_bottom;   /* lowest address (start of allocated pages) */
    uint64_t stack_top;      /* highest address (top of page) */
    uint64_t stack_watermark;/* lowest RSP ever recorded on this stack */
    uint64_t magic;          /* IRQ_STACK_MAGIC if initialized */
    int      cpu_id;
};

/*
 * set_irq_regs  - Save current register state @regs onto the per-CPU
 *                 register stack.  Returns the previously saved set
 *                 (or NULL if this is the first level).
 *                 Also performs stack overflow and nesting checks.
 */
struct pt_regs *set_irq_regs(struct pt_regs *regs);

/*
 * get_irq_regs  - Return the most recently saved register state on
 *                 the current CPU, or NULL if not in IRQ context.
 */
struct pt_regs *get_irq_regs(void);

/*
 * irq_regs_init  - Initialise the IRQ register infrastructure.
 *                  Allocates per-CPU IRQ stacks.  Must be called after
 *                  PMM is available and before any IRQs are enabled.
 */
void irq_regs_init(void);

/*
 * irq_stack_switch  - Switch to the per-CPU IRQ stack if currently
 *                     running on the interrupted task's kernel stack.
 *                     Returns the previous stack pointer so it can be
 *                     restored on IRQ exit.
 *                     Returns 0 if already on the IRQ stack (nested).
 */
uint64_t irq_stack_switch(void);

/*
 * irq_stack_restore  - Return to the original stack after IRQ handling.
 *                      Pass the value returned by irq_stack_switch().
 */
void irq_stack_restore(uint64_t prev_rsp);

/*
 * irq_stack_check  - Verify the current stack is valid and not overflown.
 *                    Prints a warning if something looks wrong.
 *                    Returns 0 on success, non-zero on error.
 */
int irq_stack_check(void);

/*
 * in_irq_context  - Returns non-zero if the current CPU is currently
 *                   handling an interrupt (i.e., we're in IRQ context
 *                   rather than process context).
 */
static inline int in_irq_context(void)
{
    return get_irq_regs() != NULL;
}

/*
 * in_irq_stack  - Returns non-zero if the current RSP is within the
 *                 per-CPU IRQ stack range.
 */
int in_irq_stack(void);

/*
 * irq_nesting_depth  - Returns the current interrupt nesting depth
 *                      for the current CPU (0 = not in IRQ context).
 */
int irq_nesting_depth(void);

#endif /* IRQ_REGS_H */
