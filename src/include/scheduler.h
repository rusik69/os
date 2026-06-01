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
void scheduler_tick(int was_user);  /* called each timer tick; handles time-slice expiry */
void scheduler_age(void);   /* called periodically; boosts starved processes */
uint64_t scheduler_get_idle_ticks(void);

/* Scheduler statistics */
struct sched_stats {
    uint64_t context_switches;
    uint64_t preemptions;
    uint64_t yields;
    uint64_t idle_ticks_total;
};
void scheduler_get_stats(struct sched_stats *stats);
void scheduler_stats_inc_ctx_switch(void);
void scheduler_stats_inc_preempt(void);
void scheduler_stats_inc_yield(void);

/* Per-CPU runqueue statistics */
struct runqueue_stats {
    int nr_runnable;
    int nr_running;
    int nr_uninterruptible;
    int load_weight;
    int prio_distribution[SCHED_LEVELS];
};
void scheduler_get_runqueue_stats(int cpu, struct runqueue_stats *s);

extern void context_switch(struct cpu_context **old, struct cpu_context *new_ctx);

/* ── Autogroup support ──────────────────────────────────────── */
#define SCHED_AUTOGROUP_MAX 16
struct sched_autogroup {
    int id;
    uint64_t vruntime;       /* group vruntime */
    int member_count;
};

/* Get or create autogroup for the given session ID */
int sched_autogroup_get(int session_id);

/* Assign process to an autogroup */
void sched_autogroup_assign(struct process *proc, int group_id);

/* Find highest vruntime among group members */
uint64_t sched_autogroup_max_vruntime(int group_id);

#endif
