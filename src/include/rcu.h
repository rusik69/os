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
 *
 *   // Option A: synchronous wait
 *   synchronize_rcu();   // blocks until all existing readers finish
 *   kfree(old);
 *
 *   // Option B: asynchronous callback
 *   struct rcu_head rh = { .func = my_free_cb };
 *   call_rcu(&rh, my_free_cb);   // non-blocking; callback invoked after GP
 *
 * This implementation uses a quiescent-state tracking mechanism where
 * each CPU records a QS at every context switch.  Grace periods are
 * driven by the periodic timer tick (rcu_check_stall()).
 */

/* ── RCU callback mechanism ──────────────────────────────────────── */

/** Forward declaration of struct rcu_head (defined below). */
struct rcu_head;

/** Type of an RCU callback function. */
typedef void (*rcu_callback_t)(struct rcu_head *head);

/**
 * struct rcu_head — callback entry for call_rcu().
 *
 * Users embed this in their own data structures and pass it to
 * call_rcu().  The @func will be invoked after a grace period has
 * elapsed.  @next is for internal list management and must be
 * zero-initialised by the caller.
 */
struct rcu_head {
    struct rcu_head  *next;   /**< internal: pointer to next callback */
    rcu_callback_t    func;   /**< callback to invoke after GP */
};

/* ── Core RCU API ────────────────────────────────────────────────── */

/* Called at each context switch — records a quiescent state for the current CPU */
void rcu_quiescent_state(void);

/* Periodic stall check — call from timer tick or NMI context.
 * Returns 1 if a stall was detected and handled, 0 otherwise.
 * Prints warnings at RCU_STALL_WARN_TICKS, panics at RCU_STALL_PANIC_TICKS.
 * Also drives grace-period completion for call_rcu() callbacks. */
int rcu_check_stall(void);

/* Reader-side critical section markers */
static inline void rcu_read_lock(void)   { __asm__ volatile("": : :"memory"); }
static inline void rcu_read_unlock(void) { __asm__ volatile("": : :"memory"); }

/* Synchronize: block until every CPU has passed through a quiescent state.
 * Must not be called from within an RCU read-side critical section. */
void synchronize_rcu(void);

/**
 * call_rcu() — register an RCU callback to be invoked after a grace period.
 *
 * @head:  callback entry (must be zero-initialised by caller)
 * @func:  function to call after a quiescent state has passed on all CPUs
 *
 * This is non-blocking.  The callback is enqueued and will be invoked
 * from the timer tick context once all CPUs have passed through a
 * quiescent state.  For synchronous waiting, use rcu_barrier() or
 * synchronize_rcu().
 */
void call_rcu(struct rcu_head *head, rcu_callback_t func);

/**
 * rcu_barrier() — wait for all previously queued RCU callbacks to complete.
 *
 * Blocks the caller until all RCU callbacks that were submitted via
 * call_rcu() before the call to rcu_barrier() have been invoked.
 */
void rcu_barrier(void);

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
