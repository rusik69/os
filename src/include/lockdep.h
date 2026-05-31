#ifndef LOCKDEP_H
#define LOCKDEP_H

#include "types.h"

/*
 * Lockdep — Lock Dependency Validator.
 *
 * Tracks lock acquisition order at runtime and detects:
 *   - Deadlock cycles (A->B, B->A)
 *   - Double-lock of same lock by same task
 *   - Improper lock release (unlocking without holding)
 *
 * Usage:
 *   lock_acquire("lock_name", &lock_addr);
 *   lock_release("lock_name", &lock_addr);
 *
 * Call these from spin_lock() / spin_unlock() and mutex_lock() / mutex_unlock().
 * When lockdep is not compiled in (LOCKDEP_DISABLE), these are no-ops.
 */

#define LOCKDEP_MAX_LOCKS   64
#define LOCKDEP_MAX_DEPTH   16   /* max nested locks per task */
#define LOCKDEP_CLASS_HASH  16

/* Lock class — one per unique lock address */
struct lock_class {
    const char *name;
    uint64_t    addr;          /* address of the lock object */
    int         in_use;
    /* Dependency edges: which locks are held when this one is acquired */
    uint64_t    deps[LOCKDEP_MAX_LOCKS];
    int         dep_count;
};

/* Initialize lockdep subsystem */
void lockdep_init(void);

/* Record acquisition of a lock */
void lock_acquire(const char *name, uint64_t lock_addr);

/* Record release of a lock */
void lock_release(const char *name, uint64_t lock_addr);

/* Check for circular dependencies — returns non-zero if deadlock risk */
int lockdep_check_circular(uint64_t from_addr, uint64_t to_addr);

/* Dump lockdep state (for debugging) */
void lockdep_dump(void);

/* Validate that no locks are held at process exit */
void lockdep_check_exit(void);

#endif
