#ifndef FAULT_H
#define FAULT_H

/* Register the page fault handler (ISR 14). Call once after idt_init(). */
void fault_init(void);

/* Kernel panic — prints message, register state, then halts.
 * Use via the PANIC() macro for file/line info. */
void kpanic(const char *fmt, ...) __attribute__((noreturn));

#define PANIC(msg, ...) \
    kpanic("PANIC at %s:%d: " msg, __FILE__, __LINE__, ##__VA_ARGS__)

#endif
