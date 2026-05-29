#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"
#include "io.h"  /* for pause/rep nop */

typedef volatile int spinlock_t;

#define SPINLOCK_INIT 0

static inline void spinlock_init(spinlock_t *lock) {
    *lock = 0;
}

static inline void spinlock_acquire(spinlock_t *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        /* Wait (PAUSE instruction for hyper-threading efficiency) */
        __asm__ volatile("pause");
    }
    __sync_synchronize(); /* full memory barrier */
}

static inline void spinlock_release(spinlock_t *lock) {
    __sync_synchronize();
    __sync_lock_release(lock);
}

static inline int spinlock_try_acquire(spinlock_t *lock) {
    return __sync_lock_test_and_set(lock, 1) == 0;
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

#endif
