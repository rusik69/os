#ifndef TIMERS_H
#define TIMERS_H

#include "types.h"

#define TIMER_MAX 32

/* Timer callback type */
typedef void (*timer_callback_t)(void *arg);

/* Schedule a one-shot timer. Returns timer_id (>=0) or -1 on failure. */
int timer_schedule(timer_callback_t fn, void *arg, uint64_t delay_ticks);

/* Cancel a pending timer by its ID */
void timer_cancel(int timer_id);

/* Called from the timer IRQ (src/drivers/timer.c) — drives the timer subsystem */
void timer_handler_soft(void);

/* Initialize the dynamic timer subsystem */
void timers_init(void);

/* Returns 1 if timers are available, 0 before timers_init() */
int timer_available(void);

#endif /* TIMERS_H */
