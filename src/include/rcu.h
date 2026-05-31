#ifndef RCU_H
#define RCU_H

#include "types.h"

/*
 * RCU (Read-Copy-Update) — scalable synchronization for read-mostly data.
 *
 * Usage:
 *   DEFINE_RCU_POINTER(struct foo, my_foo);   // or just: struct foo __rcu *my_foo;
 *
 *   // Reader side:
 *   rcu_read_lock();
 *   struct foo *p = rcu_dereference(my_foo);
 *   // ... read through p ...
 *   rcu_read_unlock();
 *
 *   // Updater side:
 *   struct foo *new = kmalloc(sizeof(*new));
 *   *new = *old;  // copy
 *   new->field = new_value;
 *   rcu_assign_pointer(my_foo, new);
 *   synchronize_rcu();   // wait for all existing readers to finish
 *   kfree(old);
 *
 * This implementation uses a simplified grace-period detection: on
 * synchronize_rcu(), we snapshot the per-CPU read-side counters and
 * busy-wait until all counters return to their base state (or a
 * quiescent state is detected via context switch).
 *
 * Limitations:
 *   - Blocking synchronize_rcu() (SOFTIRQ-based GP pending)
 *   - Single-threaded updater assumed for now
 *   - No call_rcu() callback mechanism (yet)
 */

/* Called at each context switch — records a quiescent state for the current CPU */
void rcu_quiescent_state(void);

/* Reader-side critical section markers */
static inline void rcu_read_lock(void)   { __asm__ volatile("": : :"memory"); }
static inline void rcu_read_unlock(void) { __asm__ volatile("": : :"memory"); }

/* Synchronize: block until every CPU has passed through a quiescent state.
 * Must not be called from within an RCU read-side critical section. */
void synchronize_rcu(void);

/* Initialize RCU subsystem (call once at boot) */
void rcu_init(void);

/* Dereference an RCU-protected pointer */
#define rcu_dereference(p)  ({ \
    __typeof__(p) ___p = (p); \
    __asm__ volatile("" : : : "memory"); \
    ___p; })

/* Assign a new value to an RCU-protected pointer */
#define rcu_assign_pointer(p, v)  ({ \
    __asm__ volatile("" : : : "memory"); \
    (p) = (v); \
})

#endif
