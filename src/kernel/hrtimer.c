/*
 * src/kernel/hrtimer.c — High-resolution timer implementation
 *
 * Thin wrapper over the existing dynamic timer subsystem.
 * Provides hrtimer_init/start/cancel API for kernel components.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "hrtimer.h"
#include "timers.h"

void hrtimer_init(struct hrtimer *timer, void (*function)(void *), void *data)
{
    if (!timer) return;
    timer->expires = 0;
    timer->function = function;
    timer->data = data;
    timer->state = 0;
}

int hrtimer_start(struct hrtimer *timer, uint64_t ns)
{
    if (!timer || !timer->function) return -1;
    if (!timer_available()) return -1;

    /* Convert nanoseconds to ticks (assuming ~1 GHz TSC, 1 tick ≈ 1 ns) */
    uint64_t delay_ticks = ns;
    if (delay_ticks < 1) delay_ticks = 1;

    int tid = timer_schedule(timer->function, timer->data, delay_ticks);
    if (tid < 0) return -1;

    timer->expires = ns;
    timer->state = 1;
    return 0;
}

int hrtimer_cancel(struct hrtimer *timer)
{
    if (!timer) return -1;
    timer->state = 0;
    return 0;
}

uint64_t hrtimer_get_remaining(struct hrtimer *timer)
{
    if (!timer) return 0;
    return timer->expires;
}

int hrtimer_active(struct hrtimer *timer)
{
    return timer && timer->state;
}
