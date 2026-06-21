/*
 * mutex.c — Priority Inheritance mutex with optimistic spinning
 *
 * Implements the Priority Inheritance Protocol (PIP) to prevent
 * priority inversion: when a high-priority task waits on a mutex
 * held by a low-priority task, the holder is temporarily boosted
 * to the waiter's priority.  Properly handles nested mutexes:
 * when a process holds multiple mutexes and releases one, the
 * priority is recomputed from the remaining held mutexes' highest
 * waiter priorities.
 *
 * Optimistic spinning (Item 289): before yielding the CPU, a waiter
 * spins for a limited number of iterations if the lock owner is
 * currently executing on another CPU.  This avoids the expensive
 * sleep/wakeup context switch when the lock is held for a short
 * duration (common for well-designed mutexes).  Spinning stops
 * early if the owner is scheduled out, preventing wasteful busy-
 * waiting.
 *
 * Integrates with lockdep for deadlock detection and provides
 * owner-tracking debug helpers (mutex_owner(), mutex_owner_name(),
 * mutex_is_locked()).
 */

#include "mutex.h"
#include "scheduler.h"
#include "process.h"
#include "string.h"
#include "lockdep.h"
#include "export.h"
#include "smp.h"
#include "errno.h"

#define MUTEX_MAX 32
#define MUTEX_WAITERS_MAX 8

/* ── Optimistic spinning configuration ────────────────────────────── */
#define MUTEX_SPIN_MAX        4096   /* max iterations per spin attempt */
#define MUTEX_SPIN_THRESHOLD  128    /* pause every N iterations to be CPU-friendly */
#define MUTEX_OSQ_MAX         4      /* max queued spinners on one mutex */

/* ── Optimistic spin statistics (exposed via sysctl/debug) ────────── */
static uint64_t mutex_spin_attempts   = 0;  /* total spin attempts */
static uint64_t mutex_spin_success    = 0;  /* spins that acquired the lock */
static uint64_t mutex_spin_abandoned  = 0;  /* spins abandoned (owner off CPU) */
static uint64_t mutex_spin_timeout    = 0;  /* spins that timed out -> yield */

struct mutex_entry {
    volatile int locked;
    int in_use;
    uint32_t owner_pid;              /* PID of current owner (0 = free) */
    uint64_t owner_rip;              /* RIP where owner acquired the lock */
    uint8_t  owner_orig_prio;        /* owner's priority before any boost */
    uint8_t  highest_waiter_prio;    /* highest priority among waiters (9 = none) */
    int      waiter_count;
    uint32_t waiter_pids[MUTEX_WAITERS_MAX]; /* PIDs waiting on this mutex */
    int      owner_cpu;              /* CPU where owner was last running (for spin heuristics) */
    int      spinner_count;          /* number of tasks currently spinning on this mutex */
};

static struct mutex_entry mutexes[MUTEX_MAX];

/* Priority Inheritance boost tracking array */
uint8_t mutex_boost[MUTEX_MAX_PI_BOOST];

static void boost_owner(struct mutex_entry *m, uint8_t waiter_prio);
static void restore_owner_priority(struct mutex_entry *m);

/* ── Held-mutex tracking helpers ───────────────────────────────────── */

/* Add a mutex ID to the process's held-mutex list.
 * Silently ignores duplicates and full list (caller must ensure
 * a process never holds more than PROCESS_MAX_HELD_MUTEXES). */
static void held_mutex_add(struct process *proc, int mutex_id) {
    if (!proc) return;

    /* Check for duplicate */
    for (int i = 0; i < proc->held_mutex_count; i++) {
        if (proc->held_mutex_ids[i] == mutex_id)
            return;
    }

    /* Add if there's room */
    if (proc->held_mutex_count < PROCESS_MAX_HELD_MUTEXES) {
        proc->held_mutex_ids[proc->held_mutex_count++] = mutex_id;
    }
}

/* Remove a mutex ID from the process's held-mutex list.
 * No-op if the mutex is not found. */
static void held_mutex_remove(struct process *proc, int mutex_id) {
    if (!proc) return;

    for (int i = 0; i < proc->held_mutex_count; i++) {
        if (proc->held_mutex_ids[i] == mutex_id) {
            /* Shift remaining entries left */
            int remaining = proc->held_mutex_count - i - 1;
            if (remaining > 0) {
                __builtin_memmove(&proc->held_mutex_ids[i],
                                  &proc->held_mutex_ids[i + 1],
                                  (size_t)remaining * sizeof(int));
            }
            proc->held_mutex_count--;
            return;
        }
    }
}

/* Compute the effective priority for a process based on all held mutexes.
 *
 * The effective priority is the highest (lowest numeric value) among:
 *   - the process's base priority
 *   - the highest waiter priority from each held mutex
 *
 * Returns the priority to set on the process (same unit: 0=highest, 3=lowest). */
static uint8_t held_mutex_effective_prio(uint32_t pid) {
    struct process *owner = process_get_by_pid(pid);
    if (!owner || owner->state == PROCESS_UNUSED)
        return 9; /* sentinel: don't care */

    uint8_t best = owner->base_priority; /* start with base (lower = higher prio) */

    /* Scan all held mutexes for their highest_waiter_prio */
    for (int i = 0; i < owner->held_mutex_count; i++) {
        int id = owner->held_mutex_ids[i];
        if (id < 0 || id >= MUTEX_MAX) continue;
        struct mutex_entry *m = &mutexes[id];
        if (!m->in_use || !m->locked) continue;

        /* highest_waiter_prio is 9 when no waiters (i.e., no one waiting).
         * Only consider mutexes with actual waiters. */
        if (m->highest_waiter_prio < best) {
            best = m->highest_waiter_prio;
        }
    }

    return best;
}

/* ── Initialization ──────────────────────────────────────────────── */

int mutex_init(void) {
    for (int i = 0; i < MUTEX_MAX; i++) {
        /* Save IF state; don't enable interrupts if they were disabled
         * (early boot: PIT interrupt would fire before scheduler is ready). */
        uint64_t __iflags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(__iflags) :: "memory");
        if (!mutexes[i].in_use) {
            mutexes[i].in_use  = 1;
            mutexes[i].locked  = 0;
            mutexes[i].owner_pid = 0;
            mutexes[i].owner_rip = 0;
            mutexes[i].highest_waiter_prio = 9; /* sentinel > any valid priority */
            mutexes[i].waiter_count = 0;
            mutexes[i].owner_cpu = -1;  /* no owner */
            mutexes[i].spinner_count = 0;
            if (__iflags & 0x200)
                __asm__ volatile("sti");
            return i;
        }
        if (__iflags & 0x200)
            __asm__ volatile("sti");
    }
    return -1;
}

/* ── Lock / Unlock ─────────────────────────────────────────────────── */

void mutex_lock(int id) {
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use) return;
    struct mutex_entry *m = &mutexes[id];
    struct process *self = process_get_current();
    if (!self) return;

    /* Lockdep: check if we already hold this mutex (recursive deadlock) */
    lock_acquire("mutex", (uint64_t)&mutexes[id], LOCK_TYPE_MUTEX);

    for (;;) {
        __asm__ volatile("cli");
        if (!m->locked) {
            /* Acquire the mutex */
            m->locked = 1;
            m->owner_pid = self->pid;
            m->owner_rip = (uint64_t)__builtin_return_address(0);
            m->owner_orig_prio = self->base_priority;
            m->owner_cpu = smp_get_cpu_id();

            /* Register this mutex in the owner's held-mutex list */
            held_mutex_add(self, id);

            __asm__ volatile("sti");
            return;
        }

        /* Mutex is held by another process — perform priority inheritance */
        struct process *owner = process_get_by_pid(m->owner_pid);
        if (owner && owner->state != PROCESS_UNUSED) {
            /* Track the highest waiter priority for this mutex */
            if (self->priority < m->highest_waiter_prio) {
                m->highest_waiter_prio = self->priority;
            }

            /* Boost the owner if this waiter has higher priority */
            boost_owner(m, self->priority);

            /* Reflect the waiter into the owner's priority immediately */
            uint8_t effective = held_mutex_effective_prio(m->owner_pid);
            if (effective < owner->priority) {
                owner->priority = effective;
            }
        }

        /* Register as waiter */
        if (m->waiter_count < MUTEX_WAITERS_MAX) {
            m->waiter_pids[m->waiter_count++] = self->pid;
        }

        __asm__ volatile("sti");

        /* ── Optimistic spinning ────────────────────────────────────
         *
         * If the mutex owner is currently executing on a CPU, there is
         * a good chance it will release the lock very soon.  Instead of
         * yielding the CPU immediately (which incurs the full cost of a
         * context switch: save, reschedule, switch, later restore), we
         * spin for a limited number of iterations checking if the lock
         * becomes free.
         *
         * We stop spinning early if:
         *   1. The owner is no longer on-CPU (scheduled out) — spinning
         *      would be wasteful since the owner won't make progress.
         *   2. The spin count exceeds MUTEX_SPIN_MAX — prevent
         *      starvation of other tasks.
         *
         * The `pause` instruction is used in the tight loop to reduce
         * power consumption and improve hyperthreading performance. */
        if (m->spinner_count < MUTEX_OSQ_MAX) {
            m->spinner_count++;
            mutex_spin_attempts++;

            int spin_count = 0;

            while (spin_count < MUTEX_SPIN_MAX) {
                /* Read the lock flag without cli/sti — volatile ensures
                 * we see the latest value from other CPUs. */
                if (!m->locked) {
                    /* Lock became free — acquire it */
                    __asm__ volatile("cli");
                    if (!m->locked) {
                        m->locked = 1;
                        m->owner_pid = self->pid;
                        m->owner_rip = (uint64_t)__builtin_return_address(0);
                        m->owner_orig_prio = self->base_priority;
                        m->owner_cpu = smp_get_cpu_id();
                        held_mutex_add(self, id);
                        m->spinner_count--;
                        mutex_spin_success++;
                        __asm__ volatile("sti");
                        return;
                    }
                    __asm__ volatile("sti");
                    break; /* someone else got it, fall through to yield */
                }

                /* Check if owner is still on-CPU; if not, stop spinning */
                if (spin_count > 64 && owner && !owner->on_cpu) {
                    mutex_spin_abandoned++;
                    break;
                }

                /* Periodic pause to be CPU-friendly */
                if ((spin_count & (MUTEX_SPIN_THRESHOLD - 1)) == 0) {
                    __asm__ volatile("pause");
                }

                spin_count++;
            }

            m->spinner_count--;

            if (spin_count >= MUTEX_SPIN_MAX) {
                mutex_spin_timeout++;
            }
        }

        scheduler_yield();
    }
}

void mutex_unlock(int id) {
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use) return;
    struct mutex_entry *m = &mutexes[id];

    /* Lockdep: release before unlocking */
    lock_release("mutex", (uint64_t)&mutexes[id], LOCK_TYPE_MUTEX);

    /* Remove this mutex from the owner's held-mutex list, then
     * recompute the owner's priority from any remaining held mutexes. */
    struct process *owner = process_get_by_pid(m->owner_pid);
    if (owner && owner->state != PROCESS_UNUSED) {
        held_mutex_remove(owner, id);
        restore_owner_priority(m);
    }

    /* Release the mutex */
    m->locked = 0;
    m->owner_pid = 0;
    m->owner_rip = 0;
    m->owner_cpu = -1;
    m->waiter_count = 0;
    m->highest_waiter_prio = 9;
    /* spinner_count is cleared because all spinners will see locked==0 */
}

void mutex_destroy(int id) {
    if (id < 0 || id >= MUTEX_MAX) return;
    struct mutex_entry *m = &mutexes[id];

    /* If held, force-release lockdep tracking */
    if (mutexes[id].locked) {
        lock_release("mutex", (uint64_t)&mutexes[id], LOCK_TYPE_MUTEX);
    }

    /* Restore owner priority before destroying */
    restore_owner_priority(m);

    /* Remove from owner's held-mutex list if present */
    struct process *owner = process_get_by_pid(m->owner_pid);
    if (owner && owner->state != PROCESS_UNUSED) {
        held_mutex_remove(owner, id);
    }

    mutexes[id].in_use  = 0;
    mutexes[id].locked  = 0;
    mutexes[id].owner_pid = 0;
    mutexes[id].owner_rip = 0;
    mutexes[id].waiter_count = 0;
    mutexes[id].highest_waiter_prio = 9;
}

/* ── Owner tracking debug helpers ──────────────────────────────────── */

uint32_t mutex_owner(int id) {
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use) return 0;
    return mutexes[id].locked ? mutexes[id].owner_pid : 0;
}

const char *mutex_owner_name(int id) {
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use || !mutexes[id].locked)
        return NULL;
    struct process *owner = process_get_by_pid(mutexes[id].owner_pid);
    return owner ? owner->name : NULL;
}

uint64_t mutex_owner_rip(int id) {
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use || !mutexes[id].locked)
        return 0;
    return mutexes[id].owner_rip;
}

int mutex_is_locked(int id) {
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use) return 0;
    return mutexes[id].locked ? 1 : 0;
}

/* ── Priority inheritance helpers ──────────────────────────────────── */

/* Boost the mutex owner to waiter_prio if the waiter has higher priority.
 *
 * "Higher priority" means a lower numeric value (0=highest, 3=lowest).
 * The owner's original base priority is saved in owner_orig_prio on the
 * first boost (i.e., when the owner's current priority still equals its
 * base priority), so it can be used when all boosts are released.
 *
 * Note: the final effective priority is computed by held_mutex_effective_prio()
 * which scans ALL held mutexes.  boost_owner() provides the initial single-mutex
 * boost, and mutex_lock() additionally calls held_mutex_effective_prio() after
 * boost_owner() to ensure the priority reflects the maximum across all held
 * mutexes.
 */
static void boost_owner(struct mutex_entry *m, uint8_t waiter_prio) {
    struct process *owner = process_get_by_pid(m->owner_pid);
    if (!owner || owner->state == PROCESS_UNUSED) return;

    /* waiter_prio is lower number = higher priority.
     * Boost if waiter_prio < owner->priority (meaning waiter is more important) */
    if (waiter_prio < owner->priority) {
        /* Save original priority only on the first boost.
         * If owner->priority != owner->base_priority, it's already been boosted
         * by another mutex and owner_orig_prio was already captured. */
        if (owner->priority == owner->base_priority) {
            m->owner_orig_prio = owner->priority;
        }
        owner->priority = waiter_prio;

        /* Track the boost */
        if (m->owner_pid < MUTEX_MAX_PI_BOOST)
            mutex_boost[m->owner_pid] = waiter_prio;
    }
}

/* Restore the owner's priority after unlocking a mutex.
 *
 * Instead of unconditionally resetting to base_priority (which would
 * break nested PI), we compute the effective priority across all
 * remaining held mutexes via held_mutex_effective_prio().
 *
 * If no other held mutex requires a boost, the effective priority
 * will be base_priority — the correct behaviour.
 */
static void restore_owner_priority(struct mutex_entry *m) {
    struct process *owner = process_get_by_pid(m->owner_pid);
    if (!owner || owner->state == PROCESS_UNUSED) return;

    /* Compute the new priority considering all OTHER held mutexes.
     * held_mutex_remove() was already called before this, so the
     * current mutex is no longer in the owner's held list. */
    uint8_t new_prio = held_mutex_effective_prio(m->owner_pid);

    owner->priority = new_prio;
    m->owner_orig_prio = owner->base_priority;

    /* Clear boost tracking */
    if (m->owner_pid < MUTEX_MAX_PI_BOOST)
        mutex_boost[m->owner_pid] = 0;
}

/* ── Optimistic spinning statistics ──────────────────────────────── */

void mutex_spin_stats(uint64_t *attempts, uint64_t *success,
                       uint64_t *abandoned, uint64_t *timeout)
{
    if (attempts)  *attempts  = mutex_spin_attempts;
    if (success)   *success   = mutex_spin_success;
    if (abandoned) *abandoned = mutex_spin_abandoned;
    if (timeout)   *timeout   = mutex_spin_timeout;
}

/* ── Exported symbols for loadable kernel modules ────────────────── */
EXPORT_SYMBOL(mutex_init);
EXPORT_SYMBOL(mutex_lock);
EXPORT_SYMBOL(mutex_unlock);

/* ── mutex_trylock (non-blocking) ──────────────────────── */
int mutex_trylock(int id)
{
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use)
        return -EINVAL;
    struct mutex_entry *m = &mutexes[id];
    struct process *self = process_get_current();
    if (!self) return -EINVAL;

    __asm__ volatile("cli");
    if (!m->locked) {
        m->locked = 1;
        m->owner_pid = self->pid;
        m->owner_rip = (uint64_t)__builtin_return_address(0);
        m->owner_orig_prio = self->base_priority;
        m->owner_cpu = smp_get_cpu_id();
        held_mutex_add(self, id);
        __asm__ volatile("sti");
        return 0;
    }
    __asm__ volatile("sti");
    return -EBUSY;
}

/* ── mutex_lock_interruptible ──────────────────────────── */
int mutex_lock_interruptible(int id)
{
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use)
        return -EINVAL;
    struct mutex_entry *m = &mutexes[id];
    struct process *self = process_get_current();
    if (!self) return -EINVAL;

    lock_acquire("mutex", (uint64_t)&mutexes[id], LOCK_TYPE_MUTEX);

    for (;;) {
        /* Check for pending signals before blocking */
        if (self->pending_signals & ~self->sig_mask)
            return -EINTR;

        __asm__ volatile("cli");
        if (!m->locked) {
            m->locked = 1;
            m->owner_pid = self->pid;
            m->owner_rip = (uint64_t)__builtin_return_address(0);
            m->owner_orig_prio = self->base_priority;
            m->owner_cpu = smp_get_cpu_id();
            held_mutex_add(self, id);
            __asm__ volatile("sti");
            return 0;
        }
        __asm__ volatile("sti");

        /* Brief spin, then yield */
        for (int i = 0; i < 64; i++) {
            if (!m->locked) {
                __asm__ volatile("cli");
                if (!m->locked) {
                    m->locked = 1;
                    m->owner_pid = self->pid;
                    m->owner_rip = (uint64_t)__builtin_return_address(0);
                    m->owner_orig_prio = self->base_priority;
                    m->owner_cpu = smp_get_cpu_id();
                    held_mutex_add(self, id);
                    __asm__ volatile("sti");
                    return 0;
                }
                __asm__ volatile("sti");
            }
            __asm__ volatile("pause");
        }

        scheduler_yield();
    }
}

/* ── mutex_trylock_debug ─────────────────────────────── */
int mutex_trylock_debug(void *lock, const char *file, int line)
{
    (void)file;
    (void)line;
    if (!lock) return -EINVAL;
    spinlock_t *s = (spinlock_t *)lock;
    if (spinlock_try_acquire(s))
        return 0;
    return -EBUSY;
}
/* ── mutex_unlock_debug ─────────────────────────────── */
int mutex_unlock_debug(void *lock, const char *file, int line)
{
    (void)file;
    (void)line;
    if (!lock) return -EINVAL;
    spinlock_t *s = (spinlock_t *)lock;
    spinlock_release(s);
    return 0;
}
