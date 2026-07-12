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
}

int hrtimer_start(struct hrtimer *timer, uint64_t ns)
{
    if (!timer || !timer->function) return -1;
    if (!timer_available()) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&timer->lock, &irq_flags);

    /* Cancel any previously-scheduled underlying timer first */
    if (timer->timer_id >= 0) {
        timer_cancel(timer->timer_id);
        timer->timer_id = -1;
    }

    /* Convert nanoseconds to ticks.
     * PIT runs at 100 Hz → NS_PER_TICK = 10,000,000.
     * Divide and round up so even tiny ns values yield at least 1 tick. */
    uint64_t delay_ticks = (ns + NS_PER_TICK - 1) / NS_PER_TICK;
    if (delay_ticks < 1) delay_ticks = 1;

    int tid = timer_schedule(timer->function, timer->data, delay_ticks);
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
    spinlock_irqsave_acquire(&timer->lock, &irq_flags);

    if (timer->timer_id >= 0) {
        timer_cancel(timer->timer_id);
        timer->timer_id = -1;
    }
    timer->state = 0;

    spinlock_irqsave_release(&timer->lock, irq_flags);
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
