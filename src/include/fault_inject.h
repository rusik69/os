#ifndef FAULT_INJECT_H
#define FAULT_INJECT_H

#include "types.h"

/*
 * Fault injection framework for testing error paths.
 *
 * Provides a "fail_kmalloc" knob: when enabled, the N-th kmalloc() call
 * (or every call after N) returns NULL, allowing subsystems to exercise
 * their allocation failure recovery paths.
 *
 * Usage (from kernel init or debug shell):
 *   fault_inject_enable(10);   // fail every 10th kmalloc
 *   fault_inject_enable(0);    // fail all kmallocs
 *   fault_inject_disable();    // disable fault injection
 *
 * Sysfs interface (runtime control):
 *   /sys/kernel/debug/fault_inject/fail_kmalloc   (write "interval" or "0" to disable)
 *   /sys/kernel/debug/fault_inject/fail_count      (read current failure count)
 *   /sys/kernel/debug/fault_inject/call_count      (read total kmalloc calls counted)
 */

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialise the fault injection subsystem. Called once at boot. */
void fault_inject_init(void);

/* Enable fault injection: fail kmalloc every 'interval' calls.
 * interval=0 means fail every call.  interval<0 disables. */
void fault_inject_enable(int interval);

/* Disable fault injection.  Equivalent to fault_inject_enable(-1). */
static inline void fault_inject_disable(void) {
    fault_inject_enable(-1);
}

/* Return non-zero if the current kmalloc should fail (inject fault).
 * Called from kmalloc() — checks the global interval setting. */
int fault_inject_should_fail_kmalloc(void);

/* Return number of injected failures so far. */
uint64_t fault_inject_get_fail_count(void);

/* Return number of kmalloc calls counted by the fault injector. */
uint64_t fault_inject_get_call_count(void);

#endif /* FAULT_INJECT_H */
