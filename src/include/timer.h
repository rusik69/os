#ifndef TIMER_H
#define TIMER_H

#include "types.h"

#define TIMER_FREQ 100

void timer_init(void);
uint64_t timer_get_ticks(void);

#endif
