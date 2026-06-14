#ifndef CORE_SCHED_H
#define CORE_SCHED_H

#include "types.h"

/* Forward declaration */
struct process;

/* Initialise core scheduling infrastructure */
void sched_core_init(void);

/* Return the sibling CPU mask for a given logical CPU */
uint64_t sched_core_siblings(int cpu);

/* Check whether two logical CPUs share the same physical core */
int sched_core_share(int cpu1, int cpu2);

/* Mark two CPUs as siblings */
int sched_core_set_sibling(int cpu1, int cpu2);

/* Return the number of CPUs per physical core */
int sched_core_cpus_per_core(void);

/* Check whether a task may run on a given CPU (core scheduling policy) */
int sched_core_allow(struct process *task, int target_cpu);

/* Set per-task core scheduling cookie (0 = no restriction) */
void sched_core_set_cookie(struct process *task, uint64_t cookie);

/* Get per-task core scheduling cookie */
uint64_t sched_core_get_cookie(struct process *task);

#endif /* CORE_SCHED_H */
