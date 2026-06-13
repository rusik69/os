#ifndef FTRACE_H
#define FTRACE_H

#include "types.h"

/*
 * Dynamic function tracer (FTRACE).
 *
 * Provides a mechanism to register trace callbacks on kernel functions.
 * When a traced function is called, the registered callback is invoked
 * with the function's return address.
 *
 * Implementation: kprobe-based with a trampoline table for now.
 */

/* Maximum number of tracepoints */
#define FTRACE_MAX_TRACEPOINTS 64

/* Tracepoint entry */
struct ftrace_tracepoint {
    char     func_name[64];    /* function name to trace */
    void     (*callback)(uint64_t ip, uint64_t parent_ip);
    int      active;           /* 1 = registered */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialize the FTRACE subsystem */
void ftrace_init(void);

/* Register a trace callback on a function by name.
 * Returns 0 on success, -1 on error (table full or already registered). */
int  ftrace_register(const char *func_name, void (*callback)(uint64_t ip, uint64_t parent_ip));

/* Unregister a tracepoint by function name.
 * Returns 0 on success, -1 if not found. */
int  ftrace_unregister(const char *func_name);

/* Enable/disable all tracepoints globally. */
void ftrace_enable(void);
void ftrace_disable(void);

/* Check if FTRACE is currently enabled. */
int  ftrace_enabled(void);

/* Called by the kprobe handler when a traced function is hit.
 * Not intended for direct use by modules. */
void ftrace_dispatch(uint64_t ip, uint64_t parent_ip);

/* Dump registered tracepoints via kprintf. */
void ftrace_dump(void);

#endif /* FTRACE_H */
