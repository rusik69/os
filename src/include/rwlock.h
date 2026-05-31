#ifndef RWLOCK_H
#define RWLOCK_H

/*
 * Read-Write lock (fair, writer-preference).
 *
 * Multiple readers can hold the lock simultaneously.
 * Writers get exclusive access.  When a writer is waiting,
 * new readers queue behind it to prevent writer starvation.
 *
 * All operations are IRQ-safe: interrupts are disabled during
 * manipulation and restored to previous state.
 */

#include "types.h"
#include "spinlock.h"
#include "waitqueue.h"

typedef struct {
    spinlock_t lock;         /* protects internal state */
    struct wait_queue readers_wq;  /* readers wait here */
    struct wait_queue writers_wq;  /* writers wait here */
    int readers;             /* number of active readers */
    int writer_active;       /* 1 if a writer holds the lock */
    int writers_waiting;     /* number of queued writers */
} rwlock_t;

#define RWLOCK_INIT { \
    .lock = SPINLOCK_INIT, \
    .readers_wq = WAITQUEUE_INIT, \
    .writers_wq = WAITQUEUE_INIT, \
    .readers = 0, \
    .writer_active = 0, \
    .writers_waiting = 0 \
}

static inline void rwlock_init(rwlock_t *rw) {
    spinlock_init(&rw->lock);
    wait_queue_init(&rw->readers_wq);
    wait_queue_init(&rw->writers_wq);
    rw->readers = 0;
    rw->writer_active = 0;
    rw->writers_waiting = 0;
}

/*
 * Acquire read lock.
 * Blocks if a writer is active or waiting (writer-preference).
 */
static inline void rwlock_rdlock(rwlock_t *rw) {
    uint64_t flags;
    spinlock_irqsave_acquire(&rw->lock, &flags);

    while (rw->writer_active || rw->writers_waiting > 0) {
        /* Must sleep — release spinlock, block on readers_wq */
        spinlock_release(&rw->lock);
        wait_queue_sleep(&rw->readers_wq);
        spinlock_irqsave_acquire(&rw->lock, &flags);
    }

    rw->readers++;
    spinlock_irqsave_release(&rw->lock, flags);
}

/*
 * Acquire write lock (exclusive).
 * Blocks until no readers and no other writer.
 */
static inline void rwlock_wrlock(rwlock_t *rw) {
    uint64_t flags;
    spinlock_irqsave_acquire(&rw->lock, &flags);

    rw->writers_waiting++;

    while (rw->readers > 0 || rw->writer_active) {
        spinlock_release(&rw->lock);
        wait_queue_sleep(&rw->writers_wq);
        spinlock_irqsave_acquire(&rw->lock, &flags);
    }

    rw->writers_waiting--;
    rw->writer_active = 1;
    spinlock_irqsave_release(&rw->lock, flags);
}

/*
 * Release read lock.
 * If last reader, wakes a waiting writer.
 */
static inline void rwlock_runlock(rwlock_t *rw) {
    uint64_t flags;
    spinlock_irqsave_acquire(&rw->lock, &flags);

    if (rw->readers > 0)
        rw->readers--;

    if (rw->readers == 0 && rw->writers_waiting > 0) {
        /* Wake one writer */
        wait_queue_wake(&rw->writers_wq);
    }

    spinlock_irqsave_release(&rw->lock, flags);
}

/*
 * Release write lock.
 * Wake waiting readers (if no more writers queued) or a writer.
 */
static inline void rwlock_wrunlock(rwlock_t *rw) {
    uint64_t flags;
    spinlock_irqsave_acquire(&rw->lock, &flags);

    rw->writer_active = 0;

    if (rw->writers_waiting > 0) {
        /* Writer preference: wake a waiting writer */
        wait_queue_wake(&rw->writers_wq);
    } else {
        /* Wake all waiting readers */
        wait_queue_wake_all(&rw->readers_wq);
    }

    spinlock_irqsave_release(&rw->lock, flags);
}

#endif /* RWLOCK_H */
