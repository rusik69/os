#ifndef RWSEM_H
#define RWSEM_H
#include "types.h"
#include "spinlock.h"

struct rw_semaphore {
    volatile int count;          /* -1 = write-locked, >=0 = readers / free */
    spinlock_t   wait_lock;      /* spinlock for wait queue */
    uint32_t     owner_pid;      /* PID of writer owner (0 if free/read-locked) */
    uint32_t     reader_count;   /* number of active readers */
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

#endif
