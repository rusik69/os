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
#include "smp.h"

/* Per-CPU: the hrtimer whose user callback is currently executing on
 * this CPU (set/cleared by hrtimer_dispatch()).  Lets hrtimer_start()
 * recognize an in-callback re-arm: such a re-arm must not wait on
 * timer->running — that flag only clears when the dispatch returns,
 * i.e. after the callback itself returns, so waiting on it from inside
 * the callback would self-deadlock the callback's CPU. */
static struct hrtimer *volatile g_cb_timer[SMP_MAX_CPUS];

/* Current CPU id, clamped to the per-CPU array bounds. */
static inline int hrtimer_cur_cpu(void)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        cpu = 0;
    return cpu;
}

/**
 * hrtimer_init - Initialise a high-resolution timer
 * @timer: hrtimer structure to initialise
 * @function: Callback invoked on expiry
 * @data: Opaque argument passed to @function
 *
 * Prepares @timer for use with the given expiry callback.
 */
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
     * early-exit path below.  Record the dispatching CPU so
     * hrtimer_start() can recognize an in-callback re-arm. */
    timer->running = 1;
    int cpu = hrtimer_cur_cpu();
    g_cb_timer[cpu] = timer;

    /* If the timer was canceled (or never armed), do not invoke the
     * user callback.  timer_handler_soft() dequeues the slot before
     * calling us, so this is the only fence between hrtimer_cancel()
     * returning and the callback running. */
    if (timer->state == 0) {
        timer->running = 0;
        g_cb_timer[cpu] = NULL;
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
    g_cb_timer[cpu] = NULL;
    spinlock_irqsave_release(&timer->lock, irq_flags);
}

/**
 * hrtimer_start - Start a high-resolution timer
 * @timer: hrtimer to arm
 * @ns: Expiry delay in nanoseconds
 *
 * Arms @timer to fire its callback after @ns nanoseconds. Returns 0 on success or a negative error
 * code.
 */
int hrtimer_start(struct hrtimer *timer, uint64_t ns)
{
    if (!timer || !timer->function) return -1;
    if (!timer_available()) return -1;

    uint64_t irq_flags;
    int slot;

    /* Cancel any previously-scheduled underlying timer first, waiting
     * out any dispatch already dequeued for it before re-arming.
     *
     * timer->timer_id is only non-negative while the underlying timer
     * is genuinely pending: hrtimer_dispatch() resets it to -1 on
     * expiry, so this can never cancel an unrelated timer that
     * recycled the slot.
     *
     * The drain (the same fence hrtimer_cancel() uses) closes the
     * reprogram-vs-dispatch race: without it, a dispatch dequeued by
     * timer_handler_soft() just before the reprogram (slot marked
     * firing) runs after we re-arm, sees state == 1, invokes the
     * callback for the old expiry, and resets timer_id/state —
     * silently cancelling the new arming (the reprogrammed expiry
     * never fires) and leaving the new slot scheduled as a phantom
     * dispatch that dereferences the timer after the owner freed it.
     * state is cleared before the wait, so a stale dispatch
     * early-exits without invoking the callback.  In-callback re-arms
     * never wait on running: timer_id is already -1 by the time
     * hrtimer_dispatch() calls the user function, so the periodic
     * re-arm pattern schedules directly (an in-callback re-arm only
     * waits out a dispatch dequeued for a slot it just canceled, and
     * only on the rare path where a concurrent reprogram armed the
     * timer mid-callback). */
    for (;;) {
        spinlock_irqsave_acquire(&timer->lock, &irq_flags);

        if (timer->timer_id >= 0) {
            slot = timer->timer_id;
            timer_cancel(slot);
            timer->timer_id = -1;
            timer->state = 0;
        } else {
            slot = -1;
        }

        spinlock_irqsave_release(&timer->lock, irq_flags);

        /* In-callback re-arm (the callback of THIS timer re-arming
         * itself, e.g. the periodic vblank pattern): never wait on
         * timer->running — that flag is only cleared when
         * hrtimer_dispatch() returns, which requires the callback to
         * return first, so waiting would self-deadlock.  The only
         * thing to wait out is a dispatch already dequeued for the
         * slot we just canceled (pending); it sees state == 0 and
         * early-exits without invoking the callback. */
        if (g_cb_timer[hrtimer_cur_cpu()] == timer) {
            while (timer_callback_pending(slot, timer))
                __asm__ volatile("pause");
            break;
        }

        if (slot < 0 && !timer->running)
            break;

        if (!timer->running && !timer_callback_pending(slot, timer))
            break;

        /* A dispatch for the canceled slot is running or about to
         * run.  It sees state == 0 and returns without invoking the
         * callback; the loop re-checks in case the in-flight callback
         * re-armed the timer while we waited.
         *
         * This wait also covers slot < 0 with running == 1 — the
         * dispatch already cleared timer_id and its callback is still
         * executing.  Waiting for it to finish BEFORE re-arming closes
         * the reprogram-vs-callback race: if we re-armed while the
         * callback was still running, the callback's own re-arm would
         * cancel our fresh slot and then spin on running == 1 (set by
         * its own dispatch), deadlocking the callback's CPU. */
        while (timer->running || timer_callback_pending(slot, timer)) {
            __asm__ volatile("pause");
        }
    }

    /* Convert nanoseconds to ticks.
     * PIT runs at 100 Hz → NS_PER_TICK = 10,000,000.
     * Divide and round up so even tiny ns values yield at least 1 tick. */
    uint64_t delay_ticks = (ns + NS_PER_TICK - 1) / NS_PER_TICK;
    if (delay_ticks < 1) delay_ticks = 1;

retry_arm:
    spinlock_irqsave_acquire(&timer->lock, &irq_flags);

    /* Re-check under the lock before arming: a concurrent
     * hrtimer_start() (external reprogram, or an IRQ nested over our
     * own callback) may have armed the timer while we were waiting
     * out the old dispatch above.  If we ignored it and scheduled a
     * second slot, the other slot stays live in the timer table,
     * fires the callback at the wrong time, clobbers timer_id/state
     * on expiry, and silently swallows the expiry we arm here (missed
     * expiry); it then remains armed as a phantom dispatch.  Cancel
     * the other arming, wait out any dispatch dequeued for it, and
     * retry. */
    if (timer->timer_id >= 0) {
        slot = timer->timer_id;
        timer_cancel(slot);
        timer->timer_id = -1;
        timer->state = 0;
        spinlock_irqsave_release(&timer->lock, irq_flags);

        if (g_cb_timer[hrtimer_cur_cpu()] == timer) {
            /* In-callback path: never wait on timer->running — that
             * flag only clears when our own dispatch returns, so
             * waiting would self-deadlock.  Only a dispatch already
             * dequeued for the canceled slot can still run. */
            while (timer_callback_pending(slot, timer))
                __asm__ volatile("pause");
        } else {
            while (timer->running || timer_callback_pending(slot, timer))
                __asm__ volatile("pause");
        }
        goto retry_arm;
    }

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

/**
 * hrtimer_cancel - Cancel and deactivate an hrtimer
 * @timer: hrtimer to cancel
 *
 * Deactivates @timer so its callback will not fire and waits for any in-flight expiry to complete.
 * Returns 0 on success.
 */
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

/**
 * hrtimer_get_remaining - Return the time left on an hrtimer
 * @timer: hrtimer to query
 *
 * Returns the remaining nanoseconds until @timer expires, or 0 if it is not active.
 */
uint64_t hrtimer_get_remaining(struct hrtimer *timer)
{
    if (!timer) return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&timer->lock, &irq_flags);
    uint64_t rem = timer->expires;
    spinlock_irqsave_release(&timer->lock, irq_flags);
    return rem;
}

/**
 * hrtimer_active - Test whether an hrtimer is armed
 * @timer: hrtimer to query
 *
 * Returns non-zero if @timer is currently scheduled and pending.
 */
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
