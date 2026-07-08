#ifndef COREDUMP_CORE_H
#define COREDUMP_CORE_H

#include "types.h"

/* Forward declaration */
struct process;

/*
 * coredump_core.h — Core dump hook indirection for loadable modules.
 *
 * When coredump is built as a loadable module (coredump.ko), the kernel
 * core cannot link directly to coredump_deferred().  Instead, the module
 * registers its handler via coredump_register_handler() on init and
 * unregisters via coredump_unregister_handler() on exit.
 *
 * For the built-in case, the same registration path is used during boot,
 * keeping a single interface regardless of how coredump is linked.
 */

/*
 * coredump_trigger — Called by do_coredump() to initiate a core dump.
 * @pid: process ID to dump.
 * @signo: signal number that caused the core dump (0 if unknown).
 *
 * Dispatches to the registered handler, or silently does nothing if no
 * handler is registered (coredump not loaded / not initialised).
 */
void coredump_trigger(uint32_t pid, int signo);

/*
 * Handler function type — receives PID and signal number that triggered
 * the core dump.
 */
typedef void (*coredump_handler_fn)(uint32_t pid, int signo);

/*
 * coredump_register_handler — Register a core dump handler function.
 * @handler: function pointer (receives PID + signo, should schedule deferred dump).
 *
 * Returns 0 on success, -EBUSY if a handler is already registered.
 */
int coredump_register_handler(coredump_handler_fn handler);

/*
 * coredump_unregister_handler — Unregister the core dump handler.
 * Safe to call even if no handler is registered.
 */
void coredump_unregister_handler(void);

/*
 * do_coredump — Initiate a core dump for a process that received a fatal signal.
 * @proc: the process to dump.
 * @signo: the signal that triggered the dump.
 *
 * Checked RLIMIT_CORE and dispatches to coredump_trigger().
 */
void do_coredump(struct process *proc, int signo);

#endif /* COREDUMP_CORE_H */
