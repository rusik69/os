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
 * A BUG_ON hit calls dump_regs + dump_stack then panics.
 * A WARN_ON hit calls dump_stack but continues.
 * A PANIC halts the system after printing.
 */

/* Panic the kernel — never returns */
__attribute__((noreturn)) __printf(1, 2)
void panic(const char *fmt, ...);

/* Dump stack backtrace */
void dump_stack(void);

/* Dump CPU registers to console */
void dump_regs(void);

/* Set up exception stack for better backtraces */
void panic_init(void);

/*
 * Panic timeout in seconds.  When panic() is called, instead of hanging
 * forever, the system will attempt to reset after this many seconds.
 * Set to 0 to disable the timeout (infinite hang, legacy behaviour).
 * Default: 30 seconds.
 */
extern int panic_timeout;

/* oops_count: running count of non-fatal kernel warnings (WARN_ON hits).
 * Read via /sys/kernel/oops_count for monitoring. */
extern uint64_t oops_count;

/*
 * Set the panic timeout.  Pass 0 to disable timeout-based reset
 * (system will hang forever on panic as before).
 */
void panic_set_timeout(int seconds);

/*
 * Return the estimated TSC frequency in Hz (calibrated during panic_init).
 * Returns 0 if not yet calibrated.  Useful for other subsystems that
 * need a rough TSC-based time measurement.
 */
uint64_t panic_get_tsc_freq(void);

/*
 * Return a canonical panic cause string given a numeric cause code.
 * Use this to standardise panic messages across the kernel.
 * Cause codes:
 *   0 = NULL pointer dereference
 *   1 = Out of memory
 *   2 = Kernel BUG
 *   3 = Unexpected trap
 *   4 = Stack overflow
 *   5 = Scheduler failure
 *   6 = Filesystem error
 *   7 = Kernel page fault
 *   8 = Lockup / deadlock
 *   9 = RCU stall
 */
const char *panic_cause_str(int cause);

/* Helper macros */
#define BUG_ON(cond) do { \
    if (__builtin_expect(!!(cond), 0)) { \
        kprintf("BUG at %s:%d: " #cond "\n", __FILE__, __LINE__); \
        dump_stack(); \
        panic("BUG at %s:%d: %s", __FILE__, __LINE__, #cond); \
    } \
} while(0)

#define WARN_ON(cond) do { \
    if (__builtin_expect(!!(cond), 0)) { \
        extern uint64_t oops_count; \
        oops_count++; \
        kprintf("WARN at %s:%d: " #cond "\n", __FILE__, __LINE__); \
        dump_stack(); \
    } \
} while(0)

#endif
