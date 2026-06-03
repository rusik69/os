#ifndef COREDUMP_H
#define COREDUMP_H
#include "types.h"
#include "process.h"
void coredump_init(void);
void coredump_set_enabled(int en);
int coredump_generate(struct process *proc, int signo);

/* Deferred core dump worker — safe to call from any context.
 * Schedules a workqueue item that generates the dump in process context.
 * @arg: pointer to coredump_deferred_arg (PID + signal number). */
void coredump_deferred(void *arg);
#endif
