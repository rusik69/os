#ifndef PERCPU_COUNTER_H
#define PERCPU_COUNTER_H

#include "types.h"
#include "spinlock.h"

/*
 * Per-CPU counter for scalable statistics.
 *
 * A global 'count' is kept alongside per-CPU batch counters.
 * Most updates hit the local per-CPU cache; the global count is
 * updated when a batch overflows.  Summation reads the global count
 * plus all per-CPU deltas.
 *
 * This implementation uses a fixed number of per-CPU slots
 * (SMP_MAX_CPUS, defined in smp.h) for simplicity in the
 * freestanding environment.
 */

struct percpu_counter {
    int64_t         count;       /* approximate global count */
    int64_t        *percpu;      /* array of per-CPU deltas, allocated at init */
    spinlock_t      lock;        /* serialises global count updates */
    int             batch;       /* batch size (default 32) */
    int             num_cpus;    /* number of per-CPU slots */
};

/*
 * percpu_counter_init  - Initialise a percpu counter.
 * Returns 0 on success, -ENOMEM if allocation fails.
 */
int percpu_counter_init(struct percpu_counter *fbc, int64_t value, int batch);

/*
 * percpu_counter_add  - Add 'amount' to the counter (scalable).
 */
void percpu_counter_add(struct percpu_counter *fbc, int64_t amount);

/*
 * percpu_counter_sum  - Return the precise sum (reads all per-CPU deltas).
 * This is accurate but may be expensive.
 */
int64_t percpu_counter_sum(struct percpu_counter *fbc);

/*
 * percpu_counter_read  - Fast read of the approximate global count.
 */
static inline int64_t percpu_counter_read(struct percpu_counter *fbc)
{
    return fbc->count;
}

/*
 * percpu_counter_set  - Set the counter to an exact value.
 */
void percpu_counter_set(struct percpu_counter *fbc, int64_t value);

/*
 * percpu_counter_destroy  - Release per-CPU memory.
 */
void percpu_counter_destroy(struct percpu_counter *fbc);

void percpu_counter_init_global(void);

#endif /* PERCPU_COUNTER_H */
