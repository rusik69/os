#ifndef PANIC_H
#define PANIC_H

#include "types.h"

/* Kernel oops/panic handler with register dump, backtrace, and diagnostics.
 *
 * Use:
 *   PANIC("Something terrible happened: %s", reason);
 *   BUG_ON(condition);
 *   WARN_ON(condition);
 *   dump_stack();
 *
 * A BUG_ON hit calls dump_regs + dump_stack then halts.
 * A WARN_ON hit calls dump_stack but continues.
 * A PANIC halts the system after printing.
 */

/* Panic the kernel — never returns */
__attribute__((noreturn))
void panic(const char *fmt, ...);

/* Dump stack backtrace */
void dump_stack(void);

/* Dump CPU registers to console */
void dump_regs(void);

/* Set up exception stack for better backtraces */
void panic_init(void);

/* Helper macros */
#define BUG_ON(cond) do { \
    if (__builtin_expect(!!(cond), 0)) { \
        kprintf("BUG at %s:%d: " #cond "\n", __FILE__, __LINE__); \
        dump_stack(); \
        cli(); for(;;) hlt(); \
    } \
} while(0)

#define WARN_ON(cond) do { \
    if (__builtin_expect(!!(cond), 0)) { \
        kprintf("WARN at %s:%d: " #cond "\n", __FILE__, __LINE__); \
        dump_stack(); \
    } \
} while(0)

#endif
