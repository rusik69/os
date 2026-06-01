/*
 * rwsem.c — Read-Write Semaphore with owner tracking
 *
 * Implements a simple fair rwsem with:
 *   - count-based locking (count==-1 write-locked, count>=0 readers)
 *   - Writer owner PID tracking for debugging / deadlock detection
 *   - Lockdep integration (lock_acquire / lock_release)
 *   - Debug helpers: rwsem_owner(), rwsem_is_write_locked(),
 *                    rwsem_is_read_locked()
 */
#include "rwsem.h"
#include "scheduler.h"
#include "process.h"
#include "lockdep.h"
#include "printf.h"

void rwsem_init(struct rw_semaphore *sem) {
    sem->count        = 0;
    sem->wait_lock    = 0;
    sem->owner_pid    = 0;
    sem->reader_count = 0;
}

void down_read(struct rw_semaphore *sem) {
    if (!sem) return;

    /* Lockdep: track reader lock acquisition */
    lock_acquire("rwsem-read", (uint64_t)sem, LOCK_TYPE_RWSEM);

    for (;;) {
        if (sem->count >= 0) {
            int old = sem->count;
            if (__sync_bool_compare_and_swap(&sem->count, old, old + 1)) {
                sem->reader_count++;
                return;
            }
        }
        scheduler_yield();
    }
}

void up_read(struct rw_semaphore *sem) {
    if (!sem) return;

    lock_release("rwsem-read", (uint64_t)sem, LOCK_TYPE_RWSEM);

    __sync_fetch_and_sub(&sem->count, 1);
    if (sem->reader_count > 0)
        sem->reader_count--;
}

void down_write(struct rw_semaphore *sem) {
    if (!sem) return;

    /* Lockdep: track writer lock acquisition */
    lock_acquire("rwsem-write", (uint64_t)sem, LOCK_TYPE_RWSEM);

    for (;;) {
        if (sem->count == 0) {
            if (__sync_bool_compare_and_swap(&sem->count, 0, -1)) {
                /* Track the writer owner PID */
                struct process *self = process_get_current();
                sem->owner_pid = self ? self->pid : 0;
                return;
            }
        }
        scheduler_yield();
    }
}

void up_write(struct rw_semaphore *sem) {
    if (!sem) return;

    lock_release("rwsem-write", (uint64_t)sem, LOCK_TYPE_RWSEM);

    sem->owner_pid = 0;
    __sync_fetch_and_add(&sem->count, 1);
}

/* ── Owner tracking / debug helpers ───────────────────────────── */

uint32_t rwsem_owner(struct rw_semaphore *sem) {
    if (!sem) return 0;
    /* Only meaningful when write-locked (count == -1) */
    if (sem->count < 0)
        return sem->owner_pid;
    return 0;
}

int rwsem_is_write_locked(struct rw_semaphore *sem) {
    if (!sem) return 0;
    return sem->count < 0 ? 1 : 0;
}

int rwsem_is_read_locked(struct rw_semaphore *sem) {
    if (!sem) return 0;
    if (sem->count < 0) return 0; /* write-locked, not read-locked */
    return sem->reader_count > 0 ? 1 : 0;
}
