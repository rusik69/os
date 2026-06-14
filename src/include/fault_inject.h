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

/* ── Per-callsite probability (FAIL Probability = N / RATE) ──────── */

/* Configure a callsite with failure probability N/RATE.
 * @caller_ip — use __builtin_return_address(0) from the allocation site.
 * Returns 0 on success, -1 if table full. */
int fault_inject_callsite_config(uint64_t caller_ip, int fail_n, int fail_rate);

/* Check if a specific callsite should fail. Call from allocation sites. */
int fault_inject_callsite_should_fail(uint64_t caller_ip);

/* ── alloc_pages / vmalloc failure injection ───────────────────── */

/* Enable/disable alloc_pages fault injection (interval-based). */
void fault_inject_alloc_pages_enable(int interval);

/* Return non-zero if the current alloc_pages should fail. */
int fault_inject_should_fail_alloc_pages(void);

/* Enable/disable vmalloc fault injection (interval-based). */
void fault_inject_vmalloc_enable(int interval);

/* Return non-zero if the current vmalloc should fail. */
int fault_inject_should_fail_vmalloc(void);

#endif /* FAULT_INJECT_H */
