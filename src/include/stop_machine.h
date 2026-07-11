#ifndef STOP_MACHINE_H
#define STOP_MACHINE_H

#include "types.h"

/*
 * stop_machine — Synchronously execute a function on all online CPUs.
 *
 * The stop_machine mechanism is the foundation for text patching,
 * kexec, and other operations that require all CPUs to reach a
 * known quiescent state before the calling CPU performs a
 * system-wide change.
 *
 * stop_machine_call() sends an IPI to every other online CPU and
 * waits for each to acknowledge completion.  The callback function
 * is called directly on the calling CPU and from the IPI handler on
 * each remote CPU.
 *
 * The callback runs in interrupt context on remote CPUs and in
 * normal context on the calling CPU (with preemption implicitly
 * disabled for the duration).  It must not sleep, acquire blocking
 * locks, or perform any operation that requires scheduling.
 *
 * ── Usage ──────────────────────────────────────────────────────────
 *
 *   static void my_callback(void *data) {
 *       // perform atomic work
 *   }
 *
 *   stop_machine_call(my_callback, &my_data);
 *
 * Returns 0 on success, -1 if the subsystem is not initialised.
 */

/* Callback type — invoked on every online CPU. */
typedef void (*stop_machine_fn_t)(void *data);

/*
 * Execute @fn(data) on every online CPU synchronously.
 *
 * The calling CPU executes @fn directly; each remote CPU executes it
 * from the IPI handler before acknowledging completion.  The function
 * does not return until all CPUs have completed.
 *
 * On uniprocessor (single CPU), @fn is called and this returns
 * immediately — no IPI is sent.
 *
 * Must not be called from interrupt context (it disables local
 * interrupts to prevent deadlock against the stop-machine IPI).
 *
 * Returns 0 on success, -1 on failure.
 */
int stop_machine_call(stop_machine_fn_t fn, void *data);

/*
 * Initialize the stop_machine subsystem.
 * Called once during kernel boot (from ipi_init()).
 * Registers the IPI vector handler.
 */
void stop_machine_init(void);

#endif /* STOP_MACHINE_H */
