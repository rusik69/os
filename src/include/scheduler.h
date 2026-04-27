#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"

void scheduler_init(void);
void schedule(void);
void scheduler_add(struct process *proc);
void scheduler_remove(struct process *proc);
void scheduler_yield(void);

extern void context_switch(struct cpu_context **old, struct cpu_context *new_ctx);

#endif
