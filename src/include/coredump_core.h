#ifndef COREDUMP_CORE_H
#define COREDUMP_CORE_H

#include "types.h"

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
 *
 * Dispatches to the registered handler, or silently does nothing if no
 * handler is registered (coredump not loaded / not initialised).
 */
void coredump_trigger(uint32_t pid);

/*
 * coredump_register_handler — Register a core dump handler function.
 * @handler: function pointer (receives PID, should schedule deferred dump).
 *
 * Returns 0 on success, -EBUSY if a handler is already registered.
 */
int coredump_register_handler(void (*handler)(uint32_t pid));

/*
 * coredump_unregister_handler — Unregister the core dump handler.
 * Safe to call even if no handler is registered.
 */
void coredump_unregister_handler(void);

#endif /* COREDUMP_CORE_H */
