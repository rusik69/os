/*
 * rwsem.c — Read-Write Semaphore with optimistic spinning (Item 290)
 *
 * Implements a fair rwsem with:
 *   - count-based locking (count==-1 write-locked, count>=0 readers)
 *   - Writer owner PID tracking for debugging / deadlock detection
 *   - Optimistic spinning: before yielding the CPU, a waiter spins
 *     for a limited number of iterations if the lock owner is currently
 *     executing on another CPU.  This avoids the expensive sleep/wakeup
 *     context switch when the lock is held for a short duration.
 *   - Lockdep integration (lock_acquire / lock_release)
 *   - Debug helpers: rwsem_owner(), rwsem_is_write_locked(),
 *                    rwsem_is_read_locked()
 *
 * Spinning strategy:
 *   - Writers writing-waiting: if count == -1 (write-locked), check if
 *     the writer owner is on-CPU; if yes, spin briefly.
 *   - Readers reading-waiting: if count < 0 (write-locked by a writer),
 *     check if the writer owner is on-CPU; if yes, spin briefly.
 *   - When count > 0 (reader-held), writers cannot easily check reader
 *     on-CPU state (multiple readers), so they fall through to yield
 *     after a brief spin.
 */
#include "rwsem.h"
#include "scheduler.h"
#include "process.h"
#include "lockdep.h"
#include "printf.h"
#include "smp.h"
#include "export.h"

/* ── Optimistic spinning statistics (exposed via sysctl/debug) ────── */
static uint64_t rwsem_spin_attempts   = 0;
static uint64_t rwsem_spin_success    = 0;
static uint64_t rwsem_spin_abandoned  = 0;
static uint64_t rwsem_spin_timeout    = 0;

void rwsem_init(struct rw_semaphore *sem) {
    if (!sem) return;
    sem->count         = 0;
    sem->wait_lock     = 0;
    sem->owner_pid     = 0;
    sem->reader_count  = 0;
    sem->owner_cpu     = -1;
    sem->spinner_count = 0;
}

void down_read(struct rw_semaphore *sem) {
    if (!sem) return;

    /* Lockdep: track reader lock acquisition */
    lock_acquire("rwsem-read", (uint64_t)sem, LOCK_TYPE_RWSEM);

    for (;;) {
        /* Fast path: try to acquire read lock directly.
         * Must be done with interrupts disabled to prevent
         * races with up_write (which also touches count). */
        __asm__ volatile("cli");
        if (sem->count >= 0) {
            int old = sem->count;
            if (__sync_bool_compare_and_swap(&sem->count, old, old + 1)) {
                sem->reader_count++;
                __asm__ volatile("sti");
                return;
            }
        }
        __asm__ volatile("sti");

        /* ── Optimistic spinning ────────────────────────────────────
         *
         * If count < 0, the lock is write-held by a single writer.
         * If that writer is currently on-CPU, spin for a limited
         * number of iterations hoping it releases the lock quickly.
         * Stop spinning early if the owner is no longer on-CPU. */
        if (sem->count < 0 && sem->spinner_count < 4) {
            sem->spinner_count++;
            rwsem_spin_attempts++;

            struct process *owner = process_get_by_pid(sem->owner_pid);
            int spin_count = 0;

            while (spin_count < RWSEM_SPIN_MAX) {
                /* Check if lock has become available (count >= 0) */
                __asm__ volatile("cli");
                if (sem->count >= 0) {
                    int old = sem->count;
                    if (__sync_bool_compare_and_swap(&sem->count, old, old + 1)) {
                        sem->reader_count++;
                        sem->spinner_count--;
                        rwsem_spin_success++;
                        __asm__ volatile("sti");
                        return;
                    }
                }
                __asm__ volatile("sti");

                /* Stop spinning if owner is no longer on-CPU */
                if (spin_count > 64 && owner && !owner->on_cpu) {
                    rwsem_spin_abandoned++;
                    break;
                }

                /* Pause periodically to be CPU-friendly */
                if ((spin_count & (RWSEM_SPIN_THRESHOLD - 1)) == 0) {
                    __asm__ volatile("pause");
                }

                spin_count++;
            }

            sem->spinner_count--;

            if (spin_count >= RWSEM_SPIN_MAX) {
                rwsem_spin_timeout++;
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
        /* Fast path: try to acquire write lock (count == 0) */
        __asm__ volatile("cli");
        if (sem->count == 0) {
            if (__sync_bool_compare_and_swap(&sem->count, 0, -1)) {
                /* Track the writer owner */
                struct process *self = process_get_current();
                sem->owner_pid   = self ? self->pid : 0;
                sem->owner_cpu   = smp_get_cpu_id();
                __asm__ volatile("sti");
                return;
            }
        }
        __asm__ volatile("sti");

        /* ── Optimistic spinning ────────────────────────────────────
         *
         * If count == -1, the lock is write-held by a single writer.
         * Check if that writer is on-CPU; if yes, spin briefly.
         * If count > 0, readers hold the lock — we spin briefly
         * anyway (they might finish soon) but stop quickly since
         * we can't efficiently check reader on-CPU state. */
        if (sem->spinner_count < 4) {
            sem->spinner_count++;
            rwsem_spin_attempts++;

            struct process *owner = (sem->count == -1)
                ? process_get_by_pid(sem->owner_pid)
                : NULL;
            int spin_count = 0;

            while (spin_count < RWSEM_SPIN_MAX) {
                /* Check if lock has become free */
                __asm__ volatile("cli");
                if (sem->count == 0) {
                    if (__sync_bool_compare_and_swap(&sem->count, 0, -1)) {
                        struct process *self = process_get_current();
                        sem->owner_pid   = self ? self->pid : 0;
                        sem->owner_cpu   = smp_get_cpu_id();
                        sem->spinner_count--;
                        rwsem_spin_success++;
                        __asm__ volatile("sti");
                        return;
                    }
                }
                __asm__ volatile("sti");

                /* For writer-held (count == -1): stop spinning if the
                 * writer owner is no longer on-CPU.  For reader-held
                 * (count > 0): we don't have per-reader tracking, so
                 * stop spinning sooner (after ~512 iterations). */
                if (sem->count == -1) {
                    if (spin_count > 64 && owner && !owner->on_cpu) {
                        rwsem_spin_abandoned++;
                        break;
                    }
                } else if (spin_count > 512) {
                    /* Reader-held: limit spin to avoid excessive
                     * busy-waiting since we can't efficiently check
                     * individual reader on-CPU state. */
                    rwsem_spin_abandoned++;
                    break;
                }

                /* Pause periodically to be CPU-friendly */
                if ((spin_count & (RWSEM_SPIN_THRESHOLD - 1)) == 0) {
                    __asm__ volatile("pause");
                }

                spin_count++;
            }

            sem->spinner_count--;

            if (spin_count >= RWSEM_SPIN_MAX) {
                rwsem_spin_timeout++;
            }
        }

        scheduler_yield();
    }
}

void up_write(struct rw_semaphore *sem) {
    if (!sem) return;

    lock_release("rwsem-write", (uint64_t)sem, LOCK_TYPE_RWSEM);

    sem->owner_pid   = 0;
    sem->owner_cpu   = -1;
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

const char *rwsem_owner_name(struct rw_semaphore *sem) {
    if (!sem || sem->count >= 0)
        return NULL;
    struct process *owner = process_get_by_pid(sem->owner_pid);
    return owner ? owner->name : NULL;
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

/* ── Optimistic spinning statistics ──────────────────────────────── */

void rwsem_spin_stats(uint64_t *attempts, uint64_t *success,
                       uint64_t *abandoned, uint64_t *timeout)
{
    if (attempts)  *attempts  = rwsem_spin_attempts;
    if (success)   *success   = rwsem_spin_success;
    if (abandoned) *abandoned = rwsem_spin_abandoned;
    if (timeout)   *timeout   = rwsem_spin_timeout;
}

/* ── Exported symbols for loadable kernel modules ────────────────── */
EXPORT_SYMBOL(rwsem_init);
EXPORT_SYMBOL(down_read);
EXPORT_SYMBOL(up_read);
EXPORT_SYMBOL(down_write);
EXPORT_SYMBOL(up_write);
EXPORT_SYMBOL(rwsem_owner);
EXPORT_SYMBOL(rwsem_owner_name);
EXPORT_SYMBOL(rwsem_is_write_locked);
EXPORT_SYMBOL(rwsem_is_read_locked);
