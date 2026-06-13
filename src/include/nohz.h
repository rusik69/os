#ifndef NOHZ_H
#define NOHZ_H

#include "types.h"

/* Initialise adaptive tick infrastructure */
void nohz_init(void);

/* Mark a CPU as isolated (candidate for NO_HZ_FULL adaptive tick) */
int nohz_isolate_cpu(int cpu);

/* Stop the periodic tick on a given isolated CPU */
int nohz_tick_stop(int cpu);

/* Restart the periodic tick on a given CPU */
int nohz_tick_restart(int cpu);

/* Check whether the tick is stopped on a given CPU */
int nohz_tick_is_stopped(int cpu);

/* Return milliseconds since tick was stopped */
uint64_t nohz_tick_stopped_ms(int cpu);

/* Update last tick timestamp (called from timer interrupt) */
void nohz_tick_account(int cpu);

#endif /* NOHZ_H */
