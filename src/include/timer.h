#ifndef TIMER_H
#define TIMER_H

#include "types.h"

#define TIMER_FREQ 100
#define NS_PER_TICK (1000000000ULL / TIMER_FREQ)  /* 10,000,000 ns per tick */

void timer_init(void);
uint64_t timer_get_ticks(void);
uint64_t timer_get_ns(void);  /* approximate: ticks * NS_PER_TICK */

#endif
