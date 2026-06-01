#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"
#include "io.h"  /* for pause/rep nop */

/*
 * Spinlock — busy-wait lock with exponential backoff.
 *
 * Production features:
 *   - Adaptive exponential backoff (up to 256 PAUSE iterations)
 *   - Lockup detection: after SPINLOCK_LOCKUP_THRESHOLD spins,
 *     prints diagnostic info (caller, owner, stack trace)
 *   - Owner tracking via spinlock_debug subsystem for diagnostics
 *   - Panic-time release of held spinlocks (via panic notifier)
 */

typedef volatile int spinlock_t;

#define SPINLOCK_INIT 0

/* Lockup detection threshold — ~1 billion PAUSE iterations */
#define SPINLOCK_LOCKUP_THRESHOLD  1000000000ULL

/* ── Spinlock lockup detection threshold constant ────────────────── */

#ifndef SPINLOCK_DEBUG_DISABLE

/* Debug/owner tracking functions (implemented in lockdep.c) */

/* Register owner of a lock (called after successful acquire) */
void spinlock_register_owner(spinlock_t *lock, uint64_t caller_rip);

/* Unregister owner (called before release) */
void spinlock_unregister_owner(spinlock_t *lock);

/* Called when a lockup is detected — prints diagnostics */
void spinlock_detect_lockup(spinlock_t *lock, uint64_t spin_count);

/* Release all tracked spinlocks (panic notifier) */
void spinlock_release_all_on_panic(void);

#else

/* Stubs when debugging is disabled */
static inline void spinlock_register_owner(spinlock_t *lock, uint64_t caller_rip) { (void)lock; (void)caller_rip; }
static inline void spinlock_unregister_owner(spinlock_t *lock) { (void)lock; }
static inline void spinlock_detect_lockup(spinlock_t *lock, uint64_t spin_count) { (void)lock; (void)spin_count; }
static inline void spinlock_release_all_on_panic(void) {}

#endif /* SPINLOCK_DEBUG_DISABLE */

/* ── Non-recursive spinlock operations ────────────────────────── */

static inline void spinlock_init(spinlock_t *lock) {
    *lock = 0;
}

/* Adaptive spinlock with exponential backoff and lockup detection */
static inline void spinlock_acquire(spinlock_t *lock) {
    int backoff = 1;
    const int max_backoff = 256;
    uint64_t spin_count = 0;

    while (__sync_lock_test_and_set(lock, 1)) {
        /* Adaptive backoff with PAUSE */
        for (int i = 0; i < backoff; i++) {
            __asm__ volatile("pause");
        }
        if (backoff < max_backoff)
            backoff <<= 1;  /* exponential backoff */
        spin_count += (uint64_t)backoff;

        /* Lockup detection: if we've been spinning too long, dump diagnostics */
        if (spin_count >= SPINLOCK_LOCKUP_THRESHOLD) {
            spinlock_detect_lockup(lock, spin_count);
            spin_count = 0;  /* reset to avoid repeat floods */
        }
    }
    __sync_synchronize(); /* full memory barrier */

    /* Record ownership for diagnostic tracking */
    spinlock_register_owner(lock, (uint64_t)__builtin_return_address(0));
}

static inline void spinlock_release(spinlock_t *lock) {
    /* Clear ownership before releasing */
    spinlock_unregister_owner(lock);

    __sync_synchronize();
    __sync_lock_release(lock);
}

static inline int spinlock_try_acquire(spinlock_t *lock) {
    if (__sync_lock_test_and_set(lock, 1) == 0) {
        __sync_synchronize();

        /* Record ownership for successful try-acquire */
        spinlock_register_owner(lock, (uint64_t)__builtin_return_address(0));
        return 1;
    }
    return 0;
}

/* IRQ-safe variant: save interrupt flag, disable, acquire */
static inline void spinlock_irqsave_acquire(spinlock_t *lock, uint64_t *flags) {
    __asm__ volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli\n\t"
        : "=r"(*flags)
        :
        : "memory"
    );
    spinlock_acquire(lock);
}

static inline void spinlock_irqsave_release(spinlock_t *lock, uint64_t flags) {
    spinlock_release(lock);
    if (!(flags & 0x200)) { /* IF bit */
        __asm__ volatile("sti");
    }
}

/* Spinlock with timeout (try for N iterations) */
static inline int spinlock_acquire_timeout(spinlock_t *lock, int max_iters) {
    int iter = 0;
    while (__sync_lock_test_and_set(lock, 1)) {
        __asm__ volatile("pause");
        iter++;
        if (iter >= max_iters) return -1; /* timeout */
    }
    __sync_synchronize();

    /* Record ownership on successful timed acquire */
    spinlock_register_owner(lock, (uint64_t)__builtin_return_address(0));
    return 0;
}

#endif
