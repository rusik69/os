#ifndef WAITQUEUE_H
#define WAITQUEUE_H

#include "types.h"

/* wait_event: atomically check condition and sleep until woken */
#define wait_event(wq, condition) do { \
    while (!(condition)) { \
        wait_queue_sleep(&(wq)); \
    } \
} while(0)

#define wait_event_timeout(wq, condition, timeout_ticks) ({ \
    int __ret = 0; \
    uint64_t __start = 0; \
    extern uint64_t timer_get_ticks(void); \
    __start = timer_get_ticks(); \
    while (!(condition)) { \
        if (timeout_ticks > 0 && (timer_get_ticks() - __start) >= (timeout_ticks)) { \
            __ret = -1; \
            break; \
        } \
        wait_queue_sleep(&(wq)); \
    } \
    __ret; \
})
#include "spinlock.h"
#include "process.h"

/*
 * Wait queues — generic blocking primitive.
 *
 * A process call wait_queue_sleep(wq) to block until another thread
 * calls wait_queue_wake(wq).  The sleeping process yields the CPU;
 * on wake it is re-queued in the scheduler's ready queue.
 *
 * wait_queue_wake(wq) wakes the first waiting process (FIFO order).
 * wait_queue_wake_all(wq) wakes every waiting process.
 *
 * All operations are spinlock-protected and IRQ-safe.
 */

#define WAITQUEUE_MAX_WAITERS 16

struct wait_queue {
    spinlock_t lock;
    int head;                        /* index of oldest waiter */
    int count;                       /* number of active waiters */
    uint32_t pids[WAITQUEUE_MAX_WAITERS];  /* 0 = empty slot */
} __cacheline_aligned;

/* Static initializer */
#define WAITQUEUE_INIT { .lock = SPINLOCK_INIT, .head = 0, .count = 0, .pids = {0} }

static inline void wait_queue_init(struct wait_queue *wq) {
    spinlock_init(&wq->lock);
    wq->head  = 0;
    wq->count = 0;
    for (int i = 0; i < WAITQUEUE_MAX_WAITERS; i++)
        wq->pids[i] = 0;
}

/*
 * Sleep on the wait queue.
 * The current process is marked BLOCKED and stored in the queue.
 * Re-evaluates after wake: if a spurious wake happens, loops.
 *
 * Returns 0 normally.  Returns -1 if the queue is full.
 */
int wait_queue_sleep(struct wait_queue *wq);

/*
 * Interruptible sleep on the wait queue.
 * Like wait_queue_sleep, but returns -EINTR (-4) if a signal is
 * pending before sleeping or when woken.  Use this for syscalls
 * that must be interruptible by signals (pause, sigsuspend, etc.).
 *
 * Returns 0 if woken normally, -4 (-EINTR) if interrupted by signal.
 */
int wait_queue_sleep_interruptible(struct wait_queue *wq);

/*
 * Sleep with timeout on the wait queue.
 * Blocks for at most 'ticks' timer ticks before returning -ETIME (-62).
 * Returns 0 if woken normally, -62 (-ETIME) on timeout, -1 on queue full.
 */
int wait_queue_sleep_timeout(struct wait_queue *wq, uint64_t ticks);

/* Lazy init: ensure wait queue lock is initialized (safe to call on
 * already-initialized queues).  Called from pipe/fifo tight loops. */
void wait_queue_ensure(struct wait_queue *wq);

/*
 * Interruptible sleep with timeout.
 * Returns 0 if woken normally, -4 (-EINTR) if signal, -62 (-ETIME) on timeout.
 */
int wait_queue_sleep_interruptible_timeout(struct wait_queue *wq, uint64_t ticks);

/*
 * Wake the oldest waiter (FIFO).  The woken process is moved to READY.
 * Returns 1 if a process was woken, 0 if queue was empty.
 */
int wait_queue_wake(struct wait_queue *wq);

/*
 * Wake all waiters.
 * Returns the number of processes woken.
 */
int wait_queue_wake_all(struct wait_queue *wq);

/*
 * Wake a specific PID from the queue (used for signal wakeup).
 * Returns 1 if found and woken, 0 otherwise.
 */
int wait_queue_wake_pid(struct wait_queue *wq, uint32_t pid);

/*
 * Return true (1) if the queue has any waiters.
 */
static inline int wait_queue_has_waiters(struct wait_queue *wq) {
    return wq->count > 0;
}

#endif /* WAITQUEUE_H */
