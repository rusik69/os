#ifndef HUNG_TASK_H
#define HUNG_TASK_H

#include "types.h"

/* Set the hung task timeout in seconds (0 = disabled) */
void hung_task_set_timeout(int seconds);

/* Enable/disable panic on hung task detection */
void hung_task_set_panic(int enable);

/* Periodic check — call from timer tick */
void hung_task_check(void);

/* Get statistics */
void hung_task_get_stats(uint64_t *timeout_ticks, int *panic_mode, int *check_count);

/* Initialize hung task detection */
void hung_task_init(void);

#endif /* HUNG_TASK_H */
