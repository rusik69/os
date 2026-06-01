#ifndef FAULT_H
#define FAULT_H

#include "types.h"

/* Register the page fault handler (ISR 14). Call once after idt_init(). */
void fault_init(void);

/* Kernel panic — prints message, register state, then halts.
 * Use via the PANIC() macro for file/line info. */
void kpanic(const char *fmt, ...) __attribute__((noreturn));
void arch_print_backtrace(void);

#define PANIC(msg, ...) \
    kpanic("PANIC at %s:%d: " msg, __FILE__, __LINE__, ##__VA_ARGS__)

/* Kernel stack depth check: verify RSP is within the current process's kernel stack */
int check_kernel_stack_depth(void);

/* Page fault tracing: when enabled, print detailed info for every page fault */
extern int page_fault_trace;
void page_fault_trace_enable(int enable);

#endif
