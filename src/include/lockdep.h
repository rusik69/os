#ifndef LOCKDEP_H
#define LOCKDEP_H

#include "types.h"
#include "spinlock.h"

/*
 * Lockdep — Lock Dependency Validator.
 *
 * Tracks lock acquisition order at runtime and detects:
 *   - Deadlock cycles via DFS on the dependency graph
 *   - Cross-release violations (LIFO order not respected)
 *   - Double-lock / unlock-without-hold
 *   - Sleeping while atomic (mutex acquire while holding spinlock)
 *   - Lock leaks at process exit
 *
 * Usage:
 *   lock_acquire("lock_name", &lock_addr, lock_type);
 *   lock_release("lock_name", &lock_addr, lock_type);
 *
 * Call these from spin_lock() / spin_unlock() and mutex_lock() / mutex_unlock().
 */

#define LOCKDEP_MAX_LOCKS       64
#define LOCKDEP_MAX_DEPTH       16
#define LOCKDEP_HASHSIZE        16

/* Lock types for cross-release / sleeping-while-atomic checking */
#define LOCK_TYPE_SPINLOCK      0
#define LOCK_TYPE_MUTEX         1
#define LOCK_TYPE_RWSEM         2
#define LOCK_TYPE_RCU           3

/* Lock class — one per unique lock address */
struct lock_class {
    const char *name;
    uint64_t    addr;
    int         in_use;
    int         type;              /* LOCK_TYPE_* */
    /* Dependency edges: addr of locks held when this one is acquired */
    uint64_t    deps[LOCKDEP_MAX_LOCKS];
    int         dep_count;
    /* Reverse edges: locks acquired while holding this lock */
    uint64_t    children[LOCKDEP_MAX_LOCKS];
    int         child_count;
};

/* Held lock entry — tracks one lock in the held stack */
struct held_lock {
    uint64_t    addr;
    const char *name;
    int         type;
    int         acquire_seq;       /* global acquire sequence # for LIFO check */
};

/* Per-CPU spinlock nesting counter for sleeping-while-atomic detection */
extern int spinlock_nest_count;

/* Initialize lockdep subsystem */
void lockdep_init(void);

/* Record acquisition of a lock */
void lock_acquire(const char *name, uint64_t lock_addr, int type);

/* Record release of a lock */
void lock_release(const char *name, uint64_t lock_addr, int type);

/* Check for circular dependencies — returns non-zero if deadlock risk */
int lockdep_check_circular(uint64_t from_addr, uint64_t to_addr);

/* Dump lockdep state (for debugging) */
void lockdep_dump(void);

/* Validate that no locks are held at process exit */
void lockdep_check_exit(void);

/* Spinlock nesting helpers for sleeping-while-atomic */
void lockdep_spinlock_acquired(void);
void lockdep_spinlock_released(void);
int  lockdep_holding_spinlock(void);

/* ── Spinlock owner tracking and lockup detection ───────────────── */

/* Register ownership of a spinlock (called by spinlock_acquire internals) */
void spinlock_register_owner(spinlock_t *lock, uint64_t caller_rip);

/* Clear ownership of a spinlock (called by spinlock_release internals) */
void spinlock_unregister_owner(spinlock_t *lock);

/* Diagnose a spinlock lockup */
void spinlock_detect_lockup(spinlock_t *lock, uint64_t spin_count);

/* Forcibly release all spinlocks held by the current CPU (panic path) */
void spinlock_release_all_on_panic(void);

#endif /* LOCKDEP_H */
