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

static struct {
    timer_callback_t fn;
    void            *arg;
    uint64_t         expire_tick;  /* tick at which this timer fires */
    int              active;       /* 1 = scheduled, 0 = free slot */
} g_timers[TIMER_MAX];

static spinlock_t g_timers_lock;
static int g_timers_initialized = 0;

void timers_init(void) {
    memset(g_timers, 0, sizeof(g_timers));
    spinlock_init(&g_timers_lock);
    g_timers_initialized = 1;
    kprintf("[OK] Dynamic timers initialized (%d slots)\n", TIMER_MAX);
}

int timer_available(void)
{
    return g_timers_initialized;
}

int timer_schedule(timer_callback_t fn, void *arg, uint64_t delay_ticks) {
    if (!fn || !g_timers_initialized) return -1;
    if (delay_ticks == 0) delay_ticks = 1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_timers_lock, &irq_flags);

    int slot = -1;
    for (int i = 0; i < TIMER_MAX; i++) {
        if (!g_timers[i].active) {
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

void timer_cancel(int timer_id) {
    if (timer_id < 0 || timer_id >= TIMER_MAX || !g_timers_initialized) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_timers_lock, &irq_flags);

    g_timers[timer_id].active = 0;
    g_timers[timer_id].fn = NULL;
    g_timers[timer_id].arg = NULL;

    spinlock_irqsave_release(&g_timers_lock, irq_flags);
}

void timer_handler_soft(void) {
    if (!g_timers_initialized) return;

    uint64_t now = timer_get_ticks();

    /* Walk through all timers and fire any that have expired.
     * We do NOT hold the lock while firing the callback to avoid
     * deadlocks if the callback schedules another timer. */
    for (int i = 0; i < TIMER_MAX; i++) {
        if (!g_timers[i].active) continue;
        if (now < g_timers[i].expire_tick) continue;

        /* Atomically deactivate so we don't double-fire */
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&g_timers_lock, &irq_flags);
        if (!g_timers[i].active) {
            spinlock_irqsave_release(&g_timers_lock, irq_flags);
            continue;
        }
        g_timers[i].active = 0;
        timer_callback_t fn = g_timers[i].fn;
        void *arg = g_timers[i].arg;
        g_timers[i].fn = NULL;
        g_timers[i].arg = NULL;
        spinlock_irqsave_release(&g_timers_lock, irq_flags);

        /* Fire outside the lock */
        if (fn) fn(arg);
    }
}

/* ── Stub: timer_add ─────────────────────────────── */
int timer_add(void *timer)
{
    (void)timer;
    kprintf("[timers] timer_add: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: timer_del ─────────────────────────────── */
int timer_del(void *timer)
{
    (void)timer;
    kprintf("[timers] timer_del: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: timer_mod ─────────────────────────────── */
int timer_mod(void *timer, uint64_t expires)
{
    (void)timer;
    (void)expires;
    kprintf("[timers] timer_mod: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: timer_init ─────────────────────────────── */
int timer_init(void *timer, void *func, unsigned long data)
{
    (void)timer;
    (void)func;
    (void)data;
    kprintf("[timers] timer_init: not yet implemented\n");
    return -ENOSYS;
}
