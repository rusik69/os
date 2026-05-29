#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"

/* 4-level multilevel priority queue: 0 = highest, 3 = lowest */
#define SCHED_LEVELS 4

void scheduler_init(void);
void schedule(void);
void scheduler_add(struct process *proc);
void scheduler_remove(struct process *proc);
int scheduler_set_priority(struct process *proc, uint8_t priority);
void scheduler_yield(void);
void scheduler_wake_sleepers(void);
void scheduler_tick(void);  /* called each timer tick; handles time-slice expiry */
void scheduler_age(void);   /* called periodically; boosts starved processes */
uint64_t scheduler_get_idle_ticks(void);

extern void context_switch(struct cpu_context **old, struct cpu_context *new_ctx);

#endif
