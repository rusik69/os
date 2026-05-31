#ifndef COMPLETION_H
#define COMPLETION_H

/*
 * Completion variables — a lightweight one-shot wakeup primitive.
 *
 * Pattern:
 *   struct completion comp;
 *   completion_init(&comp);
 *   // Thread A: completion_wait(&comp);  // blocks until done
 *   // Thread B: completion_done(&comp);  // wakes A
 *
 * A completion can be done before anyone waits; the waiter returns
 * immediately.  After done, the completion stays "done" so late
 * waiters also return immediately (one-shot semantic).
 *
 * Built on top of wait_queue.
 */

#include "waitqueue.h"

struct completion {
    struct wait_queue wq;
    int done;             /* 1 = complete, stays 1 forever */
};

static inline void completion_init(struct completion *c) {
    wait_queue_init(&c->wq);
    c->done = 0;
}

/*
 * Wait for completion.  If already done, returns immediately.
 * Otherwise blocks until completion_done() is called.
 */
static inline void completion_wait(struct completion *c) {
    if (c->done) return;
    /* Keep sleeping until done */
    while (!c->done)
        wait_queue_sleep(&c->wq);
}

/*
 * Mark the completion as done and wake all waiters.
 * Safe to call multiple times; only the first does the wake.
 */
static inline void completion_done(struct completion *c) {
    if (c->done) return;
    c->done = 1;
    wait_queue_wake_all(&c->wq);
}

/*
 * Non-blocking check: is this completion done?
 */
static inline int completion_is_done(struct completion *c) {
    return c->done;
}

#endif /* COMPLETION_H */
