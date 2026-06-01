#ifndef TIMER_SOURCE_H
#define TIMER_SOURCE_H

#include "types.h"

/* Clocksource – free-running counter abstraction. */
struct clocksource {
    const char *name;
    uint64_t    (*read)(void);          /* read current counter value */
    uint64_t    freq_hz;                /* frequency in Hz */
    uint64_t    mask;                   /* bits of valid counter data */
    int         rating;                 /* higher = preferred */
};

/* Clockevent – one-shot or periodic event timer. */
struct clockevent {
    const char *name;
    int         (*set_next_event)(uint64_t cycles); /* arm next event */
    void        (*set_periodic)(void);               /* switch to periodic */
    void        (*stop)(void);                       /* stop the timer */
    uint64_t    freq_hz;
    int         rating;
};

/* Current clocksource and clockevent (set by arch code). */
extern struct clocksource *current_clocksource;
extern struct clockevent  *current_clockevent;

/* Read current time in nanoseconds via the current clocksource. */
uint64_t clocksource_read_ns(void);

/* Convert counter delta to nanoseconds. */
uint64_t clocksource_cyc2ns(uint64_t cycles, uint64_t freq_hz);

/* Register a clocksource (returns index or negative errno). */
int clocksource_register(struct clocksource *cs);

/* Register a clockevent (returns index or negative errno). */
int clockevent_register(struct clockevent *ce);

/* Select the best clocksource/clockevent by rating. */
void clocksource_select_best(void);
void clockevent_select_best(void);

void timer_source_init(void);

#endif /* TIMER_SOURCE_H */
