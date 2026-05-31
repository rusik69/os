#ifndef WORKQUEUE_H
#define WORKQUEUE_H

#include "types.h"

#define WORKQUEUE_MAX 16

/* Work item callback */
typedef void (*work_fn_t)(void *arg);

/* Schedule a work item to run in the workqueue kthread. Returns work_id (>=0) or -1. */
int workqueue_schedule(work_fn_t fn, void *arg);

/* Wait for all pending work items to complete */
void workqueue_drain(void);

/* Initialize the workqueue subsystem */
void workqueue_init(void);

#endif /* WORKQUEUE_H */
