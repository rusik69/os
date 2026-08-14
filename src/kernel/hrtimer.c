/*
 * src/kernel/hrtimer.c — High-resolution timer implementation
 *
 * Thin wrapper over the existing dynamic timer subsystem.
 * Provides hrtimer_init/start/cancel API for kernel components.
 *
 * SMP safety: all struct hrtimer fields are protected by the
 * per-timer spinlock (hrtimer->lock). Callers may call these
 * functions from any context (process, softirq, timer callback).
 * Locking order: hrtimer->lock -> g_timers_lock (never reverse).
 *
 * Expiry handling: the underlying dynamic timer is registered with a
 * dispatch wrapper (hrtimer_dispatch) instead of the raw user
 * callback.  This gives the wrapper a chance to, under hrtimer->lock:
 *   - re-validate that the timer is still armed (hrtimer_cancel may
 *     have won the race against the softirq dequeue — the callback
 *     must not run after cancel returns),
 *   - invalidate timer->timer_id on expiry, so a later
 *     hrtimer_cancel()/hrtimer_start() can never cancel an unrelated
 *     timer that reused the freed slot (slot indices are recycled by
 *     timer_schedule()),
 *   - clear timer->state so hrtimer_active() reports the truth, and
 *   - track timer->running so hrtimer_cancel() can wait for an
 *     in-flight callback to finish before returning (the caller may
 *     then safely free the callback data).
 */

#define KERNEL_INTERNAL
#include "hrtimer.h"
#include "types.h"
#include "timers.h"

void hrtimer_init(struct hrtimer *timer, void (*function)(void *), void *data)
{
    if (!timer) return;
    spinlock_init(&timer->lock);
    timer->expires = 0;
    timer->function = function;
    timer->data = data;
    timer->state = 0;
    timer->timer_id = -1;
    timer->running = 0;
}

/* Dispatch wrapper invoked by the dynamic timer subsystem when the
 * underlying timer expires.  arg is the struct hrtimer * that was
 * passed to timer_schedule(). */
static void hrtimer_dispatch(void *arg)
{
    struct hrtimer *timer = (struct hrtimer *)arg;
    void (*function)(void *);
    void *data;
    uint64_t irq_flags;

    if (!timer) return;

    spinlock_irqsave_acquire(&timer->lock, &irq_flags);

    /* Mark the dispatch as running BEFORE the state check so that a
     * dispatch already dequeued by the timer softirq is fully covered
     * by hrtimer_cancel()'s completion wait: cancel() waits for
     * running to drop to 0, which now also covers the state == 0
     * early-exit path below. */
    timer->running = 1;

    /* If the timer was canceled (or never armed), do not invoke the
     * user callback.  timer_handler_soft() dequeues the slot before
     * calling us, so this is the only fence between hrtimer_cancel()
     * returning and the callback running. */
    if (timer->state == 0) {
        timer->running = 0;
        spinlock_irqsave_release(&timer->lock, irq_flags);
        return;
    }

    /* The underlying slot has fired and is now free.  Drop the stale
     * id so a later cancel/start on this hrtimer cannot cancel an
     * unrelated timer that reused the slot. */
    timer->timer_id = -1;
    timer->state = 0;

    function = timer->function;
    data = timer->data;

    /* running was set to 1 above while holding the lock, so a
     * concurrent hrtimer_cancel() will wait for the callback to
     * complete before returning. */
    spinlock_irqsave_release(&timer->lock, irq_flags);

    if (function)
        function(data);

    spinlock_irqsave_acquire(&timer->lock, &irq_flags);
    timer->running = 0;
    spinlock_irqsave_release(&timer->lock, irq_flags);
}

int hrtimer_start(struct hrtimer *timer, uint64_t ns)
{
    if (!timer || !timer->function) return -1;
    if (!timer_available()) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&timer->lock, &irq_flags);

    /* Cancel any previously-scheduled underlying timer first.
     * timer->timer_id is only non-negative while the underlying timer
     * is genuinely pending: hrtimer_dispatch() resets it to -1 on
     * expiry, so this can never cancel an unrelated timer that
     * recycled the slot. */
    if (timer->timer_id >= 0) {
        timer_cancel(timer->timer_id);
        timer->timer_id = -1;
    }

    /* Convert nanoseconds to ticks.
     * PIT runs at 100 Hz → NS_PER_TICK = 10,000,000.
     * Divide and round up so even tiny ns values yield at least 1 tick. */
    uint64_t delay_ticks = (ns + NS_PER_TICK - 1) / NS_PER_TICK;
    if (delay_ticks < 1) delay_ticks = 1;

    int tid = timer_schedule(hrtimer_dispatch, timer, delay_ticks);
    if (tid < 0) {
        spinlock_irqsave_release(&timer->lock, irq_flags);
        return -1;
    }

    timer->timer_id = tid;
    timer->expires = ns;
    timer->state = 1;

    spinlock_irqsave_release(&timer->lock, irq_flags);
    return 0;
}

int hrtimer_cancel(struct hrtimer *timer)
{
    if (!timer) return -1;

    uint64_t irq_flags;
    int slot = -1;

    spinlock_irqsave_acquire(&timer->lock, &irq_flags);

    if (timer->timer_id >= 0) {
        slot = timer->timer_id;
        timer_cancel(slot);
        timer->timer_id = -1;
    }
    timer->state = 0;

    spinlock_irqsave_release(&timer->lock, irq_flags);

    /* Wait for any callback already dequeued by the timer softirq to
     * finish.  hrtimer_dispatch() re-checks state under the lock, so
     * a callback that has not started yet will see state == 0 and
     * return without invoking the user function — after this loop the
     * caller can safely free the callback data.
     *
     * A dispatch dequeued by timer_handler_soft() (slot marked firing)
     * but not yet entered into hrtimer_dispatch() is invisible to the
     * running flag, so also wait it out via timer_callback_pending().
     * This closes the dequeue-to-entry window where a cancel could
     * otherwise return while the dispatch is about to dereference the
     * (freed) timer. */
    while (timer->running || timer_callback_pending(slot, timer)) {
        __asm__ volatile("pause");
    }

    /* The callback may have re-armed the timer (hrtimer_start() from
     * within the callback, e.g. periodic timers) after we cleared
     * state above.  That arming is still live and would invoke the
     * callback again after cancel returns — a use-after-free of the
     * callback data if the caller frees it now.  Re-check under the
     * lock and cancel any timer re-armed while the callback was
     * running (waiting out any dispatch dequeued for it), so that
     * after hrtimer_cancel() returns the callback can never fire
     * again. */
    for (;;) {
        spinlock_irqsave_acquire(&timer->lock, &irq_flags);
        if (timer->timer_id < 0) {
            spinlock_irqsave_release(&timer->lock, irq_flags);
            break;
        }
        slot = timer->timer_id;
        timer_cancel(slot);
        timer->timer_id = -1;
        timer->state = 0;
        spinlock_irqsave_release(&timer->lock, irq_flags);

        while (timer->running || timer_callback_pending(slot, timer)) {
            __asm__ volatile("pause");
        }
    }

    return 0;
}

uint64_t hrtimer_get_remaining(struct hrtimer *timer)
{
    if (!timer) return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&timer->lock, &irq_flags);
    uint64_t rem = timer->expires;
    spinlock_irqsave_release(&timer->lock, irq_flags);
    return rem;
}

int hrtimer_active(struct hrtimer *timer)
{
    if (!timer) return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&timer->lock, &irq_flags);
    int active = timer->state;
    spinlock_irqsave_release(&timer->lock, irq_flags);
    return active;
}

/* ── Stub: hrtimer_forward ─────────────────────────────────────────── */
static uint64_t hrtimer_forward(struct hrtimer *timer, uint64_t now, uint64_t interval)
{
    (void)timer; (void)now; (void)interval;
    kprintf("[HRTIMER] hrtimer_forward: not yet implemented\n");
    return 0;
}

/* ── Stub: hrtimer_nanosleep ───────────────────────────────────────── */
static int hrtimer_nanosleep(uint64_t ns)
{
    (void)ns;
    kprintf("[HRTIMER] hrtimer_nanosleep: not yet implemented\n");
    return 0;
}
