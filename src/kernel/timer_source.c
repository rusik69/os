#include "timer_source.h"
#include "printf.h"
#include "kernel.h"
#include "errno.h"
#include "string.h"

/* Globally registered clocksources and clockevents (simple fixed arrays). */
#define MAX_CLOCKSOURCES 8
#define MAX_CLOCKEVENTS  8
static int                __read_mostly clocksource_count;
static struct clocksource *clocksource_list[MAX_CLOCKSOURCES];
static int                 __read_mostly clockevent_count;

static struct clockevent  *clockevent_list[MAX_CLOCKEVENTS];

struct clocksource *current_clocksource;
struct clockevent  *current_clockevent;

uint64_t clocksource_read_ns(void)
{
    if (!current_clocksource)
        return 0;
    uint64_t cycles = current_clocksource->read();
    return clocksource_cyc2ns(cycles, current_clocksource->freq_hz);
}

uint64_t clocksource_cyc2ns(uint64_t cycles, uint64_t freq_hz)
{
    if (freq_hz == 0)
        return 0;
    /*
     * Use (cycles * 1000000000) / freq_hz with a guard against overflow.
     * For large cycle values this may wrap; in practice the caller
     * should provide a delta or the freq_hz is chosen to avoid overflow.
     */
    uint64_t ns;
    __asm__ __volatile__(
        "mulq %[freq]; divq %[hz]"
        : "=a"(ns)
        : "a"(cycles), [freq]"r"(1000000000ULL), [hz]"r"(freq_hz)
        : "rdx"
    );
    return ns;
}

int clocksource_register(struct clocksource *cs)
{
    if (clocksource_count >= MAX_CLOCKSOURCES)
        return -ENOMEM;
    if (!cs || !cs->read || cs->freq_hz == 0)
        return -EINVAL;

    clocksource_list[clocksource_count++] = cs;

    /* Select this as current if it's the first or has a better rating */
    if (!current_clocksource || cs->rating > current_clocksource->rating)
        current_clocksource = cs;

    return clocksource_count - 1;
}

int clockevent_register(struct clockevent *ce)
{
    if (clockevent_count >= MAX_CLOCKEVENTS)
        return -ENOMEM;
    if (!ce || !ce->set_next_event || ce->freq_hz == 0)
        return -EINVAL;

    clockevent_list[clockevent_count++] = ce;

    if (!current_clockevent || ce->rating > current_clockevent->rating)
        current_clockevent = ce;

    return clockevent_count - 1;
}

void clocksource_select_best(void)
{
    int i;
    struct clocksource *best = NULL;

    for (i = 0; i < clocksource_count; i++) {
        if (!best || clocksource_list[i]->rating > best->rating)
            best = clocksource_list[i];
    }
    if (best)
        current_clocksource = best;
}

void clockevent_select_best(void)
{
    int i;
    struct clockevent *best = NULL;

    for (i = 0; i < clockevent_count; i++) {
        if (!best || clockevent_list[i]->rating > best->rating)
            best = clockevent_list[i];
    }
    if (best)
        current_clockevent = best;
}

void timer_source_init(void)
{
    clocksource_count = 0;
    clockevent_count = 0;
    current_clocksource = NULL;
    current_clockevent = NULL;

    kprintf("[OK] timer_source: Clocksource/clockevent abstraction initialised\n");
}

/* ── timer_source_read: Read current clocksource counter ──────────────── */
uint64_t timer_source_read(void)
{
    if (!current_clocksource || !current_clocksource->read) {
        kprintf("[timer] timer_source_read: no clocksource available\n");
        return 0;
    }
    return current_clocksource->read();
}
/* ── timer_source_get_freq: Get current clocksource frequency in Hz ──── */
uint64_t timer_source_get_freq(void)
{
    if (!current_clocksource) {
        kprintf("[timer] timer_source_get_freq: no clocksource available\n");
        return 0;
    }
    return current_clocksource->freq_hz;
}
