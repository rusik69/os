#ifndef TSC_DEADLINE_H
#define TSC_DEADLINE_H

#include "types.h"

/* Initialize TSC deadline timer mode on the local APIC */
int tsc_deadline_init(void);

/* Set a TSC deadline (one-shot). Interrupt fires when TSC >= deadline. */
void tsc_deadline_set(uint64_t deadline);

/* Read the current TSC deadline */
uint64_t tsc_deadline_get(void);

#endif /* TSC_DEADLINE_H */
