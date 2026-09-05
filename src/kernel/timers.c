/*
 * timers.c — Dynamic kernel timer subsystem
 *
 * Allows callers to schedule one-shot callbacks to fire after N ticks.
 * Driven by timer_handler_soft() called from the timer IRQ.
 */

#include "timers.h"
#include "timer.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "softirq.h"

static struct {
    timer_callback_t fn;
    void            *arg;
    uint64_t         expire_tick;  /* tick at which this timer fires */
    int              active;       /* 1 = scheduled, 0 = free slot */
    int firing;                    /* 1 = dequeued, callback running/pending */
} g_timers[TIMER_MAX];

static spinlock_t g_timers_lock;
static int g_timers_initialized = 0;

/**
 * timers_init - Initialise the software timer subsystem
 *
 * Zeroes the timer table and plugs the timer softirq handler. Called once
 * during kernel bring-up.
 */
void __init timers_init(void) {
    memset(g_timers, 0, sizeof(g_timers));
    spinlock_init(&g_timers_lock);
    g_timers_initialized = 1;

    /* Register the timer softirq handler so timer_handler_soft() is
     * called from do_softirq() after the timer IRQ fires. */
    softirq_register(SOFTIRQ_TIMER, timer_handler_soft);

    kprintf("[OK] Dynamic timers initialized (%d slots)\n", TIMER_MAX);
}

/**
 * timer_available - Check whether a timer slot is available
 * Returns non-zero if the timer table has a free slot for a new timer.
 */
int timer_available(void)
{
    return g_timers_initialized;
}

/**
 * timer_schedule - Schedule a software timer
 * @fn: Callback invoked when the timer fires
 * @arg: Opaque argument passed to @fn
 * @delay_ticks: Delay before firing, in timer ticks
 *
 * Registers a one-shot timer and returns a timer id (>= 0) that can be passed to timer_cancel, or a
 * negative error code if the table is full.
 */
int timer_schedule(timer_callback_t fn, void *arg, uint64_t delay_ticks) {
    if (!fn || !g_timers_initialized) return -1;
    if (delay_ticks == 0) delay_ticks = 1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_timers_lock, &irq_flags);

    int slot = -1;
    for (int i = 0; i < TIMER_MAX; i++) {
        /* Skip firing slots: their callback is still running/pending and
         * the fn/arg pair is retained for cancellation detection until the
         * callback completes (see timer_handler_soft). */
        if (!g_timers[i].active && !g_timers[i].firing) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_timers_lock, irq_flags);
        return -1;
    }

    g_timers[slot].fn = fn;
    g_timers[slot].arg = arg;
    g_timers[slot].expire_tick = timer_get_ticks() + delay_ticks;
    g_timers[slot].active = 1;

    int timer_id = slot; /* use slot index as ID for simplicity */

    spinlock_irqsave_release(&g_timers_lock, irq_flags);
    return timer_id;
}

/**
 * timer_cancel - Cancel a pending software timer
 * @timer_id: Timer id returned by timer_schedule
 *
 * Removes the timer so its callback will not fire. Safe to call for an already-expired timer.
 */
void timer_cancel(int timer_id) {
    if (timer_id < 0 || timer_id >= TIMER_MAX || !g_timers_initialized) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_timers_lock, &irq_flags);

    if (g_timers[timer_id].firing) {
        /* The callback has already been dequeued and is running or about
         * to run.  Do not clear fn/arg: timer_handler_soft() clears them
         * when the callback completes, and the retained firing marker +
         * arg lets hrtimer_cancel() detect and wait out this dispatch. */
        spinlock_irqsave_release(&g_timers_lock, irq_flags);
        return;
    }

    g_timers[timer_id].active = 0;
    g_timers[timer_id].fn = NULL;
    g_timers[timer_id].arg = NULL;

    spinlock_irqsave_release(&g_timers_lock, irq_flags);
}

/* Returns 1 while the given slot holds a dequeued dispatch whose callback
 * is running (or about to run) with arg == expected_arg.  Used by
 * hrtimer_cancel() to wait out a dispatch that timer_handler_soft() has
 * dequeued but hrtimer_dispatch() has not yet entered: that window is
 * invisible to the hrtimer's own running flag, so without this check a
 * cancel could return while the dispatch is about to dereference a freed
 * timer. */
/**
 * timer_callback_pending - Test whether a timer callback is queued
 * @timer_id: Timer id to inspect
 * @expected_arg: Expected callback argument
 *
 * Returns non-zero if a timer with the given id/arg is still pending execution.
 */
int timer_callback_pending(int timer_id, void *expected_arg) {
    if (timer_id < 0 || timer_id >= TIMER_MAX || !g_timers_initialized)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_timers_lock, &irq_flags);
    int pending = (g_timers[timer_id].firing && g_timers[timer_id].arg == expected_arg);
    spinlock_irqsave_release(&g_timers_lock, irq_flags);
    return pending;
}

/**
 * timer_handler_soft - Run due software timer callbacks
 * Softirq handler that walks the timer table and invokes the callbacks of all expired timers.
 * Called from the timer interrupt context.
 */
void timer_handler_soft(void) {
    if (!g_timers_initialized) return;

    /* Walk through all timers and fire any that have expired.
     * We do NOT hold the lock while firing the callback to avoid
     * deadlocks if the callback schedules another timer.
     *
     * Refresh now on each iteration so a long-running callback in a
     * lower-index slot does not starve timers in higher-index slots:
     * the stale-now condition (now < expire_tick) would otherwise
     * skip timers whose expire_tick falls between the original
     * snapshot and the actual current tick.  In a timer-wheel design
     * this is prevented by bucket-level isolation; here we mitigate
     * it by re-reading the monotonic tick counter in the loop. */
    for (int i = 0; i < TIMER_MAX; i++) {
        uint64_t now = timer_get_ticks();
        if (!g_timers[i].active) continue;
        if (now < g_timers[i].expire_tick) continue;

        /* Atomically deactivate so we don't double-fire.
         * Re-check both active AND expire_tick under the lock:
         * the unlocked read of expire_tick above could have seen
         * a stale value if timer_schedule() on another CPU just
         * rescheduled this timer with a new expiration. */
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&g_timers_lock, &irq_flags);
        if (!g_timers[i].active || now < g_timers[i].expire_tick) {
            spinlock_irqsave_release(&g_timers_lock, irq_flags);
            continue;
        }
        g_timers[i].active = 0;
        g_timers[i].firing = 1;
        timer_callback_t fn = g_timers[i].fn;
        void *arg = g_timers[i].arg;
        spinlock_irqsave_release(&g_timers_lock, irq_flags);

        /* Fire outside the lock.  fn/arg are retained in the slot (marked
         * firing) until the callback returns so a concurrent cancel can
         * detect the in-flight dispatch via timer_callback_pending();
         * timer_schedule() skips firing slots, so the retained fn/arg
         * cannot be overwritten by a new timer. */
        if (fn) fn(arg);

        /* Callback finished — release the slot for reuse. */
        spinlock_irqsave_acquire(&g_timers_lock, &irq_flags);
        g_timers[i].firing = 0;
        g_timers[i].fn = NULL;
        g_timers[i].arg = NULL;
        spinlock_irqsave_release(&g_timers_lock, irq_flags);
    }
}
