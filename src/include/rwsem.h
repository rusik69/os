#ifndef RWSEM_H
#define RWSEM_H
#include "types.h"
#include "spinlock.h"

/*
 * Read-Write Semaphore with optimistic spinning (Item 290).
 *
 *   count == -1  : write-locked by exactly one writer
 *   count ==  0  : unlocked (free)
 *   count  >  0  : read-locked by 'count' readers
 *
 * Optimistic spinning: before yielding the CPU, a waiter spins for
 * a limited number of iterations if the lock owner is currently
 * executing on another CPU.  This avoids the expensive sleep/wakeup
 * context switch when the lock is held briefly.
 */

#define RWSEM_SPIN_MAX       4096   /* max iterations per spin attempt */
#define RWSEM_SPIN_THRESHOLD 128    /* pause every N iterations */

struct rw_semaphore {
    volatile int count;          /* -1 = write-locked, >=0 = readers / free */
    spinlock_t   wait_lock;      /* spinlock for wait queue */
    uint32_t     owner_pid;      /* PID of writer owner (0 if free/read-locked) */
    uint32_t     reader_count;   /* number of active readers */

    /* Optimistic spinning state */
    int          owner_cpu;      /* CPU where writer owner was last running (-1 = none) */
    int          spinner_count;  /* number of tasks currently spinning on this rwsem */
};

void rwsem_init(struct rw_semaphore *sem);
void down_read(struct rw_semaphore *sem);
void up_read(struct rw_semaphore *sem);
void down_write(struct rw_semaphore *sem);
void up_write(struct rw_semaphore *sem);

/* ── Owner tracking / debug helpers ───────────────────────────── */

/* Returns PID of writer owner (0 if not write-locked). */
uint32_t rwsem_owner(struct rw_semaphore *sem);

/* Returns name of writer owner, or NULL if not write-locked. */
const char *rwsem_owner_name(struct rw_semaphore *sem);

/* Returns 1 if write-locked, 0 otherwise. */
int rwsem_is_write_locked(struct rw_semaphore *sem);

/* Returns number of active readers (0 if write-locked or free). */
int rwsem_is_read_locked(struct rw_semaphore *sem);

/* ── Optimistic spinning statistics ───────────────────────────── */
void rwsem_spin_stats(uint64_t *attempts, uint64_t *success,
                       uint64_t *abandoned, uint64_t *timeout);

#endif
